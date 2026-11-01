// Distance calibration arithmetic (src/Gui/Calibration.h): the cones/pole and known-car modes.
//
//   calibration_test
//
// Cones: a 10 ft spacing over 200 px with a 600 px zone is a 30 ft zone, which a car at 25 mph (36.67 ft/s) crosses in
// 0.818 s = 24.5 frames at 30 fps.  Known car: the same arithmetic with the bumpers as marks, plus the depth correction
// for a car parked beyond the traffic lane, checked against a pinhole camera: a car at depth Z spans f × L / Z pixels,
// so the corrected marks must equal what the same car would span in the lane, and the calibration must then equal the
// one a car in the lane gives.  Also the no-op and guard cases, and the picker's lengths being plausible car sizes.
#include "Calibration.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool approx(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

int main() {
	// Cones: 30 ft zone at 25 mph, 30 fps.
	double cones = calib::framesForDistance(200, 10, 600, 25, 30);
	CHECK(approx(cones, 30.0 / (25 * 5280.0 / 3600.0) * 30));
	CHECK(approx(cones, 24.545454, 1e-5));
	CHECK(calib::framesForDistance(0, 10, 600, 25, 30) == 0);
	CHECK(calib::framesForDistance(200, 0, 600, 25, 30) == 0);

	// Pinhole camera: focal length 1000 px, lane 30 ft away, car 16 ft long, parked 7 ft beyond the lane.
	const double f = 1000, laneFt = 30, farther = 7, carFt = 16;
	double inLanePx = f * carFt / laneFt, parkedPx = f * carFt / (laneFt + farther);
	CHECK(approx(calib::markPxAtLane(parkedPx, laneFt, farther), inLanePx, 1e-9));
	// The zone between the speed lines, 600 px at the lane's depth, is f-scaled: 600 × 30 / 1000 = 18 ft.
	double zonePx = 600, zoneFt = zonePx * laneFt / f;
	double truth = zoneFt / (25 * 5280.0 / 3600.0) * 30;
	double corrected = calib::framesForDistance(calib::markPxAtLane(parkedPx, laneFt, farther), carFt, zonePx, 25, 30);
	double moving = calib::framesForDistance(inLanePx, carFt, zonePx, 25, 30);
	CHECK(approx(corrected, truth, 1e-9));
	CHECK(approx(moving, truth, 1e-9));
	// Uncorrected, the parked car makes the zone look longer, so the calibration (and every speed) reads high by 37/30.
	double raw = calib::framesForDistance(parkedPx, carFt, zonePx, 25, 30);
	CHECK(approx(raw / truth, (laneFt + farther) / laneFt, 1e-9));

	// No correction when nothing is known or the car is in the lane.
	CHECK(calib::markPxAtLane(250, 30, 0) == 250);
	CHECK(calib::markPxAtLane(250, 0, 7) == 250);
	CHECK(calib::markPxAtLane(0, 30, 7) == 0);

	// Every model in the picker has a name and a car-sized length (13-21 ft).
	for (const calib::KnownCar& k : calib::kKnownCars) CHECK(k.inches >= 156 && k.inches <= 252 && k.name[0]);

	// Drive-by target: the presets plus a limit that isn't one of them, in order; the default follows the limit
	// until the user picks one; the check suggestion differs from the calibration and stays in range.
	CHECK((calib::targetPresets(25) == std::vector<int>{ 10, 15, 20, 25 }));
	CHECK((calib::targetPresets(15) == std::vector<int>{ 10, 15, 20, 25 }));
	CHECK((calib::targetPresets(30) == std::vector<int>{ 10, 15, 20, 25, 30 }));
	CHECK((calib::targetPresets(12) == std::vector<int>{ 10, 12, 15, 20, 25 }));
	CHECK((calib::targetPresets(0) == std::vector<int>{ 10, 15, 20, 25 }));
	CHECK(calib::defaultTarget(0, 15) == 15);
	CHECK(calib::defaultTarget(20, 15) == 20);
	CHECK(calib::defaultTarget(0, 0) == 25);
	CHECK(calib::defaultTarget(200, 15) == calib::kTargetMax);
	CHECK(calib::checkTarget(15) == 20);
	CHECK(calib::checkTarget(10) == 15);
	CHECK(calib::checkTarget(25) == 20);
	CHECK(calib::checkTarget(5) == 10);
	CHECK(calib::belowCruiseControl(15) && !calib::belowCruiseControl(25));
	// A slower drive-by measures the same zone: the 30 ft zone above takes a 15 mph car 25/15 as many frames, and the
	// cones' CalibrationFrames is the same whatever CalibrationSpeed the config is expressed in, once scaled back.
	CHECK(approx(calib::framesForDistance(200, 10, 600, 15, 30), cones * 25 / 15, 1e-9));

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
