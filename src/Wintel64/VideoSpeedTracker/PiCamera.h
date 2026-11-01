// Raspberry Pi ribbon-cable (CSI) cameras: the Global Shutter Camera (IMX296), Camera Module 3 (IMX708, also Wide and
// NoIR) and Camera Module v2 / v2 NoIR (IMX219), on Raspberry Pi OS Bookworm or later.
//
// Those cameras are driven by libcamera, which OpenCV's V4L2 backend can't read and whose GStreamer element OpenCV only
// reaches when it was built with GStreamer (the release builds aren't).  So VST runs the camera stack's own recorder,
// rpicam-vid (libcamera-vid on Bullseye), and reads raw frames from its stdout:
//
//     rpicam-vid --camera 0 -t 0 --nopreview --codec yuv420 --flush -o - --width 1280 --height 720 --framerate 60
//                --shutter 1000 [--gain G] [--autofocus-mode manual --lens-position D] [--rotation 180]
//
// Each frame is width*height*3/2 bytes of planar YUV 4:2:0 (I420), converted to BGR here.  Nothing new is linked: no
// libcamera, GStreamer or FFmpeg dependency, and the same VST binary works with or without a Pi camera.  rpicam-vid ships
// with Raspberry Pi OS (package rpicam-apps); VST_RPICAM_VID names another program to run instead (the tests use a
// stand-in that plays the sample video).
//
// A Pi camera is named like any other source, so it works with --input, VST Studio's recent list and the run manifest:
//
//     picam                                   camera 0 with the defaults below
//     picam:1                                 camera 1 (a Compute Module or Pi 5 has two connectors)
//     picam:0?fps=60&shutter=1000&gain=0&focus=0.25&size=1280x720&rotate=180
//
//   fps      frames per second.  Default 60, or less when the camera can't do 60 at that size (a v2 tops out at ~47).
//   shutter  exposure time in microseconds, fixed so fast cars don't smear.  Default 1000 (1/1000 s); 0 = automatic.
//            The camera raises its gain to make up for the short exposure (or uses `gain` when that is set).
//   gain     analogue gain, e.g. 4.  Default 0 = automatic.
//   focus    lens position in dioptres (1 / distance in metres), for Camera Module 3's motorised lens; the focus is
//            locked there for the whole run, never hunting when a car passes.  Default 0.25 (4 m / 13 ft), which with
//            Camera Module 3's lens keeps roughly 2 m to beyond 30 m sharp.  The Global Shutter Camera and v2 focus by
//            hand on the lens, so it is ignored for them.
//   size     picture size rpicam-vid delivers.  Default 1280x720, VST's working size, so no scaling is needed.  The
//            width must be a multiple of 64 and the height of 16 (the camera pads other sizes).
//   rotate   0 or 180, for a camera mounted upside down.
//
// The frame rate is what speeds are computed from, and rpicam-vid's output carries no timestamps, so after starting the
// camera VST times the first second of frames.  If they arrive more than 10% slower than asked (a sensor mode that can't
// keep up), that measured rate is reported instead, with a line saying so.
//
// Linux and macOS build this; only a Raspberry Pi has rpicam-vid.  On Windows every call reports that Pi cameras need VST
// running on the Pi itself.
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <vector>

namespace picam {

// The parsed form of a "picam..." source name.  0 in fps means "60, or the camera's best below that".
struct Settings {
	int camera = 0;
	int width = 1280, height = 720;
	double fps = 0;
	int shutterUs = 1000;
	double gain = 0;
	double focus = 0.25;        // dioptres; only sent to cameras with a motorised lens (or when given explicitly)
	bool focusGiven = false;
	int rotation = 0;
};

// One camera from `rpicam-vid --list-cameras`.
struct Camera {
	int index = 0;
	std::string sensor;          // "imx296", "imx708_wide_noir", ...
	std::string model;           // "Global Shutter Camera", "Camera Module 3 Wide NoIR", ...
	bool autofocus = false;      // a motorised lens (Camera Module 3)
	struct Mode { int width, height; double fps; };
	std::vector<Mode> modes;     // sensor modes, as listed
	// The fastest frame rate of any sensor mode at least width x height (0 if none is that big, or no modes were listed).
	double maxFps(int width, int height) const;
};

// True for "picam" and "picam:<anything>".
bool isName(const std::string& name);
// Fills `out` from a source name.  On a bad name returns false and says what's wrong in `error`.
bool parse(const std::string& name, Settings& out, std::string& error);
// The source name for these settings, leaving out what is at its default: picam:0, picam:0?fps=30&shutter=500, ...
// parse(name(s)) gives back s.
std::string name(const Settings& s);
// The short name for the UI and the log: "Pi camera 0".
std::string label(const std::string& name);
// The friendly model name for a sensor, e.g. imx296 -> "Global Shutter Camera"; the sensor name itself if unknown.
std::string modelName(const std::string& sensor);
// Parses the text `rpicam-vid --list-cameras` prints (exposed for tests).
std::vector<Camera> parseList(const std::string& text);
// The arguments for rpicam-vid (without the program), for these settings at this frame rate; `cam` may be null.
std::vector<std::string> commandArgs(const Settings& s, double fps, const Camera* cam);
// The frame rate to ask for: s.fps when given, else 60 capped by what the camera's modes allow at s.width x s.height.
double chooseFps(const Settings& s, const Camera* cam);

// Whether this looks like a Raspberry Pi (/proc/device-tree/model), for friendlier messages.
bool onRaspberryPi();
// The recorder to run: $VST_RPICAM_VID, else rpicam-vid or libcamera-vid on PATH; "" when there is none.
std::string findTool();
// What to tell someone who has no recorder to run (no rpicam-apps, or not a Pi at all).
std::string noToolMessage();
// The cameras attached, from `rpicam-vid --list-cameras`.  `ok` is false (and `error` says why) when the listing
// couldn't be run at all; an empty list with ok == true means rpicam-vid ran and found no camera.
std::vector<Camera> list(bool& ok, std::string& error);

// One running rpicam-vid, read frame by frame.  Not copyable; release() (or the destructor) stops the process.
class Reader {
public:
	Reader() = default;
	~Reader();
	Reader(const Reader&) = delete;
	Reader& operator=(const Reader&) = delete;

	// Starts the camera named `name` and waits for its first frames (up to ~10 s, then 1.5 s settling and timing its rate).
	// Returns false with a plain-language `error()` when it can't.
	bool open(const std::string& name);
	bool isOpened() const { return pid_ > 0; }
	// The next frame, as BGR.  Returns false if rpicam-vid stopped, or sent nothing for 5 s.
	bool read(cv::Mat& bgr);
	void release();

	int width() const { return settings_.width; }
	int height() const { return settings_.height; }
	double fps() const { return fps_; }
	const Camera& camera() const { return camera_; }
	const std::string& error() const { return error_; }
	// Lines worth showing once the camera is open: the model and settings, and any frame rate fallback.
	const std::vector<std::string>& notes() const { return notes_; }

private:
	bool readRaw(int timeoutMs);
	std::string failure(const std::string& fallback);

	Settings settings_;
	Camera camera_;
	double fps_ = 0;
	int pid_ = -1;
	int fd_ = -1;
	std::string logPath_;
	std::vector<unsigned char> buf_;
	std::string error_;
	std::vector<std::string> notes_;
};

}  // namespace picam
