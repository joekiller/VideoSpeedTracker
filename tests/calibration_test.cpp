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
// Combining (src/Gui/CalEvidence.h), with the scratch directory as argv[1]: every method's zone length and error model;
// the inverse-variance combination, contributions and Birge widening; an outlier singled out only with three or more;
// removed inputs, other directions and other speed lines left out; the novice summary; a repeated input not counted
// twice; and calibration-inputs.json round-tripping one config while keeping another's, and surviving a damaged file.
#include "CalEvidence.h"
#include "Calibration.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool approx(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

static calib::Input mk(const char* id, const char* method, double zoneFt, double sigma, int dirs = calib::kBothDirs, const char* lines = "L") {
	calib::Input in;
	in.id = id; in.method = method; in.zoneFt = zoneFt; in.sigma = sigma; in.dirs = dirs; in.lines = lines; in.time = "2026-11-01T12:00:00-04:00";
	return in;
}

static void combining(const std::string& dir) {
	using namespace calib;
	// Each method's zone length.  A pass at 25 mph taking 30 frames at 30 fps covers 36.67 ft; the frames the engine
	// uses for that zone at CalibrationSpeed 25 / CalibrationFPS 30 are those 30 frames, the inverse of the engine formula.
	CHECK(approx(zoneFtFromPass(25, 30, 30), 25 * 5280.0 / 3600.0));
	CHECK(approx(framesForZone(zoneFtFromPass(15, 40, 60), 25, 30), 40 * 15.0 / 25 * 30 / 60, 1e-9));   // = calib::impliedFrames
	CHECK(approx(zoneFtForFrames(framesForZone(31.4, 25, 30), 25, 30), 31.4, 1e-9));
	CHECK(approx(zoneFtFromMarks(200, 10, 600), 30));
	CHECK(zoneFtFromPass(0, 30, 30) == 0 && framesForZone(0, 25, 30) == 0);

	// Error models: a 15 mph pass over 20 frames is about 4% (speed 3.6%, counting 2%); a slower count of more frames is
	// better on counting.  A parked car is less sure than one in the lane; cones on a wider span are surer.
	CHECK(approx(sigmaPass(15, 20), std::hypot(std::hypot(0.3, 0.5) / 15, 0.41 / 20), 1e-12));
	CHECK(sigmaPass(15, 40) < sigmaPass(15, 20));
	CHECK(sigmaCar(150, true, 30, 7) > sigmaCar(150, false, 30, 7));
	CHECK(sigmaCar(150, true, 0, 7) > sigmaCar(150, true, 30, 7));
	CHECK(sigmaCones(400) < sigmaCones(100));

	// Equal sigmas: the plain mean, equal shares, sigma shrinking by sqrt(n) when they agree as well as expected.
	std::vector<Input> v = { mk("a", "pass", 30, .03), mk("b", "cones", 30.6, .03) };
	Combined c = combine(v, true, "L");
	CHECK(c.valid && c.n == 2 && approx(c.zoneFt, 30.3, 1e-9));
	CHECK(approx(c.shares[0].contribution, .5) && approx(c.shares[1].contribution, .5));
	CHECK(c.birge < 1 && approx(c.sigma, .03 / std::sqrt(2), 2e-3));
	CHECK(c.agree() && summary(c) == "2 measurements agree within 2%");

	// Inverse-variance weights: a rough camera setup (10%) beside a careful pass (3%) gets about 8% of the say,
	// and adding it moves the result only a little toward it; it never replaces the pass.
	v = { mk("a", "pass", 30, .03), mk("c", "camera", 33, .10) };
	c = combine(v, true, "L");
	double wp = 1 / (.03 * .03), wc = 1 / (.10 * .10);
	CHECK(approx(c.shares[1].contribution, wc / (wp + wc), 1e-9) && c.shares[1].contribution < .1);
	CHECK(c.zoneFt > 30 && c.zoneFt < 30.3);

	// Two careful inputs that disagree by 10%: the Birge ratio widens the sigma well past what either claims, and the
	// summary says they differ instead of naming one.
	v = { mk("a", "pass", 30, .02), mk("b", "car", 33, .02) };
	c = combine(v, true, "L");
	CHECK(c.birge > 2 && c.sigma > .04 && c.outliers == 0 && !c.agree());
	CHECK(summary(c) == "2 measurements differ by 10%");

	// Three: two agree, one is 15% off.  Only that one is flagged.
	v = { mk("a", "pass", 30, .03), mk("b", "cones", 30.4, .03), mk("x", "car", 35, .03) };
	c = combine(v, true, "L");
	CHECK(c.n == 3 && c.outliers == 1 && !c.shares[0].outlier && !c.shares[1].outlier && c.shares[2].outlier);
	CHECK(summary(c) == "3 measurements, 1 well off the others");
	// Taken out, the other two agree; the removed one stays on record.
	v[2].removed = true;
	c = combine(v, true, "L");
	CHECK(c.n == 2 && c.agree() && summary(c) == "2 measurements agree within 2%");
	CHECK(v.size() == 3);

	// Three that all disagree: none is the odd one out, so none is flagged, and the summary says they differ.
	c = combine({ mk("a", "pass", 30, .02), mk("b", "cones", 33, .02), mk("c", "car", 36, .02) }, true, "L");
	CHECK(c.outliers == 0 && !c.agree() && summary(c) == "3 measurements differ by 19%");

	// Three in agreement: the novice line Joe asked for.
	v = { mk("a", "pass", 30, .04), mk("b", "cones", 30.5, .03), mk("c", "car", 29.7, .03) };
	CHECK(summary(combine(v, true, "L")) == "3 measurements agree within 3%");

	// Direction and speed lines: a right-to-left input doesn't count left to right; one from other lines is set aside.
	v = { mk("a", "pass", 30, .03, kL2R), mk("b", "pass", 40, .03, kR2L), mk("c", "cones", 31, .03, kBothDirs, "OLD") };
	c = combine(v, true, "L");
	CHECK(c.n == 1 && approx(c.zoneFt, 30) && c.setAside == 1 && summary(c) == "1 measurement so far");
	CHECK(approx(combine(v, false, "L").zoneFt, 40));
	c = combine({ mk("c", "cones", 31, .03, kBothDirs, "OLD") }, true, "L");
	CHECK(!c.valid && c.setAside == 1 && summary(c) == "Your earlier measurements don't fit these speed lines");
	CHECK(summary(combine({}, true, "L")) == "Not calibrated yet");

	// The same pass added twice counts once; a different one is added with a fresh id.
	v.clear();
	CHECK(addInput(v, mk("", "pass", 30, .03)));
	CHECK(!addInput(v, mk("", "pass", 30, .03)));
	CHECK(addInput(v, mk("", "pass", 30.2, .03)));
	CHECK(v.size() == 2 && !v[0].id.empty() && v[0].id != v[1].id);

	// The file: round trip for one config, another config's inputs kept, and a damaged file read as empty.
	namespace fs = std::filesystem;
	fs::create_directories(dir);
	std::string file = (fs::path(dir) / "calibration-inputs.json").string();
	fs::remove(file);
	bool known = true;
	CHECK(loadInputs(file, "/x/VST.cfg", &known).empty() && !known);
	std::string err;
	v[1].removed = true;
	v[1].dirs = kR2L;
	v[1].label = "Toyota Camry 2018-2024, parked";
	CHECK(saveInputs(file, "/x/VST.cfg", v, err));
	CHECK(saveInputs(file, "/y/VST.cfg", { mk("o", "cones", 25, .02) }, err));
	std::vector<Input> back = loadInputs(file, "/x/VST.cfg", &known);
	CHECK(known && back.size() == 2);
	if (back.size() == 2) {
		CHECK(back[0].id == v[0].id && back[0].method == "pass" && approx(back[0].zoneFt, 30, 1e-3) && approx(back[0].sigma, .03, 1e-4));
		CHECK(back[1].removed && back[1].dirs == kR2L && back[1].label == v[1].label && back[1].lines == "L");
	}
	CHECK(loadInputs(file, "/y/VST.cfg").size() == 1);
	// A config with an empty list is still "known", so its calibration isn't seeded again after every input is gone.
	CHECK(saveInputs(file, "/x/VST.cfg", {}, err));
	CHECK(loadInputs(file, "/x/VST.cfg", &known).empty() && known);
	CHECK(loadInputs(file, "/y/VST.cfg").size() == 1);
	{ std::ofstream f(file, std::ios::trunc); f << "{ not json"; }
	CHECK(loadInputs(file, "/y/VST.cfg", &known).empty() && !known);
	CHECK(saveInputs(file, "/x/VST.cfg", { mk("a", "pass", 30, .03) }, err) && loadInputs(file, "/x/VST.cfg").size() == 1);
}

int main(int argc, char** argv) {
	combining(argc > 1 ? argv[1] : "calibration-test");

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

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
