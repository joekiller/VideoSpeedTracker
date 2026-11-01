// Distance calibration arithmetic (src/Gui/Calibration.h): the cones/pole and known-car modes.
//
//   calibration_test
//
// Cones: a 10 ft spacing over 200 px with a 600 px zone is a 30 ft zone, which a car at 25 mph (36.67 ft/s) crosses in
// 0.818 s = 24.5 frames at 30 fps.  Known car: the same arithmetic with the bumpers as marks, plus the depth correction
// for a car parked beyond the traffic lane, checked against a pinhole camera: a car at depth Z spans f × L / Z pixels,
// so the corrected marks must equal what the same car would span in the lane, and the calibration must then equal the
// one a car in the lane gives.  Also the no-op and guard cases, and the picker's lengths being plausible car sizes.
//
// The parking lane (ParkingLane.h): a car marked by its wheel centres is called Parked or In the travel lane by which line
// its marks are nearer, and a parked car's wheelbase, scaled by the camera model, calibrates the zone as well as a
// passing car's does.  Checked end to end through perspective::projectPoint: the marks are where a real camera would
// show the wheel centres of a car parked 8 ft beyond the lane, and the calibration must match the zone's true length.
#include "Calibration.h"
#include "ParkingLane.h"
#include <cmath>
#include <cstdio>

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

	// Wheelbases: shorter than the car, by the usual 55-70%.
	for (const calib::KnownCar& k : calib::kKnownCars) CHECK(k.wheelbaseIn > .55 * k.inches && k.wheelbaseIn < .70 * k.inches);

	// ---- the parking lane ----------------------------------------------------------------------------------------
	{
		using namespace perspective;
		CameraSetup cam{ 20, 15, 0, 66 };   // a second-floor window 20 ft from the lane's middle, square to the street
		CarSize car;
		double Y, laneZ;
		CHECK(hubcapLine(cam, car, Y, laneZ));
		const double parkZ = laneZ + 8, wheelbase = 111.0 / 12;
		auto rowOf = [&](double u, double Z) { return projectPoint(cam, 1280, 720, streetXAt(cam, 1280, 720, u, Y, Z), Y, Z).v; };
		calib::ParkingLane park{ rowOf(0, parkZ), rowOf(1280, parkZ) };
		CHECK(park.set());
		CHECK(!calib::ParkingLane{}.set());

		// A parked car's two wheel centres, as the camera sees them.
		double X0 = -2, X1 = X0 + wheelbase;
		Pixel w0 = projectPoint(cam, 1280, 720, X0, Y, parkZ), w1 = projectPoint(cam, 1280, 720, X1, Y, parkZ);
		double midX = (w0.u + w1.u) / 2, laneRow = rowOf(midX, laneZ);
		calib::CarSpot s = calib::placeCar(cam, laneRow, park, midX, (w0.v + w1.v) / 2);
		CHECK(s.where == calib::CarSpot::Parked && !s.offLines && s.fromPicture);
		CHECK(approx(s.parkedFt, 8, 1e-4));
		// The zone between speed lines at u 340 and 940, in feet along the lane's hubcap line, against the calibration
		// from the parked car's wheelbase scaled to the lane.
		double zoneFt = zoneFeet(cam, 1280, 720, 340, 940);
		double truth = zoneFt / (25 * 5280.0 / 3600.0) * 30;
		double got = calib::framesForDistance(std::fabs(w1.u - w0.u) * s.scale(), wheelbase, 600, 25, 30);
		CHECK(std::fabs(got / truth - 1) < .005);
		// Uncorrected it reads every speed about a fifth high; the old distance-ratio correction overshoots the other way.
		double raw = calib::framesForDistance(std::fabs(w1.u - w0.u), wheelbase, 600, 25, 30);
		CHECK(raw / truth > 1.15);
		double ratio = calib::framesForDistance(calib::markPxAtLane(std::fabs(w1.u - w0.u), laneZ, 8), wheelbase, 600, 25, 30);
		CHECK(ratio / truth < .9);

		// The same car driving in the lane: In the travel lane, scale 1, and the calibration is right as it is.
		Pixel d0 = projectPoint(cam, 1280, 720, X0, Y, laneZ), d1 = projectPoint(cam, 1280, 720, X1, Y, laneZ);
		calib::CarSpot m = calib::placeCar(cam, rowOf((d0.u + d1.u) / 2, laneZ), park, (d0.u + d1.u) / 2, (d0.v + d1.v) / 2);
		CHECK(m.where == calib::CarSpot::InLane && m.scale() == 1 && !m.offLines);
		CHECK(std::fabs(calib::framesForDistance(std::fabs(d1.u - d0.u), wheelbase, 600, 25, 30) / truth - 1) < .005);

		// A typed distance wins over the picture's and takes its side from it; no parking lane: Unknown, no correction;
		// marks far from both lines are flagged.
		calib::ParkingLane typed = park;
		typed.measuredFt = 6;
		calib::CarSpot t = calib::placeCar(cam, laneRow, typed, midX, (w0.v + w1.v) / 2);
		CHECK(approx(t.parkedFt, 6) && !t.fromPicture && t.scale() > 1 && t.scale() < s.scale());
		calib::CarSpot u = calib::placeCar(cam, laneRow, calib::ParkingLane{}, midX, (w0.v + w1.v) / 2);
		CHECK(u.where == calib::CarSpot::Unknown && u.scale() == 1);
		calib::CarSpot o = calib::placeCar(cam, laneRow, park, midX, park.at(midX) - 200);
		CHECK(o.offLines && o.where == calib::CarSpot::Parked);

		// A parking lane on the near side (below the lane line): nearer, so the span shrinks.
		calib::ParkingLane nearSide{ rowOf(0, laneZ - 7), rowOf(1280, laneZ - 7) };
		calib::CarSpot n = calib::placeCar(cam, laneRow, nearSide, midX, nearSide.at(midX));
		CHECK(n.where == calib::CarSpot::Parked && approx(n.parkedFt, -7, 1e-4) && n.scale() < 1);

		// A camera at hubcap height can't read depth from rows: the typical 7 ft, on the side the line is drawn.
		calib::CarSpot lvl = calib::placeCar({ 20, 1.2, 0, 66 }, 400, calib::ParkingLane{ 380, 380 }, 640, 380);
		CHECK(lvl.typical && approx(lvl.parkedFt, calib::kTypicalParkFt) && lvl.scale() > 1);
		CHECK(calib::scaleBeyond(cam, 640, 0) == 1);
		CHECK(calib::scaleBeyond(cam, 640, -50) == 1);   // behind the camera's wall
	}

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
