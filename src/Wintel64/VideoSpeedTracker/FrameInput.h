// Frame input for VST, as a port and its adapters.  Every frame that comes into VST, whether the tracker's, VST Studio's
// preview, the Live tracking feed the setup pages show, or the frames ProcessHiLites edits, comes through this.
//
//   port      frameio::FrameSource   read the next frame, describe the source, release it.  Nothing above it knows or
//                                    cares what kind of device or file is behind it.
//   adapters  CaptureSource          cv::VideoCapture: video files, USB cameras and network streams
//             PiCameraSource         a Raspberry Pi ribbon camera, through rpicam-vid (picam::Reader, PiCamera.h)
//             ImageSource            a still photo (planning the zones before a camera is up)
//             FeedSource             frames pushed in by something that already has them (the running tracker's,
//                                    handed to VST Studio's setup pages, so they don't open the camera a second time)
//   factory   frameio::open(name)     picks and opens the adapter for a source name, applying the one opening policy:
//                                    hardware decoding, USB cameras asked for 1280x720 MJPG at 60 fps, and stream
//                                    timeouts.  The engine and VST Studio's preview both open through it, so a preview
//                                    shows the camera in the same mode the run will get.
//
// What the rest of VST works on is the "working frame": 1280x720 (the canvas every VST.cfg coordinate refers to),
// turned by levelDegrees.  frameio::toWorking() and frameio::readWorking() make one from whatever a source delivers.
//
// Source names are the ones the command line and VST.cfg have always taken: a file path, "0" (a USB camera),
// "picam[:n][?settings]" (PiCamera.h), or a URL.  classify() is the one place that tells them apart.
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <memory>
#include <string>
#include <vector>

namespace frameio {

// The working frame's size: Globals::FrameWidth x Globals::FrameHeight (the engine checks they agree).
constexpr int kWorkingWidth = 1280, kWorkingHeight = 720;

enum class Kind {
	None,       // "" or nothing open
	File,       // a recorded video
	Image,      // a still photo (.jpg, .jpeg, .png, .bmp)
	Camera,     // a USB camera by index ("0".."999")
	PiCamera,   // "picam..."
	Stream,     // anything with "://": rtsp, http, ...
	Feed,       // frames pushed in by the host (FeedSource); never named, so classify() doesn't return it
};

// What kind of source a name is.  The engine's and VST Studio's rules for this used to be separate copies.
Kind classify(const std::string& name);
// A camera of either kind, or a stream: it runs at its own pace, has no end, and is reconnected after a dropout.
bool isLive(Kind kind);
bool isCamera(Kind kind);   // Camera or PiCamera
bool isImagePath(const std::string& name);
// The name with any password removed, safe for the log, the run manifest and the UI: user:password@ in a URL, and query
// parameters named pwd or pw or with "pass" in their name (Foscam and many other IP cameras take ?user=admin&pwd=secret).
// Paths and camera indexes come back as they are.
std::string safeName(const std::string& name);
// A short name for the log: "camera 0", "Pi camera 0", or safeName().
std::string label(const std::string& name);

// The facts about an open source that the UI and the run report.
struct Facts {
	int width = 0, height = 0;   // as delivered, before toWorking()
	double fps = 0;              // 0 when unknown or implausible
	int frames = 0;              // frame count, 0 for anything live (and some H.264 files)
	std::string codec;           // fourcc as text, "" when unknown
};

// The port.  Property ids are VideoCapture's (cv::CAP_PROP_*), so existing calls read the same on every adapter.
class FrameSource {
public:
	virtual ~FrameSource() = default;
	FrameSource() = default;
	FrameSource(const FrameSource&) = delete;
	FrameSource& operator=(const FrameSource&) = delete;

	virtual Kind kind() const = 0;
	virtual bool isOpened() const = 0;
	// The next frame as the source delivers it (any size, BGR).  False at the end of a file, or when a live source stops.
	virtual bool read(cv::Mat& frame) = 0;
	// Lets go of the device or file; isOpened() is false afterwards.  Safe to call more than once.
	virtual void release() = 0;
	virtual double get(int prop) const = 0;
	// VideoCapture's set(), for seeking (CAP_PROP_POS_FRAMES) a file; the other adapters return false.
	virtual bool set(int prop, double value) { (void)prop; (void)value; return false; }
	// Why the open failed, in plain language, where the adapter knows (Pi cameras, photos); else "".
	virtual std::string error() const { return ""; }
	// Lines worth logging once it's open: a Pi camera's model and settings, a frame rate fallback.
	virtual std::vector<std::string> notes() const { return {}; }
	// get()'s answers gathered up, with an implausible fps (some cameras say 0 or 1000) reported as 0.
	Facts facts() const;
};

// How frameio::open() opens things.  The defaults are what both the engine and VST Studio use.
struct OpenOptions {
	// Let FFmpeg use the graphics chip's video decoder where there is one (Quick Sync / VA-API, D3D11, ...); falls back to
	// software by itself.  Default: on, unless VST_HW_DECODE=0 is in the environment.
	bool hwDecode = true;
	bool hwDecodeFromEnv = true;      // let VST_HW_DECODE=0 turn hwDecode off
	// FFmpeg waits 30 s by default on a stream that doesn't answer or stalls; shorter limits notice a dropout sooner and
	// keep Stop from hanging on a dead camera.
	int streamOpenTimeoutMs = 5000;
	int streamReadTimeoutMs = 10000;
	// A USB camera opens in its default mode, often 640x480 at 30 fps.  Ask for this instead (MJPG, the only way most
	// cameras manage 720p60 over USB 2, and cheaper to read than raw YUYV).  A camera without the mode picks its nearest.
	int cameraWidth = kWorkingWidth, cameraHeight = kWorkingHeight;
	double cameraFps = 60;
};

// Opens `name` with the adapter for its kind.  Never returns null: check isOpened(), and error() for why not.
std::unique_ptr<FrameSource> open(const std::string& name, const OpenOptions& options = OpenOptions());
// A source with nothing open (Kind::None), to hold until something is opened.
std::unique_ptr<FrameSource> closed();

// The adapter for frames the host already has.  push() hands in the newest one (it is shared, not copied: give it a Mat
// nobody writes into afterwards); read() returns it once, then false until the next push().  It never blocks, and
// stays open until release().  `about` gives facts() for the source the frames really come from.
class FeedSource : public FrameSource {
public:
	explicit FeedSource(const Facts& about = Facts()) : about_(about) {}
	void push(const cv::Mat& frame) { latest_ = frame; fresh_ = !frame.empty(); }
	void describe(const Facts& about) { about_ = about; }
	Kind kind() const override { return Kind::Feed; }
	bool isOpened() const override { return open_; }
	bool read(cv::Mat& frame) override;
	void release() override { open_ = false; fresh_ = false; latest_.release(); }
	double get(int prop) const override;
private:
	Facts about_;
	cv::Mat latest_;
	bool fresh_ = false, open_ = true;
};

// Scales a delivered frame to the 1280x720 working size (INTER_AREA, as the engine always has).  No copy when it's
// already that size.
void toWorking(cv::Mat& frame);
// read(), then toWorking(), then turned by levelDegrees (Level.h): the frame the tracker, the zones and the preview all
// work on.  False when read() is.
bool readWorking(FrameSource& source, cv::Mat& frame, double levelDegrees);

}  // namespace frameio
