// USB cameras as they really are: what the system calls each one, every mode its driver lists, and the mode VST picks
// for timing.  No list of known models is needed for any of it; the registry at the bottom only adds what a driver
// can't say (the lens's field of view, a known quirk) and the suggestions on "Which camera?".
//
//   list() / describe()  ask the operating system, without opening a picture stream:
//                          Linux    V4L2 ioctls on /dev/videoN (QUERYCAP, ENUM_FMT, ENUM_FRAMESIZES, ENUM_FRAMEINTERVALS,
//                                   QUERYCTRL), USB ids from sysfs, a stable id from /dev/v4l/by-id
//                          Windows  Media Foundation (MFEnumDeviceSources, each source's native media types)
//                          macOS    AVFoundation (AVCaptureDevice formats and frame rate ranges), WebcamMac.mm
//                        in the order cv::VideoCapture(index) numbers them on that platform's default backend, so
//                        `index` is what VST opens.  Anything else lists nothing, and opening falls back to asking the
//                        camera for 1280x720 at 60 fps and taking what it gives (the old policy).
//   best()               the mode to time cars with, from the modes listed (rules beside it in Webcam.cpp)
//   Settings             a user's own choice for one camera, carried in the source name the way Pi cameras' are:
//                        "0" (VST picks), "0?w=1920&h=1080&fps=30&fmt=MJPG&shutter=1000".  Changing them changes how the
//                        camera sends pictures, never the street: deviceKey() (FrameInput.h) names the camera without
//                        them, so marks, distance and calibration stay with the camera.
//   recognize()          a registry entry for a device, matched on its USB ids or its name; null for most cameras,
//                        which is fine: everything above works without it.
//
// No OpenCV here, so the Windows and macOS listers can be syntax-checked without an OpenCV for those platforms.
#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace webcam {

// One way a camera can send pictures.  `format` is a fourcc as text: MJPG, YUYV (YUY2 on Windows is reported as YUYV),
// NV12, H264, ... as the driver names it.
struct Mode {
	int width = 0, height = 0;
	double fps = 0;
	std::string format;
	bool valid() const { return width > 0 && height > 0; }
	// A format OpenCV's camera backends turn into pictures.  H.264/HEVC streams and raw Bayer are listed by some cameras
	// but come out of VideoCapture as nothing, or as garbage.
	bool usable() const;
	// 16:9 within 2% (1280x720, 1920x1080, 640x360, 848x480): the working frame's shape, so nothing is stretched.
	bool wide() const;
	// "1280 × 720 at 60 fps (MJPG)"
	std::string text() const;
	bool operator==(const Mode& o) const { return width == o.width && height == o.height && format == o.format && (fps - o.fps) * (fps - o.fps) < .01; }
};

// A camera control the driver offers, with its range where the system says (V4L2); for the camera report.
struct Control {
	std::string name;
	double min = 0, max = 0, step = 0, value = 0;
};

struct Device {
	int index = -1;          // what cv::VideoCapture(index) opens
	std::string name;        // the device's own name: "C922 Pro Stream Webcam", "FaceTime HD Camera", "Iriun Webcam"
	std::string id;          // stays the same across a replug or reboot where the system gives one; else ""
	std::string backend;     // the lister that found it: V4L2, MSMF, AVFOUNDATION
	int vendor = 0, product = 0;   // USB vendor and product ids; 0 when unknown (built-in, virtual)
	std::vector<Mode> modes; // every mode listed, in the driver's order; empty when they couldn't be read
	std::vector<Control> controls;
	std::string error;       // why the modes couldn't be read (another app has it, on Windows), else ""
	bool isVirtual() const;  // OBS Virtual Camera, v4l2loopback, phone apps' drivers: a picture made by other software
	// "C922 Pro Stream Webcam" or, with no name, "USB camera 2".
	std::string label() const;
	// The highest frame rate it lists at 720p or better (else at any size), for the menu: 0 when nothing was listed.
	double topFps() const;
};

// Every camera on this machine, by index.  Takes milliseconds on Linux and macOS and up to about a second on Windows
// (each source is activated to read its modes); call it off the GUI thread.  Never throws.
std::vector<Device> list();
// The camera at `index`, or a Device with that index, no name and no modes when there's none (or no lister).
Device describe(int index);
// The name list() or describe() last found for camera `index`, without asking the system again: for labels drawn every
// frame.  "" when it hasn't been listed (or has no name).
std::string knownName(int index);
// The platform's lister itself (WebcamList.cpp, WebcamMac.mm); list() calls it and remembers the names.
std::vector<Device> listNow();
// Held while listing and by frameio's camera open (FrameInput.cpp), so a listing never activates a camera (Windows)
// in the moment another thread is opening it.  Recursive: the open lists the camera it is about to open.
std::recursive_mutex& deviceLock();

