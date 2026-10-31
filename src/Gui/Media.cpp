#include "Media.h"
#include "Level.h"
#include <GLFW/glfw3.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2; Windows' gl.h stops at 1.1
#endif

namespace fs = std::filesystem;

Texture::~Texture() { reset(); }

void Texture::reset() {
	if (id_) glDeleteTextures(1, &id_);
	id_ = 0;
	w_ = h_ = 0;
}

void Texture::update(const cv::Mat& image) {
	if (image.empty()) return;
	switch (image.channels()) {
	case 1: cv::cvtColor(image, rgba_, cv::COLOR_GRAY2RGBA); break;
	case 4: cv::cvtColor(image, rgba_, cv::COLOR_BGRA2RGBA); break;
	default: cv::cvtColor(image, rgba_, cv::COLOR_BGR2RGBA); break;
	}
	if (!id_) {
		glGenTextures(1, &id_);
		glBindTexture(GL_TEXTURE_2D, id_);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, id_);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (rgba_.cols != w_ || rgba_.rows != h_) {
		w_ = rgba_.cols;
		h_ = rgba_.rows;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data);
	}
	else
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data);
}

static std::string lowerExt(const std::string& path) {
	std::string e = fs::path(path).extension().string();
	for (char& c : e) c = (char)tolower((unsigned char)c);
	return e;
}

bool isVideoPath(const std::string& path) {
	static const char* exts[] = { ".avi", ".mp4", ".mkv", ".mov", ".m4v", ".ts", ".h264", ".mjpeg", ".webm" };
	std::string e = lowerExt(path);
	return std::any_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; });
}

bool isImagePath(const std::string& path) {
	std::string e = lowerExt(path);
	return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp";
}

SourceInfo::Kind classifySource(const std::string& path) {
	if (path.empty()) return SourceInfo::None;
	if (picam::isName(path)) return SourceInfo::Camera;
	if (path.size() < 4 && std::all_of(path.begin(), path.end(), [](char c) { return isdigit((unsigned char)c) != 0; }))
		return SourceInfo::Camera;
	if (path.find("://") != std::string::npos) return SourceInfo::Stream;
	if (isImagePath(path)) return SourceInfo::Image;
	return SourceInfo::File;
}

std::string displayName(const std::string& path) {
	switch (classifySource(path)) {
	case SourceInfo::Camera: return picam::isName(path) ? picam::label(path) : "Camera " + path;
	case SourceInfo::Stream: {
		// Hide credentials in rtsp://user:pass@host/... when showing a URL.
		std::string s = path;
		size_t scheme = s.find("://"), at = s.find('@');
		if (at != std::string::npos && at > scheme) s = s.substr(0, scheme + 3) + s.substr(at + 1);
		return s;
	}
	default: return fs::path(path).filename().string();
	}
}

std::string friendlySourceName(const std::string& path) {
	if (classifySource(path) == SourceInfo::File && fs::path(path).filename() == "manual_20160130172251.avi")
		return "Sample video (Locust Ave, 2016)";
	return displayName(path);
}

std::string shortSourceName(const std::string& path, size_t max) {
	std::string s = friendlySourceName(path);
	if (s.size() <= max || max < 8) return s;
	size_t head = (max - 1) * 2 / 3, tail = max - 1 - head;
	auto cut = [&](size_t i) { while (i > 0 && i < s.size() && (s[i] & 0xC0) == 0x80) i--; return i; };   // not mid UTF-8
	return s.substr(0, cut(head)) + "…" + s.substr(cut(s.size() - tail));
}

void toWorkingSize(cv::Mat& frame) {
	if (!frame.empty() && (frame.cols != 1280 || frame.rows != 720))
		cv::resize(frame, frame, cv::Size(1280, 720), 0, 0, cv::INTER_AREA);
}

