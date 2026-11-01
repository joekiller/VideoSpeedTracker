// The frame input port's adapters and its opening policy (see FrameInput.h).
#include "FrameInput.h"
#include "Level.h"
#include "PiCamera.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio/registry.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <limits>

namespace frameio {

// ---------------------------------------------------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------------------------------------------------

static std::string lowerExtension(const std::string& name) {
	size_t dot = name.find_last_of('.'), slash = name.find_last_of("/\\");
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
	std::string e = name.substr(dot);
	for (char& c : e) c = char(std::tolower((unsigned char)c));
	return e;
}

bool isImagePath(const std::string& name) {
	std::string e = lowerExtension(name);
	return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp";
}

// Order matters: "picam:0?fps=60" has no "://" but would otherwise read as a file, and a URL ending in .jpg is a stream
// (an IP camera's snapshot address), not a photo on disk.
Kind classify(const std::string& name) {
	if (name.empty()) return Kind::None;
	if (picam::isName(name)) return Kind::PiCamera;
	if (name.size() < 4 && std::all_of(name.begin(), name.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }))
		return Kind::Camera;
	if (name.find("://") != std::string::npos) return Kind::Stream;
	if (isImagePath(name) && name.find('%') == std::string::npos) return Kind::Image;   // "img_%04d.jpg" is a numbered sequence: VideoCapture reads it as a video
	return Kind::File;
}

bool isLive(Kind k) { return k == Kind::Camera || k == Kind::PiCamera || k == Kind::Stream; }
bool isCamera(Kind k) { return k == Kind::Camera || k == Kind::PiCamera; }

std::string safeName(const std::string& name) {
	size_t scheme = name.find("://");
	if (scheme == std::string::npos) return name;
	std::string url = name;
	// user:password@host.  The host part ends at the query (or fragment); the last '@' before that ends the user info, so an
	// unescaped '@' or '/' in the password doesn't leave the rest of it behind.
	size_t queryAt = url.find_first_of("?#", scheme + 3);
	size_t at = url.rfind('@', queryAt == std::string::npos ? std::string::npos : queryAt - 1);
	if (at != std::string::npos && at > scheme) url.erase(scheme + 3, at - (scheme + 3) + 1);
	// ?a=1&pwd=x&b=2 -> ?a=1&b=2: drop the parameters named pwd or pw, or with "pass" anywhere in the name.
	size_t q = url.find('?');
	if (q == std::string::npos) return url;
	size_t hash = url.find('#', q);
	std::string query = url.substr(q + 1, hash == std::string::npos ? std::string::npos : hash - q - 1), kept;
	for (size_t start = 0; start <= query.size();) {
		size_t amp = query.find('&', start);
		std::string param = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
		std::string key = param.substr(0, param.find('='));
		for (char& c : key) c = char(std::tolower((unsigned char)c));
		if (!param.empty() && key != "pwd" && key != "pw" && key.find("pass") == std::string::npos) kept += (kept.empty() ? "" : "&") + param;
		if (amp == std::string::npos) break;
		start = amp + 1;
	}
	return url.substr(0, q) + (kept.empty() ? "" : "?" + kept) + (hash == std::string::npos ? "" : url.substr(hash));
}

