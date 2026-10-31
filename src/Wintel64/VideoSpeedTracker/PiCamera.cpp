// Raspberry Pi cameras through rpicam-vid; see PiCamera.h for the source-name syntax and why it works this way.
//
// Flow of Reader::open():
//   1. parse() the name into Settings (bad keys or values fail here, before anything runs);
//   2. findTool(): $VST_RPICAM_VID, rpicam-vid or libcamera-vid;
//   3. list() the cameras, to check the index, name the model, cap the frame rate to what its modes allow and know
//      whether it has a motorised lens.  A listing that can't run isn't fatal: the camera is still tried, with defaults;
//   4. spawn the recorder with stdout on a pipe and stderr in a temp log (its messages explain most failures);
//   5. wait up to 10 s for the first frame, skip half a second, then time ~1 s of frames to confirm the frame rate.
// Every failure is turned into one plain sentence that says what to check, from what the recorder logged.
#include "PiCamera.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace picam {

namespace {

constexpr int kFirstFrameMs = 10000;   // camera start-up, auto white balance and gain settling
constexpr int kFrameMs = 5000;         // a running camera that sends nothing for this long has stopped
constexpr int kSettleMs = 500;         // after the first frame, not timed: start-up bursts (see Reader::open)
constexpr double kTimingSeconds = 1.0; // frames timed after that, to confirm the frame rate
constexpr double kSlowFraction = 0.9;  // measured below this fraction of the asked-for rate -> report the measured rate

std::string lower(std::string s) {
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

// "12" -> 12, rejecting trailing junk and anything outside [lo, hi].
bool toNumber(const std::string& v, double lo, double hi, double& out) {
	if (v.empty()) return false;
	char* end = nullptr;
	double d = std::strtod(v.c_str(), &end);
	if (end != v.c_str() + v.size() || !std::isfinite(d) || d < lo || d > hi) return false;
	out = d;
	return true;
}

std::string fmt(double v) {
	std::ostringstream o;
	o << v;
	return o.str();
}

}  // namespace

double Camera::maxFps(int width, int height) const {
	double best = 0;
	for (const Mode& m : modes)
		if (m.width >= width && m.height >= height) best = std::max(best, m.fps);
	return best;
}

bool isName(const std::string& name) {
	return name == "picam" || name.compare(0, 6, "picam:") == 0;
}

bool parse(const std::string& name, Settings& out, std::string& error) {
	out = Settings();
	if (!isName(name)) { error = "\"" + name + "\" isn't a Pi camera name (picam or picam:0)"; return false; }
	std::string rest = name.size() > 6 ? name.substr(6) : "";
	std::string index = rest.substr(0, rest.find('?'));
	std::string query = rest.find('?') == std::string::npos ? "" : rest.substr(rest.find('?') + 1);
	double d = 0;
	if (!index.empty()) {
		if (!toNumber(index, 0, 15, d) || d != std::floor(d)) { error = "Pi camera number \"" + index + "\" should be 0, 1, ..."; return false; }
		out.camera = int(d);
	}
	std::stringstream params(query);
	for (std::string kv; std::getline(params, kv, '&');) {
		if (kv.empty()) continue;
		size_t eq = kv.find('=');
		std::string key = lower(kv.substr(0, eq)), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
		auto bad = [&](const std::string& want) { error = "Pi camera setting " + key + "=" + v + ": " + want; return false; };
		if (key == "fps") {
			if (!toNumber(v, 1, 240, d)) return bad("use a frame rate from 1 to 240, e.g. fps=60");
			out.fps = d;
		}
		else if (key == "shutter") {
			if (!toNumber(v, 0, 1000000, d)) return bad("use microseconds, e.g. shutter=1000 for 1/1000 s (0 = automatic)");
			out.shutterUs = int(d);
		}
		else if (key == "gain") {
			if (!toNumber(v, 0, 64, d)) return bad("use an analogue gain from 1 to 64, or 0 for automatic");
			out.gain = d;
		}
		else if (key == "focus") {
			if (!toNumber(v, 0, 32, d)) return bad("use dioptres (1 / distance in metres), e.g. focus=0.25 for 4 m; 0 is infinity");
			out.focus = d;
			out.focusGiven = true;
		}
		else if (key == "size") {
			size_t x = lower(v).find('x');
			double w = 0, h = 0;
			if (x == std::string::npos || !toNumber(v.substr(0, x), 64, 4096, w) || !toNumber(v.substr(x + 1), 16, 4096, h)
				|| std::fmod(w, 64) != 0 || std::fmod(h, 16) != 0)
				return bad("use WIDTHxHEIGHT with the width a multiple of 64 and the height of 16, e.g. size=1280x720");
			out.width = int(w);
			out.height = int(h);
		}
		else if (key == "rotate" || key == "rotation") {
			if (v != "0" && v != "180") return bad("only 0 and 180 are possible (for a camera mounted upside down)");
			out.rotation = std::stoi(v);
		}
		else {
			error = "Unknown Pi camera setting \"" + key + "\" (known: fps, shutter, gain, focus, size, rotate)";
			return false;
		}
	}
	return true;
}

std::string name(const Settings& s) {
	const Settings d;
	std::string q;
	auto add = [&](const std::string& kv) { q += (q.empty() ? "?" : "&") + kv; };
	if (s.fps > 0) add("fps=" + fmt(s.fps));
	if (s.shutterUs != d.shutterUs) add("shutter=" + std::to_string(s.shutterUs));
	if (s.gain > 0) add("gain=" + fmt(s.gain));
	if (s.focusGiven) add("focus=" + fmt(s.focus));
	if (s.width != d.width || s.height != d.height) add("size=" + std::to_string(s.width) + "x" + std::to_string(s.height));
	if (s.rotation) add("rotate=" + std::to_string(s.rotation));
	return "picam:" + std::to_string(s.camera) + q;
}

std::string label(const std::string& name) {
	Settings s;
	std::string error;
	return parse(name, s, error) ? "Pi camera " + std::to_string(s.camera) : name;
}

std::string modelName(const std::string& sensor) {
	// Variants are the base sensor plus _wide and/or _noir (no infrared filter), e.g. imx708_wide_noir.
	std::string base = sensor.substr(0, sensor.find('_'));
	std::string model = base == "imx296" ? "Global Shutter Camera"
		: base == "imx708" ? "Camera Module 3"
		: base == "imx219" ? "Camera Module v2"
		: base == "imx477" ? "HQ Camera"
		: base == "imx500" ? "AI Camera"
		: base == "ov5647" ? "Camera Module v1"
		: "";
	if (model.empty()) return sensor;
	if (sensor.find("_wide") != std::string::npos) model += " Wide";
	if (sensor.find("_noir") != std::string::npos) model += " NoIR";
	return model;
}

std::vector<Camera> parseList(const std::string& text) {
	// 0 : imx708_wide [4608x2592 10-bit RGGB] (/base/axi/pcie@120000/rp1/i2c@88000/imx708@1a)
	//     Modes: 'SRGGB10_CSI2P' : 1536x864 [120.13 fps - (768, 432)/3072x1728 crop]
	//                              2304x1296 [56.03 fps - (0, 0)/4608x2592 crop]
	static const std::regex camLine(R"(^\s*(\d+)\s*:\s*([A-Za-z0-9_]+)\s*\[)");
	static const std::regex modeEntry(R"((\d+)x(\d+)\s*\[\s*([0-9.]+)\s*fps)");
	std::vector<Camera> cams;
	std::stringstream in(text);
	for (std::string line; std::getline(in, line);) {
		std::smatch m;
		if (std::regex_search(line, m, camLine)) {
			Camera c;
			c.index = std::stoi(m[1]);
			c.sensor = m[2];
			c.model = modelName(c.sensor);
			c.autofocus = c.sensor.compare(0, 6, "imx708") == 0;
			cams.push_back(c);
			continue;   // the sensor's full size on this line is in brackets too, but has no "fps"
		}
		if (cams.empty()) continue;
		for (auto it = std::sregex_iterator(line.begin(), line.end(), modeEntry); it != std::sregex_iterator(); ++it)
			cams.back().modes.push_back({ std::stoi((*it)[1]), std::stoi((*it)[2]), std::stod((*it)[3]) });
	}
	return cams;
}

double chooseFps(const Settings& s, const Camera* cam) {
	if (s.fps > 0) return s.fps;
	double cap = cam ? cam->maxFps(s.width, s.height) : 0;
	// Whole frames per second: 60.38 -> 60, 47.57 -> 47.
	return cap > 0 ? std::min(60.0, std::floor(cap)) : 60.0;
}

std::vector<std::string> commandArgs(const Settings& s, double fps, const Camera* cam) {
	std::vector<std::string> a = {
		"--camera", std::to_string(s.camera), "-t", "0", "--nopreview", "--codec", "yuv420", "--flush", "-o", "-",
		"--width", std::to_string(s.width), "--height", std::to_string(s.height), "--framerate", fmt(fps),
	};
	if (s.shutterUs > 0) { a.push_back("--shutter"); a.push_back(std::to_string(s.shutterUs)); }
	if (s.gain > 0) { a.push_back("--gain"); a.push_back(fmt(s.gain)); }
	// A fixed lens position: continuous autofocus (Camera Module 3's default) hunts every time a car crosses the view.
	if ((cam && cam->autofocus) || (!cam && s.focusGiven)) {
		for (const char* x : { "--autofocus-mode", "manual", "--lens-position" }) a.push_back(x);
		a.push_back(fmt(s.focus));
	}
	if (s.rotation == 180) { a.push_back("--rotation"); a.push_back("180"); }
	return a;
}

bool onRaspberryPi() {
	std::ifstream f("/proc/device-tree/model");
	std::string model;
	std::getline(f, model, '\0');
	return model.find("Raspberry Pi") != std::string::npos;
}

std::string noToolMessage() {
#ifdef _WIN32
	bool pi = false;
#else
	bool pi = onRaspberryPi();
#endif
	if (pi) return "rpicam-vid, which runs Raspberry Pi cameras, isn't installed. Install it with: sudo apt install rpicam-apps";
	return "Raspberry Pi ribbon cameras work when VST runs on the Raspberry Pi they're plugged into. "
		"On this computer, use a USB camera, a phone as a webcam, or a network camera.";
}

#ifdef _WIN32

std::string findTool() { return ""; }
std::vector<Camera> list(bool& ok, std::string& error) { ok = false; error = noToolMessage(); return {}; }
Reader::~Reader() {}
bool Reader::open(const std::string& name) {
	std::string e;
	if (!parse(name, settings_, e)) { error_ = e; return false; }
	error_ = noToolMessage();
	return false;
}
bool Reader::read(cv::Mat&) { return false; }
void Reader::release() {}
bool Reader::readRaw(int) { return false; }
std::string Reader::failure(const std::string& fallback) { return fallback; }

#else

namespace {

bool executable(const fs::path& p) { return !p.empty() && access(p.c_str(), X_OK) == 0 && !fs::is_directory(p); }

// The environment for the recorder: ours, plus quieter libcamera logging unless the user set their own.
std::vector<std::string> childEnv() {
	std::vector<std::string> env;
	bool hasLevels = false;
	for (char** e = environ; e && *e; e++) {
		env.emplace_back(*e);
		if (env.back().compare(0, 21, "LIBCAMERA_LOG_LEVELS=") == 0) hasLevels = true;
	}
	if (!hasLevels) env.emplace_back("LIBCAMERA_LOG_LEVELS=*:ERROR");
	return env;
}

// Starts `tool args...` with stdout on a new pipe (returned in readFd) and stderr to stderrPath ("" = the same pipe).
// posix_spawn rather than fork(): VST Studio is multithreaded, and nothing between fork and exec has to be async-safe.
int spawn(const std::string& tool, const std::vector<std::string>& args, const std::string& stderrPath, int& readFd) {
	// Close-on-exec, so no other program VST starts holds the pipe open (pipe2() isn't on macOS).
	int fds[2];
	if (pipe(fds) != 0) return -1;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	if (stderrPath.empty()) posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	else posix_spawn_file_actions_addopen(&fa, 2, stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	std::vector<std::string> argStore = { tool };
	argStore.insert(argStore.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (auto& s : argStore) argv.push_back(s.data());
	argv.push_back(nullptr);
	std::vector<std::string> envStore = childEnv();
	std::vector<char*> envp;
	for (auto& s : envStore) envp.push_back(s.data());
	envp.push_back(nullptr);
	pid_t pid = -1;
	int rc = posix_spawn(&pid, tool.c_str(), &fa, nullptr, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (rc != 0) { close(fds[0]); return -1; }
	readFd = fds[0];
	return pid;
}

// Ends a child: SIGTERM, up to 2 s to exit, then SIGKILL.  Always reaps it, so the camera is free when this returns.
void stopChild(int pid) {
	if (pid <= 0) return;
	kill(pid, SIGTERM);
	for (int i = 0; i < 100; i++) {
		if (waitpid(pid, nullptr, WNOHANG) == pid) return;
		usleep(20000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, nullptr, 0);
}

// Reads up to n bytes into p, waiting at most timeoutMs in all.  Returns the bytes read (< n on EOF or timeout).
size_t readFully(int fd, unsigned char* p, size_t n, int timeoutMs) {
	size_t got = 0;
	auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
	while (got < n) {
		int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
		if (left <= 0) break;
		pollfd pfd{ fd, POLLIN, 0 };
		int r = poll(&pfd, 1, left);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		ssize_t k = ::read(fd, p + got, n - got);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) break;
		got += size_t(k);
	}
	return got;
}

}  // namespace

std::string findTool() {
	if (const char* env = std::getenv("VST_RPICAM_VID"); env && *env) return executable(env) ? env : "";
	const char* path = std::getenv("PATH");
	std::stringstream dirs(path && *path ? path : "/usr/local/bin:/usr/bin:/bin");
	std::vector<std::string> dirList;
	for (std::string d; std::getline(dirs, d, ':');) if (!d.empty()) dirList.push_back(d);
	for (const char* name : { "rpicam-vid", "libcamera-vid" })   // libcamera-vid: the name before Bookworm
		for (const std::string& d : dirList)
			if (executable(fs::path(d) / name)) return (fs::path(d) / name).string();
	return "";
}

std::vector<Camera> list(bool& ok, std::string& error) {
	ok = false;
	std::string tool = findTool();
	if (tool.empty()) { error = noToolMessage(); return {}; }
	int fd = -1;
	int pid = spawn(tool, { "--list-cameras" }, "", fd);
	if (pid <= 0) { error = "Couldn't run " + tool; return {}; }
	// Everything it prints until it exits (EOF), or 5 s if it hangs.
	std::string text;
	std::vector<unsigned char> chunk(1 << 16);
	auto until = Clock::now() + std::chrono::seconds(5);
	for (;;) {
		int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
		size_t k = left > 0 ? readFully(fd, chunk.data(), chunk.size(), left) : 0;
		text.append((const char*)chunk.data(), k);
		if (k < chunk.size()) break;
	}
	close(fd);
	stopChild(pid);
	ok = true;
	return parseList(text);
}

Reader::~Reader() { release(); }

void Reader::release() {
	if (fd_ >= 0) close(fd_);   // the recorder gets SIGPIPE on its next write, if SIGTERM below hasn't ended it first
	fd_ = -1;
	stopChild(pid_);
	pid_ = -1;
	if (!logPath_.empty()) { std::error_code ec; fs::remove(logPath_, ec); logPath_.clear(); }
}

bool Reader::readRaw(int timeoutMs) {
	return readFully(fd_, buf_.data(), buf_.size(), timeoutMs) == buf_.size();
}

bool Reader::read(cv::Mat& bgr) {
	if (fd_ < 0) return false;
	if (!readRaw(kFrameMs)) { error_ = "The Pi camera stopped sending pictures."; return false; }
	cv::Mat yuv(settings_.height * 3 / 2, settings_.width, CV_8UC1, buf_.data());
	cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
	return true;
}

// The recorder's log, turned into what to do about it.
std::string Reader::failure(const std::string& fallback) {
	std::ifstream f(logPath_);
	std::string log((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), l = lower(log);
	std::string cam = "Pi camera " + std::to_string(settings_.camera);
	if (l.find("no cameras available") != std::string::npos)
		return "No Raspberry Pi camera found. Check the ribbon cable is pushed fully into both connectors with the contacts "
			"facing the right way (a Pi 5 needs the 22-pin to 15-pin cable), then reboot. rpicam-hello --list-cameras should list it.";
	if (l.find("busy") != std::string::npos || l.find("failed to acquire") != std::string::npos)
		return cam + " is in use by another program (another VST, rpicam-hello or a browser tab). Close it and connect again.";
	if (l.find("permission denied") != std::string::npos)
		return "VST isn't allowed to use the camera. Add yourself to the video group (sudo usermod -aG video $USER), then log out and back in.";
	// Otherwise the last thing it said, which is usually the reason.
	std::string last;
	std::stringstream in(log);
	for (std::string line; std::getline(in, line);) if (line.find_first_not_of(" \t\r") != std::string::npos) last = line;
	return last.empty() ? fallback : fallback + " (" + last + ")";
}

bool Reader::open(const std::string& name) {
	release();
	error_.clear();
	notes_.clear();
	camera_ = Camera();
	if (!parse(name, settings_, error_)) return false;
	std::string tool = findTool();
	if (tool.empty()) { error_ = noToolMessage(); return false; }

	bool listed = false;
	std::string listError;
	std::vector<Camera> cams = list(listed, listError);
	const Camera* cam = nullptr;
	if (listed) {
		if (cams.empty()) {
			error_ = "No Raspberry Pi camera found. Check the ribbon cable is pushed fully into both connectors with the contacts "
				"facing the right way (a Pi 5 needs the 22-pin to 15-pin cable), then reboot.";
			return false;
		}
		for (const Camera& c : cams) if (c.index == settings_.camera) cam = &c;
		if (!cam) {
			error_ = "There's no Pi camera " + std::to_string(settings_.camera) + "; this Pi has ";
			for (size_t i = 0; i < cams.size(); i++)
				error_ += (i ? ", " : "") + std::string("camera ") + std::to_string(cams[i].index) + " (" + cams[i].model + ")";
			error_ += ".";
			return false;
		}
		camera_ = *cam;
	}
	double asked = chooseFps(settings_, cam);
	std::vector<std::string> args = commandArgs(settings_, asked, cam);

	logPath_ = (fs::temp_directory_path() / ("vst-picam" + std::to_string(settings_.camera) + "-" + std::to_string(getpid()) + ".log")).string();
	pid_ = spawn(tool, args, logPath_, fd_);
	if (pid_ <= 0) { pid_ = -1; error_ = "Couldn't run " + tool; return false; }
	buf_.assign(size_t(settings_.width) * settings_.height * 3 / 2, 0);

	if (!readRaw(kFirstFrameMs)) {
		std::string why = failure("Pi camera " + std::to_string(settings_.camera) + " started but sent no pictures within 10 s. "
			"Check the ribbon cable, then connect again.");
		release();
		error_ = why;
		return false;
	}
	// Time a second of frames: the recorder only says what it was asked for, not what the sensor managed.  The first half
	// second is skipped, since frames queued while the camera started arrive in a burst.  Each part stops early after 1.5x
	// the frames it should take, so a source faster than real time (the tests' stand-in) loses only ~1.2 s worth.
	auto settle = Clock::now() + std::chrono::milliseconds(kSettleMs);
	for (int skipped = 0, most = int(std::ceil(asked * kSettleMs / 1000.0 * 1.5)); Clock::now() < settle && skipped < most; skipped++)
		if (!readRaw(kFrameMs)) break;
	int frames = 0, enough = int(std::ceil(asked * kTimingSeconds * 1.5));
	auto t0 = Clock::now();
	double elapsed = 0;
	while (elapsed < kTimingSeconds && frames < enough && readRaw(kFrameMs)) {
		frames++;
		elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
	}
	if (frames == 0) {
		std::string why = failure("Pi camera " + std::to_string(settings_.camera) + " stopped right after starting.");
		release();
		error_ = why;
		return false;
	}
	double measured = frames / std::max(elapsed, 1e-6);
	fps_ = asked;
	std::string model = camera_.model.empty() ? "Pi camera" : camera_.model;
	if (measured < asked * kSlowFraction) {
		fps_ = std::round(measured * 10) / 10;
		notes_.push_back(model + " is delivering " + fmt(fps_) + " fps, not the " + fmt(asked) + " asked for; speeds use "
			+ fmt(fps_) + ". A smaller size or a lower fps= keeps it steady.");
	}
	else if (settings_.fps == 0 && asked < 60)
		notes_.push_back(model + " tops out at " + fmt(asked) + " fps at " + std::to_string(settings_.width) + "x"
			+ std::to_string(settings_.height) + "; that's enough for most streets (see the site planner).");
	std::string line = "Pi camera " + std::to_string(settings_.camera) + (camera_.model.empty() ? "" : " (" + camera_.model + ")") + ": "
		+ std::to_string(settings_.width) + "x" + std::to_string(settings_.height) + " at " + fmt(fps_) + " fps, shutter "
		+ (settings_.shutterUs > 0 ? "1/" + fmt(std::round(1e6 / settings_.shutterUs)) + " s" : std::string("automatic"))
		+ ", gain " + (settings_.gain > 0 ? fmt(settings_.gain) : std::string("automatic"));
	if (std::find(args.begin(), args.end(), "--lens-position") != args.end())
		line += ", focus locked at " + fmt(settings_.focus) + " dioptres";
	notes_.insert(notes_.begin(), line);
	return true;
}

#endif

}  // namespace picam

double FrameSource::get(int prop) const {
	if (!pi.isOpened()) return cap.get(prop);
	switch (prop) {
	case cv::CAP_PROP_FRAME_WIDTH: return pi.width();
	case cv::CAP_PROP_FRAME_HEIGHT: return pi.height();
	case cv::CAP_PROP_FPS: return pi.fps();
	case cv::CAP_PROP_FOURCC: return cv::VideoWriter::fourcc('I', '4', '2', '0');
	default: return 0;   // CAP_PROP_FRAME_COUNT and the rest: a live camera has no length or position
	}
}
