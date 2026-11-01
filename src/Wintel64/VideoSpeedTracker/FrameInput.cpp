// The frame input port's adapters and its opening policy (see FrameInput.h).
#include "FrameInput.h"
#include "Level.h"
#include "PiCamera.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio/registry.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>

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
	return f;
}

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

private:
	void openCamera(int index, const OpenOptions& o) {
		if (!cap_.open(index)) return;
		if (o.cameraWidth <= 0 || o.cameraHeight <= 0) return;
		// Each set is a request; a camera without the mode keeps its nearest one, and facts() reports what it chose.
		cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
		cap_.set(cv::CAP_PROP_FRAME_WIDTH, o.cameraWidth);
		cap_.set(cv::CAP_PROP_FRAME_HEIGHT, o.cameraHeight);
		if (o.cameraFps > 0) cap_.set(cv::CAP_PROP_FPS, o.cameraFps);
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
