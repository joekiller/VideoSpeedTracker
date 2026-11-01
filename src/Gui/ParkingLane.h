// The parking lane, and where a car marked for the Known car calibration sits: parked, or in the travel lane.
//
// Mark your street (step 4) can carry a parking lane: a line the user lays on the parked cars' wheel centres, as the
// lane line sits on the moving cars' hubcaps.  It is VST Studio's own (vst-studio.ini parkY0/parkY1/parkFt), not the
// tracker's: the tracker never looks at parked cars, and the line usually lies outside the analysis box, so it is kept
// in whole-frame coordinates and doesn't move when the box does.
//
// What it is for is step 5's Known car mode.  The user clicks a car's two wheel centres (or its bumpers); the rows of
// those clicks say which line the car is on, so the page can show at a glance "Parked" or "In the travel lane", and a
// parked car's span is scaled to what it would be in the travel lane (perspective::laneScale) before it calibrates the
// zone.  How far beyond the travel lane the parked cars are comes from the picture (the gap between the two lines,
// through the camera model: perspective::hubcapDepthAtRow) unless the user typed a measured distance.
#pragma once
#include "Perspective.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace calib {

constexpr double kFrameW = 1280, kFrameH = 720;   // VST's frame grid, which the lines and marks are in
constexpr double kTypicalParkFt = 7;              // a parked car's near wheels beyond a passing car's when nothing else says

// A line across the whole frame: rows at x = 0 and x = 1280.  Rows 0, 0 = no parking lane.
struct ParkingLane {
	double yLeft = 0, yRight = 0;
	double measuredFt = 0;   // typed: how far the parked cars' near wheels are from the moving cars' (0 = from the picture)
	bool set() const { return yLeft > 0 && yRight > 0; }
	double at(double x) const { return yLeft + (yRight - yLeft) * x / kFrameW; }
};

// The span multiplier for a car whose near side is `beyondFt` farther from the camera than the moving cars' (negative:
// nearer), at picture column u: the camera model's laneScale when it can place both, else the plain depth ratio about
// the lane's near side.  1 for 0, and 1 for a "nearer" that would put the car behind the camera's wall.
inline double scaleBeyond(const perspective::CameraSetup& cam, double u, double beyondFt) {
	if (beyondFt == 0 || std::isnan(beyondFt)) return 1;
	perspective::CarSize car;
	double Y, laneZ;
	if (!perspective::hubcapLine(cam, car, Y, laneZ) || laneZ + beyondFt <= .5) return 1;
	double s = perspective::laneScale(cam, kFrameW, kFrameH, u, laneZ + beyondFt, car);
	if (s > 0 && std::isfinite(s)) return s;
	return (laneZ + beyondFt) / laneZ;
}

// Where a marked car is and what that means for its span.
struct CarSpot {
	enum Where { Unknown, InLane, Parked } where = Unknown;   // Unknown: no parking lane to compare with
	bool offLines = false;     // the marks are well away from both lines: probably not on the wheels
	double laneRow = 0, parkRow = 0, carRow = 0;   // at the car's middle column, frame px
	double parkedFt = std::numeric_limits<double>::quiet_NaN();   // the parking lane beyond the travel lane (+ = farther)
	bool fromPicture = false;  // parkedFt came from the two lines through the camera model
	bool typical = false;      // ... or is kTypicalParkFt because neither the picture nor a typed number could say
	double parkedScale = 1;    // the span multiplier for a car in the parking lane, at the car's column
	double scale() const { return where == Parked ? parkedScale : 1; }
};

// How far beyond the travel lane a parking lane is, signed: a typed distance takes the side from the picture (a line
// above the lane line is farther from a camera above the street), otherwise the camera model reads it off the gap
// between the lines at column u.
inline void parkedBeyond(const perspective::CameraSetup& cam, double u, double laneRow, const ParkingLane& p, CarSpot& s) {
	double parkRow = p.at(u);
	double side = parkRow <= laneRow ? 1 : -1;
	s.fromPicture = s.typical = false;
	if (p.measuredFt > 0) { s.parkedFt = side * p.measuredFt; return; }
	perspective::CarSize car;
	double Y, laneZ, Z = perspective::hubcapDepthAtRow(cam, kFrameW, kFrameH, u, laneRow, parkRow, car);
	if (perspective::hubcapLine(cam, car, Y, laneZ) && std::isfinite(Z)) { s.parkedFt = Z - laneZ; s.fromPicture = true; return; }
	s.parkedFt = side * kTypicalParkFt;
	s.typical = true;
}

// The car whose marks' middle column is `midX` and mean row `carRow` (frame px), against the travel lane's row there
// (`laneRow`) and the parking lane.  It is parked when its marks are nearer the parking line than the lane line.  Marks
// further from both lines than 60% of the gap between them (and at least 30 px) are flagged offLines.
inline CarSpot placeCar(const perspective::CameraSetup& cam, double laneRow, const ParkingLane& p, double midX, double carRow) {
	CarSpot s;
	s.laneRow = laneRow;
	s.carRow = carRow;
	if (!p.set()) return s;
	s.parkRow = p.at(midX);
	double toLane = std::fabs(carRow - laneRow), toPark = std::fabs(carRow - s.parkRow), gap = std::fabs(s.parkRow - laneRow);
	s.where = toPark < toLane ? CarSpot::Parked : CarSpot::InLane;
	s.offLines = std::min(toLane, toPark) > std::max(gap * .6, 30.0);
	parkedBeyond(cam, midX, laneRow, p, s);
	s.parkedScale = scaleBeyond(cam, midX, s.parkedFt);
	return s;
}

}  // namespace calib