// The best mode for timing cars, from `modes` (empty Mode when none is usable).  Frames matter more than pixels for
// VST: speed is counted in frames, and the working frame is 1280x720 whatever comes in.  So, in order:
//   1. 55 fps or more at 720p or better        (C922 720p60, Facecam 1080p60, Brio 720p90)
//   2. 55 fps or more at 480p or better        (PS3 Eye, cheap 60 fps cameras: halves the timing error of 30 fps)
//   3. 24 fps or more at 720p or better        (C920, C270, most webcams and phones)
//   4. 24 fps or more at 480p or better
//   5. anything else, the most frames x lines
// then, within a class: 16:9 (no stretching), more frames (up to 120: past that the decode costs more than it
// gains), the height nearest 720 from above (1080 next; 4K costs a lot to decode for nothing), and MJPG before raw
// (it fits more frames down a USB 2 cable; raw YUYV at 720p usually lists only 10 fps).
Mode best(const std::vector<Mode>& modes);
// One plain sentence on why best() chose `m` from `modes`, for the settings popup.
std::string whyBest(const Mode& m, const std::vector<Mode>& modes);
// The modes worth offering a person, best first: one per size and frame rate (the better format of two), usable only,
// at least 320 wide, at most `limit` of them.
std::vector<Mode> offered(const std::vector<Mode>& modes, size_t limit = 14);

// A user's own settings for one USB camera, in the source name.  Zero / empty fields mean "VST picks".
struct Settings {
	int camera = 0;
	int width = 0, height = 0;   // 0 = best()
	double fps = 0;              // 0 = the mode's own (best()'s, or the camera's at that size)
	std::string format;          // "" = best()'s, or MJPG
	int shutterUs = -1;          // -1 = VST's policy (1/1000 s, slower if dark, else automatic); 0 = automatic; else fixed
	bool picksMode() const { return width <= 0 || height <= 0; }
	bool isDefault() const { return picksMode() && fps <= 0 && format.empty() && shutterUs < 0; }
};
// "0".."999", or that followed by "?" and settings.  (Four digits is a file named 2016, as before.)
bool isName(const std::string& name);
// False for a name that isn't a USB camera's; unknown keys are ignored so a newer name opens on an older VST.
bool parse(const std::string& name, Settings& s);
// The name for `s`: just the index when it's all default, so the name stays "0" for everyone who never opens the popup.
std::string name(const Settings& s);
// What a change from `was` to `now` means for marks and calibration, in plain words; "" when nothing to say.  Shape
// (4:3 against 16:9) moves the marks' vertical positions; a smaller frame on some cameras is a zoomed-in crop of the
// sensor; frame rate and shutter never touch them (speeds are scaled by CalibrationFPS).
std::string changeNote(const Mode& was, const Mode& now);

// Linux: stop automatic exposure from lowering the frame rate (V4L2_CID_EXPOSURE_AUTO_PRIORITY = 0, "constant frame
// rate"), so a C920 on automatic in shade keeps sending 30 fps instead of 15 while reporting 30.  True when the camera
// took it; false where there's no such control (most platforms, many cameras).
bool holdFrameRate(int index);

// ---- the registry: suggestions and hints, never instructions -------------------------------------------------------

struct Model {
	const char* name;        // what people call it
	const char* match;       // lowercase words that must all appear in the device's name ("c922"), "" for none
	int vendor, product;     // USB ids, 0 when not matched on ids
	double hfov;             // horizontal field of view in degrees at its 16:9 modes; 0 when unknown
	const char* kind;        // "webcam", "phone", "capture", "global-shutter", "built-in", "virtual"
	const char* note;        // one plain sentence: what it's good for, or the quirk VST works around
	int suggest;             // 0 = only recognized; 1 = a good start; 2 = real-deal timing (60 fps+, global shutter)
	const char* cost;        // "about $70-100", "" when not suggested
	const char* where;       // chain stores first; "" when not suggested
};
// The model for a device, or null.  USB ids win over the name; names are matched on whole lowercase words.
const Model* recognize(const Device& d);
const Model* recognize(const std::string& name, int vendor, int product);
// Every entry, suggested ones first in order of suggest (2, then 1).
const std::vector<Model>& models();

}  // namespace webcam
