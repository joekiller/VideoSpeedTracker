// Site planner model (src/Gui/Plan.cpp) unit test.
//
//   plan_test
//
// Street presets: each one is a sensible view and the planner can time cars on it.  Measuring: a calibration's zone
// in feet and the speed lines' pixels give the picture's scale (the Locust Avenue sample's own numbers are checked
// against a hand calculation), car lengths across the picture give a rougher one, and applying either keeps the
// planner's picture width equal to what was measured whatever the field of view.  Verdicts: the 15 ft view at 30 fps
// times cars up to about 45 mph (the number the planner has stated since PR #29), 15 fps halves that, and a view of
// a couple of feet says so rather than dividing by zero.
#include "Plan.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace plan;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void testPresets() {
	CHECK(kStreetPresetCount >= 3);
	for (const StreetPreset& p : kStreetPresets) {
		CHECK(findStreetPreset(p.key) == &p);
		Inputs in;
		in.distM = p.distFt / kFt;
		in.clearM = p.clearFt / kFt;
		Verdict v = verdict(in, false);
		// Every preset is a view VST can time a car at limit + 10 on, with the default phone camera.
		CHECK(v.at.zone > 0);
		CHECK(!v.tooFew);
		CHECK(v.grade <= 1);
	}
	CHECK(findStreetPreset("nope") == nullptr);
	// The narrow preset is the short view the planner was built for: it needs short-view timing.
	Inputs narrow;
	narrow.distM = findStreetPreset("narrow")->distFt / kFt;
	narrow.clearM = findStreetPreset("narrow")->clearFt / kFt;
	CHECK(verdict(narrow, false).shortView);
}

static void testSources() {
	for (Source s : { Source::Guess, Source::Typed, Source::Picture, Source::Calibration }) {
		Source back = Source::Guess;
		CHECK(sourceFromKey(sourceKey(s), back) && back == s);
		CHECK(std::string(sourceName(s)).size() > 3);
	}
	Source x = Source::Typed;
	CHECK(!sourceFromKey("tape", x) && x == Source::Typed);
}

static void testFromCalibration() {
	// The 2016 Locust Avenue sample: 35 frames at 25 mph and 30 fps between speed lines 350 and 909 px.
	// Zone = 25 mph = 36.67 ft/s x 35 / 30 s = 42.78 ft over 559 px: 13.07 px/ft, so 1280 px is 97.9 ft.
	Measured m = fromCalibration(35, 25, 30, 909 - 350, 1269, 60);
	CHECK(m.valid);
	CHECK(approx(m.zoneFt, 42.78, .01));
	CHECK(approx(m.pxPerFt, 13.07, .01));
	CHECK(approx(m.coverFt, 97.94, .05));
	CHECK(approx(m.clearFt, (1269 - 60) / m.pxPerFt, 1e-9));
	// The clear stretch never exceeds the picture, and nonsense gives nothing.
	CHECK(fromCalibration(35, 25, 30, 559, 5000, 0).clearFt <= fromCalibration(35, 25, 30, 559, 5000, 0).coverFt);
	CHECK(!fromCalibration(0, 25, 30, 559, 1269, 0).valid);
	CHECK(!fromCalibration(35, 25, 30, 0, 1269, 0).valid);
	CHECK(!fromCalibration(1, 25, 30, 1200, 1269, 0).valid);   // 1.2 ft across the whole picture: a typo

	// Applying it: whatever the field of view, the planner's picture width is the measured one.
	for (double hfov : { 45.0, 66.0, 100.0 }) {
		Inputs in;
		in.hfovDeg = hfov;
		applyMeasured(in, m);
		CHECK(approx(coverAt(in.distM, in.hfovDeg) * kFt, m.coverFt, .01));
		CHECK(approx(in.clearM * kFt, m.clearFt, .01));
	}
}

static void testCarLengths() {
	// Six 15.5 ft car lengths across the picture: 93 ft, the whole box clear when there is no box yet.
	Measured m = fromCarLengths(6, 0, 0);
	CHECK(m.valid);
	CHECK(approx(m.coverFt, 93, 1e-9));
	CHECK(approx(m.clearFt, 93, 1e-9));
	// A box over half the picture with a 64 px trunk in it.
	Measured h = fromCarLengths(6, 640, 64);
	CHECK(approx(h.clearFt, (640 - 64) / (1280 / 93.0), 1e-9));
	CHECK(!fromCarLengths(0, 640, 0).valid);
}

static void testVerdicts() {
	// 15 ft clear at 30 fps (PR #29): short-view timing, cars up to about 45 mph.
	Inputs in;
	in.distM = 30 / kFt;
	in.clearM = 15 / kFt;
	Verdict v = verdict(in, true);
	CHECK(v.shortView);
	CHECK(fastestText(v.fastestMph) == "up to about 40 mph" || fastestText(v.fastestMph) == "up to about 45 mph");
	CHECK(fastestText(v.fastest60) == "over 70 mph");
	// The same view at 15 fps: half the fastest speed, so speeders at limit + 10 may go untimed.
	Inputs slow = in;
	slow.fps = 15;
	Verdict s = verdict(slow, true);
	CHECK(approx(s.fastestMph, v.fastestMph / 2, 1e-9));
	CHECK(s.tooFew || s.grade >= v.grade);
	// A view of a couple of feet: no zone, a "needs a tweak" verdict with a sentence that says what to do.
	Inputs tiny = in;
	tiny.clearM = .5;
	Verdict t = verdict(tiny, true);
	CHECK(t.grade == 2);
	CHECK(verdictText(t).find("further back") != std::string::npos);
	// A wide, clear view gets whole-frame timing; at 60 fps a good grade.
	Inputs wide;
	wide.distM = 50 / kFt;
	wide.clearM = 90 / kFt;
	wide.fps = 60;
	Verdict w = verdict(wide, false);
	CHECK(!w.shortView);
	CHECK(w.grade == 0);
	CHECK(verdictText(w).rfind("About ±", 0) == 0);
	CHECK(std::string(gradeName(w.grade)) == "Good to go");
}

int main() {
	testPresets();
	testSources();
	testFromCalibration();
	testCarLengths();
	testVerdicts();
	std::printf("plan_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
