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
#include <initializer_list>
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

// --- Working the street edge out from the camera (VST Studio's "Work it out from the camera") -------------------------
//
// Laying the street edge on the curb needs a curb to see.  Where none shows (a porch roof hides it, parked cars line it),
// the vanishing point can come from how the camera is set up instead: a pinhole camera turned `yawDeg` from facing the
// street squarely (positive: aimed toward the right, so the right end is farther away) and tipped down to look at the
// lane sees the street's direction at
//
//                         f cos(yaw)                         frame width / 2
//          xV  =  cx + ---------------------       f  =  ------------------- ,  cx = frame width / 2
//                       sin(yaw) cos(pitch)                tan(hfov / 2)
//
// with pitch = atan(height cos(yaw) / distance), the tilt that puts the lane straight ahead (the camera looks along its
// axis to the lane, distance / cos(yaw) away horizontally).  Roll is taken as levelled already (levelDegrees).

struct CameraSetup {
	double distanceFt = 0;   // from the camera to the lane, square to the street (step 2's distance)
	double heightFt = 0;     // camera above the road
	double yawDeg = 0;       // turned from square to the street; + = toward the right (right end farther)
	double hfovDeg = 0;      // lens's horizontal field of view across the whole frame
};

// Limits on the inputs: past these the answer means nothing (aimed nearly along the street, a lens that isn't one).
constexpr double kMaxYawDeg = 75, kMinHfovDeg = 10, kMaxHfovDeg = 170;
inline bool cameraSetupValid(const CameraSetup& c) {
	return c.distanceFt > 0 && c.heightFt >= 0 && std::fabs(c.yawDeg) <= kMaxYawDeg && c.hfovDeg >= kMinHfovDeg && c.hfovDeg <= kMaxHfovDeg;
}

// The vanishing point's x in frame pixels (0 at the frame's left edge) for a frame `frameWidth` wide; NaN for a camera
// square to the street (yaw under 0.1°: the street doesn't recede) or an invalid setup.
inline double vanishingFrameX(const CameraSetup& c, double frameWidth) {
	if (!cameraSetupValid(c) || std::fabs(c.yawDeg) < 0.1) return std::numeric_limits<double>::quiet_NaN();
	const double d2r = 3.14159265358979323846 / 180;
	double yaw = c.yawDeg * d2r;
	double pitch = std::atan(c.heightFt * std::cos(yaw) / c.distanceFt);
	double f = frameWidth / 2 / std::tan(c.hfovDeg * d2r / 2);
	return frameWidth / 2 + f * std::cos(yaw) / (std::sin(yaw) * std::cos(pitch));
}

// --- The same camera, projecting points: the "ghost car" ---------------------------------------------------------------
//
// To check the setup by eye, Studio draws a wireframe car of typical size on the lane (ghostCar()).  If the distance,
// height, angle or lens is wrong, the ghost comes out the wrong size or leans the wrong way next to the real cars.
// Camera coordinates as in the formula above: X along the street (+ to the right), Y down from the camera (the road is
// at Y = heightFt), Z straight out from the camera's wall toward the street (the lane is at Z = distanceFt).  The
// picture is frameWidth x frameHeight with square pixels and the lens's centre at its centre.

struct Pixel { double u = 0, v = 0; bool ok = false; };   // ok = false: behind the camera, or an invalid setup

namespace detail {
struct Rig { double cy, sy, cp, sp, f, cx, cyPix; };
inline bool rig(const CameraSetup& c, double frameWidth, double frameHeight, Rig& r) {
	if (!cameraSetupValid(c) || frameWidth <= 0 || frameHeight <= 0) return false;
	const double d2r = 3.14159265358979323846 / 180;
	double yaw = c.yawDeg * d2r, pitch = std::atan(c.heightFt * std::cos(yaw) / c.distanceFt);
	r = { std::cos(yaw), std::sin(yaw), std::cos(pitch), std::sin(pitch), frameWidth / 2 / std::tan(c.hfovDeg * d2r / 2),
		frameWidth / 2, frameHeight / 2 };
	return true;
}
}  // namespace detail

// Where world point (X, Y, Z) lands in the picture.
inline Pixel projectPoint(const CameraSetup& c, double frameWidth, double frameHeight, double X, double Y, double Z) {
	Pixel p;
	detail::Rig r;
	if (!detail::rig(c, frameWidth, frameHeight, r)) return p;
	double x1 = r.cy * X - r.sy * Z, z1 = r.sy * X + r.cy * Z;     // turn (yaw)
	double y2 = r.cp * Y - r.sp * z1, z2 = r.sp * Y + r.cp * z1;  // tip down (pitch)
	if (z2 <= 1e-6) return p;
	p.u = r.cx + r.f * x1 / z2;
	p.v = r.cyPix + r.f * y2 / z2;
	p.ok = true;
	return p;
}

// The X along the street at which a point at height Y (camera Y, down) on the line Z = z lands in picture column u: the
// inverse of projectPoint along one street-parallel line (u is a linear fraction of X there).  NaN where no X does.
inline double streetXAt(const CameraSetup& c, double frameWidth, double frameHeight, double u, double Y, double Z) {
	detail::Rig r;
	if (!detail::rig(c, frameWidth, frameHeight, r)) return std::numeric_limits<double>::quiet_NaN();
	// u - cx = f (cy X - sy Z) / (sp Y + cp (sy X + cy Z))  =>  X (k cp sy - f cy) = -f sy Z - k (sp Y + cp cy Z)
	double k = u - r.cx, den = k * r.cp * r.sy - r.f * r.cy;
	if (std::fabs(den) < 1e-9) return std::numeric_limits<double>::quiet_NaN();   // u is the vanishing point itself
	double X = (-r.f * r.sy * Z - k * (r.sp * Y + r.cp * r.cy * Z)) / den;
	return projectPoint(c, frameWidth, frameHeight, X, Y, Z).ok ? X : std::numeric_limits<double>::quiet_NaN();
}