std::string label(const std::string& name) {
	switch (classify(name)) {
	case Kind::Camera: return "camera " + name;
	case Kind::PiCamera: return picam::label(name);
	default: return safeName(name);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The port's shared parts
// ---------------------------------------------------------------------------------------------------------------------

Facts FrameSource::facts() const {
	Facts f;
	f.width = int(get(cv::CAP_PROP_FRAME_WIDTH));
	f.height = int(get(cv::CAP_PROP_FRAME_HEIGHT));
	f.fps = get(cv::CAP_PROP_FPS);
	if (!(f.fps > 0 && f.fps < 1000)) f.fps = 0;
	if (kind() == Kind::File) f.frames = std::max(0, int(get(cv::CAP_PROP_FRAME_COUNT)));
	int fourcc = int(get(cv::CAP_PROP_FOURCC));
	if (fourcc > 0)
		for (int i = 0; i < 4; i++) {
			char ch = char((fourcc >> (8 * i)) & 255);
			if (std::isprint((unsigned char)ch)) f.codec += ch;
		}
	addFacts(f);
	return f;
}

// ---------------------------------------------------------------------------------------------------------------------
// USB camera exposure (see uvc:: in FrameInput.h)
// ---------------------------------------------------------------------------------------------------------------------

namespace uvc {

static bool isWindowsBackend(const std::string& b) { return b == "MSMF" || b == "DSHOW"; }

double exposureValue(const std::string& backend, int us) {
	if (us <= 0) return std::numeric_limits<double>::quiet_NaN();
	if (backend == "V4L2") return std::max(1.0, std::round(us / 100.0));
	if (isWindowsBackend(backend)) return std::round(std::log2(us / 1e6));
	return std::numeric_limits<double>::quiet_NaN();
}

double autoExposureValue(const std::string& backend, bool automatic) {
	if (backend == "V4L2") return automatic ? 3 : 1;
	return automatic ? 1 : 0;
}

bool tookExposure(const std::string& backend, double wanted, double readBack) {
	if (std::isnan(wanted) || std::isnan(readBack)) return false;
	// V4L2 cameras round to their own step (a C922's is 1, others' coarser); Windows reads back the same log2 step.
	if (backend == "V4L2") return std::fabs(readBack - wanted) <= std::max(1.0, wanted * .25);
	return std::fabs(readBack - wanted) < .5;
}

std::vector<int> ladder(int us) {
	std::vector<int> out;
	for (int t = us; t > 0 && t <= 4000 && out.size() < 3; t *= 2) out.push_back(t);
	if (out.empty() && us > 0) out.push_back(us);   // asked for something slower than 1/250 s: just that
	return out;
}

bool brightEnough(double automaticMean, double fixedMean) {
	if (std::isnan(fixedMean)) return false;
	return fixedMean >= 40 && (std::isnan(automaticMean) || fixedMean >= automaticMean * .45);
}

}  // namespace uvc

void toWorking(cv::Mat& frame) {
	if (!frame.empty() && (frame.cols != kWorkingWidth || frame.rows != kWorkingHeight))
		cv::resize(frame, frame, cv::Size(kWorkingWidth, kWorkingHeight), 0, 0, cv::INTER_AREA);
}

bool readWorking(FrameSource& source, cv::Mat& frame, double levelDegrees) {
	if (!source.read(frame) || frame.empty()) return false;
	toWorking(frame);
	levelFrame(frame, levelDegrees);   // a camera mounted off level: turn the picture so the lanes run straight across
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------------------------------------------------

namespace {

// Nothing open: what the engine holds between videos, and what open("") gives.
class ClosedSource : public FrameSource {
public:
	Kind kind() const override { return Kind::None; }
	bool isOpened() const override { return false; }
	bool read(cv::Mat&) override { return false; }
	void release() override {}
	double get(int) const override { return 0; }
};

// Files, USB cameras and network streams: OpenCV's VideoCapture, opened by the policy in OpenOptions.
class CaptureSource : public FrameSource {
public:
	CaptureSource(const std::string& name, Kind kind, const OpenOptions& o) : kind_(kind) {
		if (kind == Kind::Camera) openCamera(std::stoi(name), o);
		else openNamed(name, o);
	}
	Kind kind() const override { return kind_; }
	bool isOpened() const override { return cap_.isOpened(); }
	bool read(cv::Mat& frame) override { return cap_.read(frame); }
	void release() override { cap_.release(); }
	double get(int prop) const override { return cap_.get(prop); }
	bool set(int prop, double value) override { return cap_.set(prop, value); }   // seeking: files, and URLs of recorded files
	std::vector<std::string> notes() const override { return notes_; }

protected:
	void addFacts(Facts& f) const override {
		f.shutter = shutter_;
		f.shutterUs = shutter_ == Shutter::Fixed ? shutterUs_ : 0;
		f.measuredFps = measuredFps_;
	}

private:
	void openCamera(int index, const OpenOptions& o) {
		if (!cap_.open(index)) return;
		if (o.cameraWidth <= 0 || o.cameraHeight <= 0) return;
		// Each set is a request; a camera without the mode keeps its nearest one, and facts() reports what it chose.
		cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
		cap_.set(cv::CAP_PROP_FRAME_WIDTH, o.cameraWidth);
		cap_.set(cv::CAP_PROP_FRAME_HEIGHT, o.cameraHeight);
		if (o.cameraFps > 0) cap_.set(cv::CAP_PROP_FPS, o.cameraFps);
		fixShutter(o);
	}

	// Reads `n` frames and returns the mean brightness of the last three, after the camera has had the first ones to
	// settle on a new setting; `fps` gets the rate those later frames arrived at.  NaN when the camera stops sending.
	double sample(int n, double& fps) {
		cv::Mat f;
		double sum = 0;
		int summed = 0;
		std::chrono::steady_clock::time_point from;
		const int timedFrom = std::max(1, n / 3);
		for (int i = 0; i < n; i++) {
			if (!cap_.read(f) || f.empty()) return std::numeric_limits<double>::quiet_NaN();
			if (i == timedFrom) from = std::chrono::steady_clock::now();
			if (i >= n - 3) { cv::Scalar m = cv::mean(f); sum += f.channels() >= 3 ? (m[0] + m[1] + m[2]) / 3 : m[0]; summed++; }
		}
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
		fps = s > 0 ? (n - 1 - timedFrom) / s : 0;
		return summed ? sum / summed : std::numeric_limits<double>::quiet_NaN();
	}

	// The opening policy's shutter step (OpenOptions::cameraShutterUs): fix the exposure time so cars stay sharp and the
	// frame rate can't sag on automatic, trying uvc::ladder() fastest first and keeping the first bright enough picture;
	// else back to automatic.  Every outcome goes in notes() for the log and in facts() for the UI.
	void fixShutter(const OpenOptions& o) {
		int want = o.cameraShutterUs;
		if (o.cameraShutterFromEnv)
			if (const char* env = std::getenv("VST_CAMERA_SHUTTER_US")) want = std::atoi(env);
		const std::string backend = cap_.getBackendName();
		double autoFps = 0;
		const double automaticMean = sample(12, autoFps);   // what the camera shows on its own, and how fast
		if (std::isnan(automaticMean)) return;               // no picture yet: the reader says so, nothing to fix
		measuredFps_ = autoFps;
		char b[200];
		if (want <= 0) { shutter_ = Shutter::Automatic; noteRate(); return; }
		if (std::isnan(uvc::exposureValue(backend, want))) {
			shutter_ = Shutter::CantSet;
			snprintf(b, sizeof b, "Camera shutter: can't be set from VST on this system (%s), so it stays automatic.", backend.c_str());
			notes_.push_back(b);
			noteRate();
			return;
		}
		for (int us : uvc::ladder(want)) {
			const double value = uvc::exposureValue(backend, us);
			cap_.set(cv::CAP_PROP_AUTO_EXPOSURE, uvc::autoExposureValue(backend, false));
			if (!cap_.set(cv::CAP_PROP_EXPOSURE, value) || !uvc::tookExposure(backend, value, cap_.get(cv::CAP_PROP_EXPOSURE))) {
				backToAutomatic(backend);
				shutter_ = Shutter::CantSet;
				notes_.push_back("Camera shutter: this camera didn't take a fixed exposure time, so it stays automatic.");
				noteRate();
				return;
			}
			double fps = 0;
			const double mean = sample(10, fps);
			if (std::isnan(mean)) return;
			if (uvc::brightEnough(automaticMean, mean)) {
				shutter_ = Shutter::Fixed;
				shutterUs_ = us;
				measuredFps_ = fps;
				snprintf(b, sizeof b, "Camera shutter: fixed at 1/%d s.", int(std::lround(1e6 / us)));
				notes_.push_back(b);
				noteRate();
				return;
			}
		}
		backToAutomatic(backend);
		double fps = 0;
		if (!std::isnan(sample(10, fps))) measuredFps_ = fps;
		shutter_ = Shutter::Automatic;
		snprintf(b, sizeof b, "Camera shutter: too dark at 1/%d s or faster, so it's back on automatic.",
			int(std::lround(1e6 / uvc::ladder(want).back())));
		notes_.push_back(b);
		noteRate();
	}

	void backToAutomatic(const std::string& backend) { cap_.set(cv::CAP_PROP_AUTO_EXPOSURE, uvc::autoExposureValue(backend, true)); }

	// A camera that says one rate and sends a lower one (a webcam slowing its shutter in dim light) is worth a line.
	void noteRate() {
		double said = cap_.get(cv::CAP_PROP_FPS);
		if (measuredFps_ > 0 && said > 0 && said < 1000 && measuredFps_ < said * .8) {
			char b[160];
			snprintf(b, sizeof b, "Camera frame rate: it reports %.0f fps but sent about %.0f while opening.", said, measuredFps_);
			notes_.push_back(b);
		}
	}

	void openNamed(const std::string& name, const OpenOptions& o) {
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 6)
		// Open parameters (hardware decoding, timeouts) need OpenCV 4.6+ and the FFmpeg backend.
		if (cv::videoio_registry::hasBackend(cv::CAP_FFMPEG)) {
			// Decoding is most of VST's work (about 55% of the CPU on the sample, against about 15% for the tracking
			// itself).  H.264/HEVC decoding is bit-exact in hardware or software, so the speeds don't change.
			bool hw = o.hwDecode;
			if (o.hwDecodeFromEnv) { const char* env = std::getenv("VST_HW_DECODE"); if (env && std::string(env) == "0") hw = false; }
			std::vector<int> params;
			if (hw) params = { cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY };
			if (kind_ == Kind::Stream) {
				params.insert(params.end(), { int(cv::CAP_PROP_OPEN_TIMEOUT_MSEC), o.streamOpenTimeoutMs, int(cv::CAP_PROP_READ_TIMEOUT_MSEC), o.streamReadTimeoutMs });
				cap_.open(name, cv::CAP_FFMPEG, params);
				return;   // a stream FFmpeg can't open isn't one another backend can
			}
			if (cap_.open(name, cv::CAP_FFMPEG, params)) return;
			// A file FFmpeg can't read: let OpenCV try its other backends, as before.
		}
#endif
		cap_.open(name);
	}

	Kind kind_;
	cv::VideoCapture cap_;
	Shutter shutter_ = Shutter::Unknown;
	int shutterUs_ = 0;
	double measuredFps_ = 0;
	std::vector<std::string> notes_;
};

// A Raspberry Pi camera: rpicam-vid, read frame by frame (PiCamera.h).
class PiCameraSource : public FrameSource {
public:
	explicit PiCameraSource(const std::string& name) { pi_.open(name); }
	Kind kind() const override { return Kind::PiCamera; }
	bool isOpened() const override { return pi_.isOpened(); }
	bool read(cv::Mat& frame) override { return pi_.read(frame); }
	void release() override { pi_.release(); }
	// Its size and frame rate, a frame count of 0 (live) and the I420 fourcc it sends.
	double get(int prop) const override {
		if (!pi_.isOpened()) return 0;
		switch (prop) {
		case cv::CAP_PROP_FRAME_WIDTH: return pi_.width();
		case cv::CAP_PROP_FRAME_HEIGHT: return pi_.height();
		case cv::CAP_PROP_FPS: return pi_.fps();
		case cv::CAP_PROP_FOURCC: return cv::VideoWriter::fourcc('I', '4', '2', '0');
		default: return 0;   // CAP_PROP_FRAME_COUNT and the rest: a live camera has no length or position
		}
	}
	std::string error() const override { return pi_.error(); }
	std::vector<std::string> notes() const override { return pi_.notes(); }
protected:
	// rpicam-vid sets the shutter it is given (PiCamera.h, 0 = automatic), and Reader::open() times the real rate.
	void addFacts(Facts& f) const override {
		if (!pi_.isOpened()) return;
		f.shutter = pi_.shutterUs() > 0 ? Shutter::Fixed : Shutter::Automatic;
		f.shutterUs = std::max(0, pi_.shutterUs());
		f.measuredFps = pi_.fps();
	}
private:
	picam::Reader pi_;
};

// A still photo: one frame, then the end, like a one-frame video.  The preview keeps the frame it read.
class ImageSource : public FrameSource {
public:
	explicit ImageSource(const std::string& name) : photo_(cv::imread(name)) {
		if (photo_.empty()) error_ = "Can't read image " + name;
	}
	Kind kind() const override { return Kind::Image; }
	bool isOpened() const override { return !photo_.empty(); }
	bool read(cv::Mat& frame) override {
		if (photo_.empty() || given_) return false;
		given_ = true;
		frame = photo_;
		return true;
	}
	void release() override { photo_.release(); }
	double get(int prop) const override {
		switch (prop) {
		case cv::CAP_PROP_FRAME_WIDTH: return photo_.cols;
		case cv::CAP_PROP_FRAME_HEIGHT: return photo_.rows;
		case cv::CAP_PROP_FRAME_COUNT: return photo_.empty() ? 0 : 1;
		default: return 0;
		}
	}
	std::string error() const override { return error_; }
private:
	cv::Mat photo_;
	bool given_ = false;
	std::string error_;
};

}  // namespace

// The feed adapter (FrameInput.h).
bool FeedSource::read(cv::Mat& frame) {
	if (!open_ || !fresh_) return false;
	fresh_ = false;
	frame = latest_;
	return true;
}

double FeedSource::get(int prop) const {
	switch (prop) {
	case cv::CAP_PROP_FRAME_WIDTH: return about_.width;
	case cv::CAP_PROP_FRAME_HEIGHT: return about_.height;
	case cv::CAP_PROP_FPS: return about_.fps;
	default: return 0;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------------------------------------------------

std::unique_ptr<FrameSource> closed() { return std::make_unique<ClosedSource>(); }

std::unique_ptr<FrameSource> open(const std::string& name, const OpenOptions& options) {
	switch (Kind k = classify(name)) {
	case Kind::None: return closed();
	case Kind::PiCamera: return std::make_unique<PiCameraSource>(name);
	case Kind::Image: return std::make_unique<ImageSource>(name);
	default: return std::make_unique<CaptureSource>(name, k, options);
	}
}

}  // namespace frameio


// ---------------------------------------------------------------------------------------------------------------------
// Camera reports (CameraReport, FrameInput.h)
// ---------------------------------------------------------------------------------------------------------------------

namespace frameio {

namespace {

// The modes worth knowing about for timing, VST's own ask first.
const struct { int w, h; double fps; } kProbeModes[] = { { 1280, 720, 60 }, { 1280, 720, 30 }, { 1920, 1080, 60 }, { 1920, 1080, 30 }, { 640, 480, 60 }, { 640, 480, 30 } };

const struct { int prop; const char* name; } kProbeControls[] = {
	{ cv::CAP_PROP_AUTO_EXPOSURE, "AUTO_EXPOSURE" }, { cv::CAP_PROP_EXPOSURE, "EXPOSURE" }, { cv::CAP_PROP_GAIN, "GAIN" },
	{ cv::CAP_PROP_BRIGHTNESS, "BRIGHTNESS" }, { cv::CAP_PROP_CONTRAST, "CONTRAST" }, { cv::CAP_PROP_SHARPNESS, "SHARPNESS" },
	{ cv::CAP_PROP_BACKLIGHT, "BACKLIGHT" }, { cv::CAP_PROP_AUTOFOCUS, "AUTOFOCUS" }, { cv::CAP_PROP_FOCUS, "FOCUS" },
	{ cv::CAP_PROP_AUTO_WB, "AUTO_WB" }, { cv::CAP_PROP_WB_TEMPERATURE, "WB_TEMPERATURE" }, { cv::CAP_PROP_ZOOM, "ZOOM" },
};

// The rate frames arrive at over about a second, after a few to let a new mode start.  0 when the camera sends nothing.
double timedRate(cv::VideoCapture& cap) {
	cv::Mat f;
	for (int i = 0; i < 5; i++) if (!cap.read(f) || f.empty()) return 0;
	auto from = std::chrono::steady_clock::now();
	int n = 0;
	while (std::chrono::steady_clock::now() - from < std::chrono::milliseconds(1000) && n < 240) {
		if (!cap.read(f) || f.empty()) break;
		n++;
	}
	double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
	return n > 0 && s > 0 ? n / s : 0;
}

std::string deviceName(int index) {
#ifdef __linux__
	std::ifstream in("/sys/class/video4linux/video" + std::to_string(index) + "/name");
	std::string line;
	if (in && std::getline(in, line)) return line;
#else
	(void)index;
#endif
	return "";
}

std::string number(double v) {
	char b[32];
	if (std::isnan(v)) return "?";
	snprintf(b, sizeof b, std::fabs(v - std::round(v)) < 1e-6 ? "%.0f" : "%.2f", v);
	return b;
}

}  // namespace

CameraReport probeCamera(int index) {
	CameraReport r;
	r.index = index;
	r.name = deviceName(index);
	try {
		for (cv::VideoCaptureAPIs api : cv::videoio_registry::getCameraBackends())
			r.available += (r.available.empty() ? "" : ", ") + cv::videoio_registry::getBackendName(api);
		cv::VideoCapture cap;
		if (!cap.open(index)) { r.error = "The camera didn't open (it may be in use by another app, or need a driver)."; return r; }
		r.opened = true;
		r.backend = cap.getBackendName();
		for (const auto& c : kProbeControls) {
			CameraReport::Control k;
			k.name = c.name;
			k.value = cap.get(c.prop);
			k.readable = !std::isnan(k.value) && k.value != -1;
			k.settable = k.readable && cap.set(c.prop, k.value);   // its own value back: changes nothing
			r.controls.push_back(k);
		}
		for (const auto& m : kProbeModes) {
			CameraReport::Mode mode;
			mode.askW = m.w; mode.askH = m.h; mode.askFps = m.fps;
			cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
			cap.set(cv::CAP_PROP_FRAME_WIDTH, m.w);
			cap.set(cv::CAP_PROP_FRAME_HEIGHT, m.h);
			cap.set(cv::CAP_PROP_FPS, m.fps);
			mode.measured = timedRate(cap);
			mode.w = int(cap.get(cv::CAP_PROP_FRAME_WIDTH));
			mode.h = int(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
			mode.fps = cap.get(cv::CAP_PROP_FPS);
			r.modes.push_back(mode);
		}
		cap.release();
		// Then as VST really opens it: the opening policy, shutter step included.
		auto src = open(std::to_string(index));
		if (src->isOpened()) { r.policy = src->facts(); r.notes = src->notes(); }
		else r.notes.push_back("Opening it the normal way afterwards failed.");
		src->release();
	} catch (const std::exception& e) {
		r.notes.push_back(std::string("Stopped by an error: ") + e.what());
	}
	return r;
}

std::string CameraReport::markdown() const {
	std::ostringstream o;
	o << "**Camera** " << index << (name.empty() ? "" : " (" + name + ")") << ", backend " << (backend.empty() ? "?" : backend) << "\n\n";
	if (!available.empty()) o << "Camera backends in this build: " << available << "\n\n";
	if (!opened) { o << error << "\n"; return o.str(); }
	o << "| Asked | Got | Reports | Sent |\n| --- | --- | --- | --- |\n";
	for (const Mode& m : modes)
		o << "| " << m.askW << "x" << m.askH << " @ " << number(m.askFps) << " | " << m.w << "x" << m.h << " | " << number(m.fps)
		  << " fps | " << (m.measured > 0 ? number(std::round(m.measured)) + " fps" : std::string("nothing")) << " |\n";
	o << "\n| Control | Value | Reads | Sets |\n| --- | --- | --- | --- |\n";
	for (const Control& k : controls)
		o << "| " << k.name << " | " << (k.readable ? number(k.value) : "") << " | " << (k.readable ? "yes" : "no") << " | " << (k.settable ? "yes" : "no") << " |\n";
	const char* shutter = policy.shutter == Shutter::Fixed ? "fixed" : policy.shutter == Shutter::Automatic ? "automatic"
		: policy.shutter == Shutter::CantSet ? "can't set" : "unknown";
	o << "\n**As VST opens it:** " << policy.width << "x" << policy.height << ", reports " << number(policy.fps) << " fps, sent about "
	  << number(std::round(policy.measuredFps)) << " fps, shutter " << shutter;
	if (policy.shutter == Shutter::Fixed && policy.shutterUs > 0) o << " at 1/" << std::lround(1e6 / policy.shutterUs) << " s";
	o << (policy.codec.empty() ? "" : ", " + policy.codec) << "\n";
	for (const std::string& n : notes) o << "- " << n << "\n";
	return o.str();
}

}  // namespace frameio
