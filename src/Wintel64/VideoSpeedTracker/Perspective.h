// Lane lines at an angle, and a street that recedes across the picture.
//
// VST began with a camera looking square across a level street: each lane is one row of the picture and a car's front
// bumper moves the same number of pixels for every foot it travels.  Neither holds when the camera is aimed along the
// street a little, as from a window that doesn't face it squarely:
//
//   1. The lane runs at an angle.  VST looks for each direction's cars only above that lane's hubcap line (anything
//      below it is the near curb, sidewalk or parked cars), so a level line cuts the bottom off cars at one end and
//      lets the sidewalk in at the other.  A lane line now has a right end (L2RStreetYRight / R2LStreetYRight) as well
//      as a left (L2RStreetY / R2LStreetY), and everything below the slanted line is ignored.
//
//   2. The street recedes: its far end is farther from the camera, so a car there covers fewer pixels per foot.  Time
//      between the two speed lines doesn't care (calibration timed a car between the same two lines in the same
//      view), but short-view timing (subFrameTiming) fits a straight line to the front bumper's pixel position against
//      frame number, and on a receding street a car at constant speed doesn't move in a straight line in pixels: it
//      speeds up on screen as it comes nearer.  The fit then reads somewhat off, and a strongly curved track is thrown
//      out as "not one vehicle moving steadily".
//
//      The fix is the ground plane along the lane.  Any two lines that are parallel on the street (the lane line and
//      the curb, a lane stripe, the tops of parked cars) meet in the picture at the street's vanishing point.  Points
//      along the lane map to distance along the street by a 1D projective map that sends the vanishing point to
//      infinity.  With the two speed lines as 0 and 1 that map is fixed by three x values, the cross-ratio
//
//                   (x - xL) (xR - xV)
//          s(x) =  -------------------        xL, xR the speed lines, xV the vanishing point's x
//                   (x - xV) (xR - xL)
//
//      which is 0 at the left line, 1 at the right, and linear in feet travelled.  A car at constant speed moves at
//      constant ds/dt, so the fit is done in s and needs no other measurement: calibration still sets feet and mph.
//      The optional streetEdge = [left, right] key is that second line; leaving it off (or parallel to the lane line)
//      gives s(x) = (x - xL) / (xR - xL), exactly the old pixel fit.
//
// All y values are relative to AnalysisBoxTop and x values to AnalysisBoxLeft, like the rest of VST.cfg; "left" and
// "right" mean the analysis box's left and right edges (x = 0 and x = AnalysisBoxWidth).
#pragma once
#include <cmath>
#include <limits>

namespace perspective {

// A line through (0, yLeft) and (width, yRight).
struct Line {
	double yLeft = 0, yRight = 0;
	double width = 1;
	double at(double x) const { return yLeft + (yRight - yLeft) * x / width; }
	double slope() const { return (yRight - yLeft) / width; }
};

// Where two lines cross, as an x; NaN when they're parallel to within `minSlope` (so far away that the street doesn't
// visibly recede: 0.001 is one pixel of convergence across 1000 px).
inline double crossingX(const Line& a, const Line& b, double minSlope = 0.001) {
	double ds = a.slope() - b.slope();
	if (std::fabs(ds) < minSlope) return std::numeric_limits<double>::quiet_NaN();
	return (b.yLeft - a.yLeft) / ds;
}

// The cross-ratio above: distance along the street, 0 at xL and 1 at xR, for a vanishing point at xV (NaN = none, the
// plain linear map).  Undefined at x = xV, which a usable setup keeps outside the analysis box (see vanishingUsable).
inline double zoneFraction(double x, double xL, double xR, double xV) {
	if (std::isnan(xV)) return (x - xL) / (xR - xL);
	return (x - xL) * (xR - xV) / ((x - xV) * (xR - xL));
}

// How much farther from the camera the right speed line is than the left: the ratio of their distances (depths) along
// the camera's axis, so 1.3 means a car at the right line looks 1/1.3 the size it does at the left.  1 with no
// vanishing point.  Follows from the map above: depth grows linearly with s, and s -> infinity at xV.
inline double depthRatio(double xL, double xR, double xV) {
	if (std::isnan(xV)) return 1;
	return (xV - xL) / (xV - xR);
}

// A vanishing point can be used only off the analysis box (the map blows up there) and not so close to it that the far
// end would be over three times farther away than the near: past that a car at the far end is a few pixels long and
// the camera should be re-aimed instead.
constexpr double kMaxDepthRatio = 3;
inline bool vanishingUsable(double xV, double boxWidth, double xL, double xR) {
	if (std::isnan(xV)) return true;
	if (xV >= 0 && xV <= boxWidth) return false;
	double r = depthRatio(xL, xR, xV);
	return r > 0 && r <= kMaxDepthRatio && 1 / r <= kMaxDepthRatio;
}

}  // namespace perspective