// The background opener and reader (see Player in Media.h).  The thread holds its own reference, so one the Player
// has let go of finishes on its own: it notices `stop` after its current open or read returns, and releases the
// device itself.  For a file it only opens, reads the first frame and hands the capture over (`handoff`); for a camera
// or stream it keeps reading, keeping only the newest frame.
struct Player::LiveReader {
	std::atomic<bool> stop{ false };
	std::atomic<bool> finished{ false };   // the thread has released the device and returned
	std::mutex mu;                 // guards everything below
	bool ready = false;            // opened and the first frame arrived
	std::string error;             // set once, when the open or the picture fails
	SourceInfo info;               // width, height, fps, codec, frame count once ready
	cv::Mat latest;                // the newest frame at working size
	std::uint64_t seq = 0;         // counts frames, so the GUI can tell a new one
	double startedAt = 0;          // glfwGetTime() when the open began
	std::unique_ptr<FrameSource> handoff;        // files: the opened capture, for the GUI thread to step through
	std::string path;              // what it opens, so reconnecting the same device can wait for the old reader
};

namespace {

constexpr int kStreamTimeoutMs = 5000;    // FFmpeg's open and read timeouts for network streams (its opens share one lock, so keep it short)
constexpr double kFirstFrameS = 15;       // an opened camera that sends nothing for this long is reported, not waited on
constexpr int kLostAfterFailures = 50;    // consecutive failed reads (about 5 s) before a live picture counts as lost

std::string openError(const SourceInfo& in, const FrameSource& cap) {
	if (picam::isName(in.path)) return cap.pi.error();
	switch (in.kind) {
	case SourceInfo::Camera:
		return "Can't open " + in.label + " yet. Check it's plugged in (for a phone, that its webcam app is running) and that no other app, like a video call, is using it. Or try another number.";
	case SourceInfo::Stream:
		return "Can't connect to " + in.label + ". Check the address, user name and password, and that the camera is on this network.";
	default:
		return "Can't open " + in.label + ". It may be damaged or in a format VST can't read; converting it to MP4 usually helps (ffmpeg -i in.ext -c:v libx264 out.mp4).";
	}
}

// Opens a camera index, a stream URL or a file.  Streams get FFmpeg timeouts so a dead address fails in seconds; a
// camera open can't be timed out, which is one reason this never runs on the GUI thread.  (The other: OpenCV
// serialises some backend work, so a file opened on the GUI thread while a camera is still opening waits for it.)
// A Raspberry Pi camera (PiCamera.h) goes through rpicam-vid, which takes a couple of seconds to list the cameras and
// time a second of frames.
bool openCapture(FrameSource& cap, const SourceInfo& in) {
	if (picam::isName(in.path)) return cap.openPiCamera(in.path);
	if (in.kind == SourceInfo::Camera) return cap.cap.open(std::stoi(in.path));
	if (in.kind == SourceInfo::Stream)
		return cap.cap.open(in.path, cv::CAP_ANY, { cv::CAP_PROP_OPEN_TIMEOUT_MSEC, kStreamTimeoutMs, cv::CAP_PROP_READ_TIMEOUT_MSEC, kStreamTimeoutMs });
	return cap.cap.open(in.path);
}

// Width, height, fps, codec and (files) frame count, as the capture reports them.
void describe(const FrameSource& cap, SourceInfo& in) {
	in.width = int(cap.get(cv::CAP_PROP_FRAME_WIDTH));
	in.height = int(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
	in.fps = cap.get(cv::CAP_PROP_FPS);
	if (!(in.fps > 0 && in.fps < 1000)) in.fps = 0;
	if (in.kind == SourceInfo::File) in.frames = std::max(0, int(cap.get(cv::CAP_PROP_FRAME_COUNT)));
	int fourcc = int(cap.get(cv::CAP_PROP_FOURCC));
	if (fourcc > 0)
		for (int i = 0; i < 4; i++) {
			char ch = char((fourcc >> (8 * i)) & 255);
			if (isprint((unsigned char)ch)) in.codec += ch;
		}
}

}  // namespace

// The thread: open, report the source's facts with the first frame, then (cameras and streams) keep only the newest
// frame until told to stop.  Everything slow (open, read, resize) happens here; the GUI only copies a finished Mat.
static void runReader(std::shared_ptr<Player::LiveReader> r, SourceInfo in, std::shared_ptr<Player::LiveReader> prev) {
	struct Done { Player::LiveReader& r; ~Done() { r.finished = true; } } done{ *r };
	// Reconnecting the same camera: give the previous reader up to 3 s to release it, or the open would find it busy.
	// Anything else opens straight away, whatever state the previous reader is in.
	if (prev && prev->path != in.path) prev.reset();
	for (int i = 0; prev && !prev->finished && i < 300 && !r->stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	prev.reset();
	auto fail = [&](const std::string& why) { std::lock_guard<std::mutex> l(r->mu); if (r->error.empty()) r->error = why; };
	auto cap = std::make_unique<FrameSource>();
	bool ok = false;
	try { ok = openCapture(*cap, in) && cap->isOpened(); } catch (const std::exception&) { ok = false; }
	if (r->stop) return;
	if (!ok) { fail(openError(in, *cap)); return; }
	describe(*cap, in);
	auto since = std::chrono::steady_clock::now();
	int failures = 0;
	bool ready = false;
	while (!r->stop) {
		cv::Mat f;
		bool got = false;
		try { got = cap->read(f) && !f.empty(); } catch (const std::exception&) { got = false; }
		if (r->stop) break;
		if (!got) {
			if (in.kind == SourceInfo::File) { fail(openError(in, *cap)); break; }
			double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
			if (!ready && waited > kFirstFrameS) { fail(in.label + " opened but sent no picture. Try unplugging it and plugging it back in, or another number."); break; }
			if (ready && ++failures >= kLostAfterFailures) { fail("Lost the picture from " + in.label + ". Check its cable or network, then connect again."); break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		failures = 0;
		if (in.width <= 0 || in.height <= 0) { in.width = f.cols; in.height = f.rows; }
		toWorkingSize(f);
		std::lock_guard<std::mutex> l(r->mu);
		r->latest = f;
		r->seq++;
		if (!ready) { r->info = in; r->ready = ready = true; }
		if (in.kind == SourceInfo::File) { r->handoff = std::move(cap); return; }   // the GUI steps through files itself
	}
	if (cap) cap->release();
}

Player::~Player() { close(); }

void Player::closeAndWait(double maxS) {
	std::shared_ptr<LiveReader> r = live_;
	close();
	auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(maxS);
	while (r && !r->finished && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

double Player::openingSeconds(double now) const {
	if (!live_) return 0;
	std::lock_guard<std::mutex> l(live_->mu);
	return now - live_->startedAt;
}

bool Player::open(const std::string& path) {
	std::shared_ptr<LiveReader> prev = live_;   // a new reader lets this one release its device first
	close();
	error.clear();
	info_ = SourceInfo();
	info_.kind = classifySource(path);
	info_.path = path;
	info_.label = friendlySourceName(path);
	if (info_.kind == SourceInfo::Image) {
		// A still photo of the view is enough to plan zones before any camera is installed.
		cv::Mat photo = cv::imread(path);
		if (photo.empty()) { error = "Can't read image " + path; return false; }
		info_.width = photo.cols;
		info_.height = photo.rows;
		toWorkingSize(photo);
		show(photo);
		opened_ = true;
		pos_ = 0;
		return true;
	}
	live_ = std::make_shared<LiveReader>();
	live_->startedAt = glfwGetTime();
	live_->path = path;
	liveSeq_ = 0;
	try { std::thread(runReader, live_, info_, prev).detach(); }
	catch (const std::exception&) { live_.reset(); error = "Can't start opening " + info_.label + "."; return false; }
	return true;
}

void Player::close() {
	if (live_) { live_->stop = true; live_.reset(); }
	if (cap_) cap_->release();
	cap_.reset();
	opened_ = false;
	playing = false;
	frame_.release();
	raw_.release();
	pos_ = -1;
	carry_ = 0;
	fed_ = false;
	fedLevel_ = 0;
}

void Player::show(const cv::Mat& working) {
	raw_ = working;
	frame_ = working;           // shares raw_'s pixels at a zero angle; levelFrame() writes a new Mat, never into raw_
	levelFrame(frame_, level_ - fedLevel_);   // turns about the centre add up, so a fed frame needs only the difference
	tex_.update(frame_);
}

// No reader, no device: the frame is the tracker's copy.  It is cloned because the caller's Mat (App::live_) is
// refilled in place by Tracker::latest(), and frame() must stay the picture the texture shows (the cone timer and the
// companion's snapshot read it).  One 1280x720 copy and one texture upload per shown frame, at most ~40 a second.
void Player::feed(const cv::Mat& working, double turnedBy, const SourceInfo& about, int frameNumber) {
	if (working.empty()) return;
	if (live_ || cap_ || !fed_) {
		// Anything this Player still had open (a preview reader App::startTracking abandoned) lets go now.
		SourceInfo keep = info_;
		close();
		info_ = keep;
		error.clear();
	}
	if (about.path != info_.path) info_ = about;
	fed_ = true;
	opened_ = true;
	playing = false;
	fedLevel_ = turnedBy;
	pos_ = frameNumber;
	show(working.clone());
}

void Player::setLevel(double degrees) {
	if (degrees == level_) return;
	level_ = degrees;
	if (!raw_.empty()) show(raw_);
}

// Takes what the reader has: an error (the open or the picture failed), the opened file to step through, or the
// newest live frame.  Never waits for the reader.
bool Player::tickLive() {
	cv::Mat f;
	{
		std::lock_guard<std::mutex> l(live_->mu);
		if (!live_->error.empty()) {
			error = live_->error;
			live_->stop = true;
			live_.reset();
			opened_ = false;
			frame_.release();
			raw_.release();
			return false;
		}
		if (!live_->ready || live_->seq == liveSeq_) return false;
		if (!opened_) {
			std::string label = info_.label;
			info_ = live_->info;
			info_.label = label;
			opened_ = true;
			pos_ = -1;
		}
		liveSeq_ = live_->seq;
		f = live_->latest;   // shares the buffer; the reader makes a new Mat for each frame rather than writing into this one
		if (live_->handoff) { cap_ = std::move(live_->handoff); live_.reset(); }   // a file: stepping is the GUI's from here
	}
	show(f);
	pos_++;
	return true;
}

bool Player::readInto() {
	cv::Mat f;
	if (!cap_ || !cap_->read(f) || f.empty()) { playing = false; return false; }
	toWorkingSize(f);
	show(f);
	pos_++;
	return true;
}

bool Player::next() {
	if (live_) return tickLive();
	if (!opened_ || info_.kind == SourceInfo::Image) return false;
	return readInto();
}

bool Player::seek(int index) {
	if (live_) { tickLive(); if (live_ || !seekable()) return opened_; }   // still opening, or live: nothing to seek
	if (!opened_) return false;
	if (!seekable()) return next();
	index = std::clamp(index, 0, std::max(0, info_.frames - 1));
	if (index == pos_ + 1) return readInto();   // sequential step: no seek, which is slow on some codecs
	if (index == pos_) return true;
	cap_->set(cv::CAP_PROP_POS_FRAMES, index);
	pos_ = index - 1;
	return readInto();
}

bool Player::tick(double now) {
	if (fed_) return false;         // the feeder (App::pumpTracker) brings each frame
	if (live_) return tickLive();   // never waits: takes the reader's newest frame (or the opened file), if there is one
	if (!opened_ || info_.kind == SourceInfo::Image) return false;
	double dt = lastTick_ > 0 ? now - lastTick_ : 0;
	lastTick_ = now;
	if (!playing) { carry_ = 0; return false; }
	carry_ += dt * (info_.fps > 0 ? info_.fps : 30);
	bool any = false;
	int steps = 0;
	while (carry_ >= 1 && steps++ < 4) { carry_ -= 1; any = readInto() || any; }
	if (steps >= 4) carry_ = 0;   // fell behind: drop rather than spiral
	return any;
}

void openWithSystem(const std::string& path) {
#ifdef _WIN32
	std::wstring w = fs::path(path).wstring();
	ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
	// Quote for /bin/sh: wrap in single quotes and escape any single quote inside.
	std::string q = "'";
	for (char c : path) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
	q += "'";
#ifdef __APPLE__
	std::string cmd = "open " + q + " &";
#else
	std::string cmd = "xdg-open " + q + " >/dev/null 2>&1 &";
#endif
	int rc = std::system(cmd.c_str());
	(void)rc;
#endif
}
