// Video plumbing for the GUI thread: OpenGL textures fed from cv::Mat, and a seekable Player for previewing a
// file, a camera or a stream while the zones are set up and calibrated.  Tracking itself runs in Tracker.
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>

// An RGBA OpenGL texture that follows a BGR/BGRA/gray Mat; reallocated only when the size changes.
class Texture {
public:
	Texture() = default;
	~Texture();
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;
	void update(const cv::Mat& image);
	void reset();
	bool valid() const { return id_ != 0; }
	void* imguiId() const { return (void*)(intptr_t)id_; }
	int width() const { return w_; }
	int height() const { return h_; }
private:
	unsigned id_ = 0;
	int w_ = 0, h_ = 0;
	cv::Mat rgba_;
};

// What a source is, as the rest of the app talks about it.
struct SourceInfo {
	enum Kind { None, File, Camera, Stream, Image } kind = None;
	std::string path;          // file path, camera index ("0") or URL
	std::string label;         // short name for the UI
	int width = 0, height = 0;
	double fps = 0;
	int frames = 0;            // 0 when unknown (cameras, streams, some H.264 files)
	std::string codec;
	double seconds() const { return fps > 0 && frames > 0 ? frames / fps : 0; }
};

SourceInfo::Kind classifySource(const std::string& path);
std::string displayName(const std::string& path);
bool isVideoPath(const std::string& path);
bool isImagePath(const std::string& path);

// Frames are delivered at VST's 1280x720 working size so overlays use config coordinates directly.
class Player {
public:
	bool open(const std::string& path);
	void close();
	bool isOpen() const { return opened_; }
	const SourceInfo& info() const { return info_; }
	bool seekable() const { return info_.kind == SourceInfo::File && info_.frames > 0; }
	// Reads the frame at index (files) or the next frame (cameras, streams).  Returns false at end or on error.
	bool seek(int index);
	bool next();
	// Called each GUI frame; when playing, advances in real time.  Returns true if a new frame arrived.
	bool tick(double now);
	bool playing = false;
	int position() const { return pos_; }
	const cv::Mat& frame() const { return frame_; }
	Texture& texture() { return tex_; }
	std::string error;
private:
	bool readInto();
	cv::VideoCapture cap_;
	SourceInfo info_;
	bool opened_ = false;
	cv::Mat frame_;
	Texture tex_;
	int pos_ = -1;
	double lastTick_ = 0, carry_ = 0;
};

// Resizes to 1280x720 like the tracker's readFrame().
void toWorkingSize(cv::Mat& frame);

// Opens a file or folder with the platform's default handler (Explorer/Finder/xdg-open).
void openWithSystem(const std::string& path);