// A typical car's size, ft (about a mid-size sedan); the hubcap is where VST's lane line goes.  distanceFt is to the
// middle of the lane, where a car is centred, so its near side runs widthFt / 2 closer.
struct CarSize { double lengthFt = 15.5, widthFt = 6, heightFt = 4.8, hubcapFt = 1.1; };

// The line VST times along: the near side of a car in the lane, at hubcap height (the lane line).  Its camera Y and Z, or
// false where the near side would reach the camera's wall (a lane nearer than half a car's width plus half a foot).
inline bool hubcapLine(const CameraSetup& c, const CarSize& car, double& Y, double& Z) {
	Y = c.heightFt - car.hubcapFt;
	Z = c.distanceFt - car.widthFt / 2;
	return Z > 0.5;
}

// Feet of street between picture columns uLeft and uRight along the hubcap line: the zone between the speed lines
// in ground units, which is all calibration needs (CalibrationFrames = feet / (mph in ft/s) x fps).  NaN where either
// column can't be placed (behind the camera, at the vanishing point, an invalid setup).
inline double zoneFeet(const CameraSetup& c, double frameWidth, double frameHeight, double uLeft, double uRight,
	const CarSize& car = CarSize()) {
	double Y, Z;
	if (!hubcapLine(c, car, Y, Z)) return std::numeric_limits<double>::quiet_NaN();
	double a = streetXAt(c, frameWidth, frameHeight, uLeft, Y, Z), b = streetXAt(c, frameWidth, frameHeight, uRight, Y, Z);
	return std::fabs(b - a);   // NaN in, NaN out
}

// The eight corners of a car on the lane, the front bumper's near corner in picture column frontU and on picture row
// anchorV at hubcap height (the lane line there), driving right (goingRight) or left.  The box is projected with the camera
// model and then shifted up or down so that corner sits on the lane line the user drew: size, slant and perspective come
// from the model, the placement from the picture, so a wrong distance or lens shows as the wrong size and a wrong angle as
// the wrong slant, even where the camera's tilt doesn't quite match.  Corner order: (back, front) x (near, far) x (bottom,
// top), index = front * 4 + far * 2 + top.  ok = false if the front can't be placed there or a corner is behind the camera.
struct Ghost { Pixel corner[8]; bool ok = false; };
inline Ghost ghostCar(const CameraSetup& c, double frameWidth, double frameHeight, double frontU, double anchorV, bool goingRight,
	const CarSize& car = CarSize()) {
	Ghost g;
	double hubY, nearZ;
	if (!hubcapLine(c, car, hubY, nearZ)) return g;
	double front = streetXAt(c, frameWidth, frameHeight, frontU, hubY, nearZ);
	if (std::isnan(front)) return g;
	double dv = anchorV - projectPoint(c, frameWidth, frameHeight, front, hubY, nearZ).v;
	double back = goingRight ? front - car.lengthFt : front + car.lengthFt;
	for (int i = 0; i < 8; i++) {
		bool isFront = i & 4, far = i & 2, top = i & 1;
		double X = isFront ? front : back;
		double Z = far ? nearZ + car.widthFt : nearZ;
		double Y = c.heightFt - (top ? car.heightFt : 0);
		g.corner[i] = projectPoint(c, frameWidth, frameHeight, X, Y, Z);
		if (!g.corner[i].ok) return g;
		g.corner[i].v += dv;
	}
	g.ok = true;
	return g;
}

// A street edge for a vanishing point at box x `xV`: the line through the lane line's point there, starting `offset` px
// below the lane at the box's left edge (above, if below won't fit; then half as far, and so on), so both ends sit
// inside the box (rows 1..boxHeight).  It is drawn where it's easy to see and drag, not on the curb itself: only where
// it meets the lane line counts.  NaN xV (no receding) gives a parallel line.  ok = false when no offset fits (a lane line
// at the very edge of a box only a few pixels high); the caller then leaves the edge as it is.
struct EdgeResult { bool ok = false; double yLeft = 0, yRight = 0; };
inline EdgeResult edgeThrough(const Line& lane, double xV, double offset, double boxHeight) {
	EdgeResult r;
	if (!std::isnan(xV) && std::fabs(xV) < 1) return r;   // on the box's left edge: no line through it can tell
	for (double off = std::fabs(offset); off >= 1; off /= 2)
		for (double sign : { 1.0, -1.0 }) {
			double left = lane.yLeft + sign * off, right;
			if (std::isnan(xV)) right = lane.yRight + sign * off;                 // parallel: no receding
			else right = left + (lane.at(xV) - left) * lane.width / xV;           // through (0, left) and (xV, lane at xV)
			if (left >= 1 && right >= 1 && left <= boxHeight && right <= boxHeight) {
				r.ok = true; r.yLeft = left; r.yRight = right;
				return r;
			}
		}
	return r;
}

}  // namespace perspective
