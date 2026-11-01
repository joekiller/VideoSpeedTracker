// Software levelling for a camera that isn't mounted square to the street.
//
// VST assumes each lane is one row of the picture: a car is tracked along the hubcap line (L2RStreetY / R2LStreetY)
// and timed by its bumper's x.  A camera rolled a few degrees on its mount tilts the lanes, so cars drift off their
// line across the frame.  levelFrame() turns the 1280x720 working frame back about its centre before anything else
// sees it: the engine does it in readFrame(), VST Studio does it for the preview, so the lines a user draws sit on
// the same pixels the tracker uses.
//
// Sign: positive degrees turns the picture counter-clockwise (OpenCV's convention, y down), which levels a street that
// runs downhill to the right on screen.  The corners the turn uncovers are black: black never changes, so the motion
// detector can't mistake them for traffic (a replicated edge would smear moving cars into them).
//
// Yaw (aiming along the street) and pitch (looking down) need no fix here: calibration measures speed in the same
// perspective it's drawn in.
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>

// Below this the turn would move no pixel by more than about half a pixel at the frame's corners, so skip the work.
constexpr double kLevelEpsilonDegrees = 0.02;

inline void levelFrame(cv::Mat& frame, double degrees) {
	if (frame.empty() || std::fabs(degrees) < kLevelEpsilonDegrees) return;
	// The affine turn about the frame's centre, written out rather than cv::getRotationMatrix2D(): OpenCV 5 moved that
	// out of imgproc into the geometry module, and these six numbers are the same on every version.
	//   [ c  s  (1-c)cx - s cy ]
	//   [-s  c  s cx + (1-c)cy ]
	const double a = degrees * CV_PI / 180, c = std::cos(a), s = std::sin(a);
	const double cx = frame.cols * .5, cy = frame.rows * .5;
	// A Matx, not Mat_<double>(2, 3) << ...: OpenCV 5 deprecates the Mat comma initializer (a warning per file on macOS).
	const cv::Matx23d turn(c, s, (1 - c) * cx - s * cy, -s, c, s * cx + (1 - c) * cy);
	cv::Mat out;
	cv::warpAffine(frame, out, turn, frame.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(0));
	frame = out;
}
