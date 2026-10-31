// Software levelling (src/Wintel64/VideoSpeedTracker/Level.h).
//
//   level_test
//
// Draws a "curb" through the frame's centre at a known tilt, as a camera rolled on its mount would see a level street,
// then turns the frame by the slope angle of that line measured the way VST Studio's "Use the curb" tool measures it
// (atan2 of the two ends in frame coordinates, y down; Canvas.cpp levelCorrection()).  The curb must come out level:
// its rows at the left and right of the frame agree to within a pixel.  Also: both signs, the no-op cases, black
// corners, and the frame staying 1280x720.
#include "Level.h"
#include <cmath>
#include <cstdio>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const double kPi = 3.14159265358979323846;

// Mean row of the white pixels in column x (-1 if none).
static double curbRow(const cv::Mat& f, int x) {
	double sum = 0; int n = 0;
	for (int y = 0; y < f.rows; y++) if (f.at<cv::Vec3b>(y, x)[0] > 128) { sum += y; n++; }
	return n ? sum / n : -1;
}

// A frame with a 3 px white line through the centre, `slopeDeg` downhill to the right on screen.
static cv::Mat tiltedCurb(double slopeDeg) {
	cv::Mat f(720, 1280, CV_8UC3, cv::Scalar::all(60));
	double k = std::tan(slopeDeg * kPi / 180);
	cv::line(f, cv::Point(0, int(std::lround(360 - 640 * k))), cv::Point(1279, int(std::lround(360 + 639 * k))), cv::Scalar::all(255), 3, cv::LINE_AA);
	return f;
}

int main() {
	for (double slope : { 4.0, -2.5, 0.7, -12.0 }) {
		cv::Mat f = tiltedCurb(slope);
		// What the curb tool reads from two dots laid on the line 800 px apart.
		double k = std::tan(slope * kPi / 180);
		double measured = std::atan2((240 + 400 * k) - (240 - 400 * k), 800.0) * 180 / kPi;
		CHECK(std::fabs(measured - slope) < 1e-9);
		double before = curbRow(f, 340) - curbRow(f, 940);
		CHECK(std::fabs(before) > 5);   // tilted to start with
		levelFrame(f, measured);
		CHECK(f.cols == 1280 && f.rows == 720);
		double l = curbRow(f, 340), r = curbRow(f, 940);
		CHECK(l > 0 && r > 0);
		CHECK(std::fabs(l - r) < 1.0);
		CHECK(std::fabs(l - 360) < 1.5);   // turned about the centre, so the line through it stays there
		std::printf("slope %+.1f deg: %.1f px off level before, rows %.2f / %.2f after\n", slope, before, l, r);
	}
	// The corners the turn uncovers are black, never smeared picture.
	cv::Mat grey(720, 1280, CV_8UC3, cv::Scalar::all(200));
	levelFrame(grey, 5);
	CHECK(grey.at<cv::Vec3b>(0, 0) == cv::Vec3b(0, 0, 0));
	CHECK(grey.at<cv::Vec3b>(719, 1279) == cv::Vec3b(0, 0, 0));
	CHECK(grey.at<cv::Vec3b>(360, 640) == cv::Vec3b(200, 200, 200));
	// Zero (and a hair off zero) leave the very same pixels; an empty frame is left alone.
	cv::Mat same = tiltedCurb(3), orig = same;
	levelFrame(same, 0);
	CHECK(same.data == orig.data);
	levelFrame(same, kLevelEpsilonDegrees / 2);
	CHECK(same.data == orig.data);
	cv::Mat none;
	levelFrame(none, 3);
	CHECK(none.empty());

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
