// Video plumbing for the GUI thread: OpenGL textures fed from cv::Mat, and a seekable Player for previewing a
// file, a camera or a stream while the zones are set up and calibrated.  Tracking itself runs in Tracker.
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <cstdint>
#include <memory>
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
// displayName for the UI: the same, except that the bundled 2016 sample says so ("Sample video (Locust Ave, 2016)"),
// so a preview of it is never mistaken for the user's own camera.
std::string friendlySourceName(const std::string& path);
// The same, cut to about `max` characters with an ellipsis in the middle, for headers and buttons (long URLs).
std::string shortSourceName(const std::string& path, size_t max = 44);
bool isVideoPath(const std::string& path);
bool isImagePath(const std::string& path);

// Frames are delivered at VST's 1280x720 working size so overlays use config coordinates directly.
//
// Nothing that can be slow runs on the GUI thread.  A camera can take seconds to open (or never answer), each read
// waits for the camera's next frame, and OpenCV serialises some backend work, so a file opened while a camera is
// still opening would wait for it.  A reader thread (LiveReader in Media.cpp) therefore does every open: for a camera
// or stream it then reads continuously and keeps only the newest frame, which tick() picks up without waiting; for a
// file it hands the opened capture back, and the GUI thread steps through it (reads and seeks are quick).  Photos
// load directly.  While an open is pending, opening() is true and the UI can show "Connecting…" with a Cancel.  close() (and Cancel, and opening something else) abandons the reader at
// once: it stops after its current call returns and releases the device itself, so a hung driver costs a thread,
// never the window.
class Player {
public:
	~Player();
	// Returns false only when a photo can't be read or the opener thread can't start.  A video, camera or stream's
	// failure arrives later through tick() as error, with opening() going false and isOpen() staying false.
	bool open(const std::string& path);
	void close();
	bool isOpen() const { return opened_; }
	bool opening() const { return live_ && !opened_ && error.empty(); }
	double openingSeconds(double now) const;   // how long the current live open has been going
	// close(), then wait up to maxS for an abandoned reader to let go of its device, for when something else is about
	// to open it (the tracker).  Usually one frame's wait; a hung driver is given up on after maxS.
	void closeAndWait(double maxS);
	struct LiveReader;                   // a camera or stream's reader thread state (Media.cpp)
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
	bool tickLive();
	std::shared_ptr<LiveReader> live_;   // the reader thread's shared state, for cameras and streams
	std::uint64_t liveSeq_ = 0;          // the newest live frame already taken
	std::unique_ptr<cv::VideoCapture> cap_;   // files, once the reader has opened them
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
