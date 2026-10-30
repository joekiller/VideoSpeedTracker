#include "Media.h"
#include <GLFW/glfw3.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
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
	if (path.size() < 4 && std::all_of(path.begin(), path.end(), [](char c) { return isdigit((unsigned char)c) != 0; }))
		return SourceInfo::Camera;
	if (path.find("://") != std::string::npos) return SourceInfo::Stream;
	if (isImagePath(path)) return SourceInfo::Image;
	return SourceInfo::File;
}

std::string displayName(const std::string& path) {
	switch (classifySource(path)) {
	case SourceInfo::Camera: return "Camera " + path;
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

void toWorkingSize(cv::Mat& frame) {
	if (!frame.empty() && (frame.cols != 1280 || frame.rows != 720))
		cv::resize(frame, frame, cv::Size(1280, 720), 0, 0, cv::INTER_AREA);
}

bool Player::open(const std::string& path) {
	close();
	error.clear();
	info_ = SourceInfo();
	info_.kind = classifySource(path);
	info_.path = path;
	info_.label = displayName(path);
	if (info_.kind == SourceInfo::Image) {
		// A still photo of the view is enough to plan zones before any camera is installed.
		frame_ = cv::imread(path);
		if (frame_.empty()) { error = "Can't read image " + path; return false; }
		info_.width = frame_.cols;
		info_.height = frame_.rows;
		toWorkingSize(frame_);
		tex_.update(frame_);
		opened_ = true;
		pos_ = 0;
		return true;
	}
	bool ok = info_.kind == SourceInfo::Camera ? cap_.open(std::stoi(path)) : cap_.open(path);
	if (!ok || !cap_.isOpened()) {
		error = info_.kind == SourceInfo::Camera ? "No camera at index " + path
			: "Can't open " + info_.label + ". Try converting it: ffmpeg -i in.ext -c:v libx264 out.mp4";
		return false;
	}
	opened_ = true;
	info_.width = int(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
	info_.height = int(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
	info_.fps = cap_.get(cv::CAP_PROP_FPS);
	if (!(info_.fps > 0 && info_.fps < 1000)) info_.fps = 0;
	if (info_.kind == SourceInfo::File) info_.frames = std::max(0, int(cap_.get(cv::CAP_PROP_FRAME_COUNT)));
	int fourcc = int(cap_.get(cv::CAP_PROP_FOURCC));
	if (fourcc > 0)
		for (int i = 0; i < 4; i++) {
			char ch = char((fourcc >> (8 * i)) & 255);
			if (isprint((unsigned char)ch)) info_.codec += ch;
		}
	return next();
}

void Player::close() {
	cap_.release();
	opened_ = false;
	playing = false;
	frame_.release();
	pos_ = -1;
	carry_ = 0;
}

bool Player::readInto() {
	cv::Mat f;
	if (!cap_.read(f) || f.empty()) { playing = false; return false; }
	toWorkingSize(f);
	frame_ = f;
	tex_.update(frame_);
	pos_++;
	return true;
}

bool Player::next() {
	if (!opened_ || info_.kind == SourceInfo::Image) return false;
	return readInto();
}

bool Player::seek(int index) {
	if (!opened_) return false;
	if (!seekable()) return next();
	index = std::clamp(index, 0, std::max(0, info_.frames - 1));
	if (index == pos_ + 1) return readInto();   // sequential step: no seek, which is slow on some codecs
	if (index == pos_) return true;
	cap_.set(cv::CAP_PROP_POS_FRAMES, index);
	pos_ = index - 1;
	return readInto();
}

bool Player::tick(double now) {
	if (!opened_ || info_.kind == SourceInfo::Image) return false;
	double dt = lastTick_ > 0 ? now - lastTick_ : 0;
	lastTick_ = now;
	// Live sources are previewed continuously; files only while playing.
	if (info_.kind != SourceInfo::File) return next();
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
