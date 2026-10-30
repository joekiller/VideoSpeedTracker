// Embedding interface for the VST tracker.
//
// videoSpeedTracker.cpp is a 2016 command-line program built around file-scope globals, imshow() and waitKey().
// Compiled with VST_EMBEDDED defined (the GUI target does this), its main() becomes vst::run(), and the places
// where it would open a window, poll the keyboard or exit the process call the Hooks below instead.  The
// command-line build doesn't define VST_EMBEDDED and is unchanged.
//
// Because the engine keeps its state in globals, only one run() may be active per process at a time.  run()
// resets every global before it starts, so back-to-back runs (a batch of files) are independent.
#pragma once
#include <functional>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace vst {

// A vehicle box the tracker drew this frame pair, in analysis-box coordinates.
struct TrackBox {
	bool leftToRight;
	cv::Rect box;
	int speed;          // > 0 once the vehicle has crossed the second speed line, else 0
};

// One analysed frame pair.  Every Mat is only valid during the callback; copy what you keep.
struct FrameInfo {
	const cv::Mat& frame;            // full 1280x720 frame (the second of the pair), unannotated
	const cv::Mat& motion;           // thresholded frame difference, analysis-box sized, CV_8UC1
	cv::Rect analysisBox;
	int frameNumber;                 // frame index within the current video
	const std::vector<TrackBox>& boxes;
};

// A vehicle the tracker logged to the stats csv (same rule as the csv: speed >= 18 and a clean track).
struct VehicleEvent {
	bool leftToRight;
	bool wrongWay;                   // travelling against LegalDirection (never set when LegalDirection = Both)
	std::string compass;             // L2RDirection or R2LDirection from the config, e.g. "NE"
	int speed;
	int frame;                       // frame number when logged (csv "Frame")
	int startFrame, endFrame;        // frames at the two speed lines
	int startPixel, endPixel;        // front bumper x at the two speed lines, analysis-box coordinates
	int area;
};

struct Hooks {
	std::function<void(const FrameInfo&)> onFrame;
	std::function<void(const VehicleEvent&)> onVehicle;
	// Replaces waitKey(): return a key code the engine understands (27 stops the run) or -1.  Called once per
	// frame pair; the host can block here to pause.
	std::function<int()> pollKey;
	std::function<void(const std::string& line)> onLog;   // stdout / stderr of the engine, one line per call
};

// Runs the tracker as if from the command line, e.g. {"--input", "a.mp4", "--config", "VST.cfg", "--stats", "s.csv"}.
// --headless is implied.  An --input of all digits opens that camera index; URLs (rtsp://, http://) work as
// files do.  Returns the process exit status the CLI would have returned.
int run(const std::vector<std::string>& args, const Hooks& hooks);

}  // namespace vst
