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
	cv::Point2f centre(frame.cols * .5f, frame.rows * .5f);
	cv::Mat turn = cv::getRotationMatrix2D(centre, degrees, 1.0);
	cv::Mat out;
	cv::warpAffine(frame, out, turn, frame.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(0));
	frame = out;
}
