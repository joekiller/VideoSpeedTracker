// Calibration sets (src/Gui/CalSets.cpp) unit test.
//
//   calsets_test <work dir>
//
// Joe asked how much extra data re-adjusting past speeds needs after a recalibration, and to lock datasets to the
// calibration they were measured with.  These checks pin the model:
//   - a run's manifest gives its view and zone feet; a run without one is "calibration not on record", never adjusted
//   - runs from one view share an open set that follows today's calibration; a run from a new view locks the old set
//     at the calibration it last had, and dragging the lines back without recording locks nothing
//   - retime() is exact from ZoneSeconds, and within half a mph from a whole-mph speed; trend fields follow
//   - lock / unlock / accept, and the store surviving a save and load
#include "CalSets.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace calsets;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static fs::path work;

// A stats csv plus its manifest, as the engine writes them.  framesL2R/R2L at 25 mph and 30 fps; lineLeft sets the view.
static std::string makeRun(const std::string& stem, const std::string& started, int framesL2R, int framesR2L, int lineLeft,
	bool withBox = true, bool manifest = true) {
	fs::path stats = work / "Stats";
	fs::create_directories(stats);
	std::ofstream(stats / ("stats_" + stem + ".csv")) << "Date, Time, Frame, Direction\n";
	if (manifest) {
		std::ofstream m(stats / ("run_" + stem + ".json"));
		m << "{ \"format\": 1, \"started\": \"" << started << "\", \"config\": { \"CalibrationSpeed\": 25, \"CalibrationFPS\": 30, "
		  << "\"CalibrationFramesL2R\": " << framesL2R << ", \"CalibrationFramesR2L\": " << framesR2L << ", \"speedLineLeft\": " << lineLeft
		  << ", \"speedLineRight\": 900, \"levelDegrees\": 0, \"streetEdge\": [0, 0]"
		  << (withBox ? ", \"analysisBox\": [10, 220, 1269, 190]" : "") << " } }\n";
	}
	return (stats / ("stats_" + stem + ".csv")).string();
}

static std::vector<RunInfo> runsIn() {
	std::vector<RunInfo> out;
	for (auto& e : fs::directory_iterator(work / "Stats"))
		if (e.path().extension() == ".csv") out.push_back(readRun(e.path().string()));
	return out;
}

static View viewAt(int lineLeft) {
	View v;
	v.lineLeft = lineLeft;
	v.lineRight = 900;
	v.box[0] = 10; v.box[1] = 220; v.box[2] = 1269; v.box[3] = 190;
	return v;
}
static Scale scaleOf(double l2r, double r2l) { Scale s; s.l2r = l2r; s.r2l = r2l; return s; }

static void testReadRun() {
	std::string csv = makeRun("a", "2026-10-22T09:00:00", 30, 36, 100);
	RunInfo r = readRun(csv);
	CHECK(r.known);
	CHECK(r.csv == "stats_a.csv");
	// 25 mph = 36.67 ft/s; 30 frames at 30 fps = 1 s, so 36.67 ft.
	CHECK(std::fabs(r.scale.l2r - 25 * 5280.0 / 3600.0) < 1e-9);
	CHECK(std::fabs(r.scale.r2l - 25 * 5280.0 / 3600.0 * 36 / 30) < 1e-9);
	CHECK(r.view.same(viewAt(100)) && !r.view.same(viewAt(101)));
	// No analysis box (a manifest from before v1.10) still matches on the rest.
	RunInfo old = readRun(makeRun("old", "2026-10-21T09:00:00", 30, 36, 100, false));
	CHECK(old.known && !old.view.hasBox() && old.view.same(viewAt(100)));
	// No manifest (VST 1.1): calibration unknown.
	CHECK(!readRun(makeRun("ancient", "", 30, 36, 100, true, false)).known);
	fs::remove_all(work / "Stats");
}

static void testSync() {
	const Scale none;
	// Two runs at view A with calibration X, then today's calibration of A is Y: one open set showing Y.
	makeRun("a1", "2026-10-22T09:00:00", 30, 36, 100);
	makeRun("a2", "2026-10-23T09:00:00", 30, 36, 100);
	Store st;
	Scale y = scaleOf(30.0, 33.0);
	CHECK(sync(st, runsIn(), viewAt(100), y, none, "now"));
	CHECK(st.sets.size() == 1);
	CHECK(!st.sets[0].locked() && st.sets[0].runs.size() == 2);
	CHECK(shownScale(st.sets[0], viewAt(100), y).same(y));
	CHECK(!sync(st, runsIn(), viewAt(100), y, none, "now"));   // nothing new: unchanged
	// Dragging the speed lines (view B in VST.cfg, nothing recorded) locks nothing; the set keeps Y, not following.
	Scale z = scaleOf(20.0, 22.0);
	sync(st, runsIn(), viewAt(150), z, none, "now");
	CHECK(st.sets.size() == 1 && !st.sets[0].locked());
	CHECK(shownScale(st.sets[0], viewAt(150), z).same(y));
	// A run recorded from view B: a new open set, and A locks at Y ("the view moved").
	makeRun("b1", "2026-10-30T09:00:00", 24, 26, 150);
	sync(st, runsIn(), viewAt(150), z, none, "now");
	CHECK(st.sets.size() == 2);
	CHECK(st.sets[0].locked() && st.sets[0].lockReason == "view" && st.sets[0].lockedAt == "2026-10-30T09:00:00");
	CHECK(st.sets[0].scale.same(y));
	CHECK(!st.sets[1].locked() && st.sets[1].has("stats_b1.csv") && shownScale(st.sets[1], viewAt(150), z).same(z));
	CHECK(st.ofRun("stats_a2.csv") == &st.sets[0]);
	CHECK(st.sets[0].first == "2026-10-22T09:00:00" && st.sets[0].last == "2026-10-23T09:00:00");
	// Going back to view A later: A stays locked, so a new A run starts a third set (nothing more joins a locked set).
	makeRun("a3", "2026-11-02T09:00:00", 30, 36, 100);
	sync(st, runsIn(), viewAt(100), y, none, "now");
	CHECK(st.sets.size() == 3 && st.sets[1].locked() && st.sets[2].has("stats_a3.csv"));
	// A csv that is gone (archived) leaves its set; an empty set goes.
	fs::remove(work / "Stats" / "stats_b1.csv");
	sync(st, runsIn(), viewAt(100), y, none, "now");
	CHECK(st.sets.size() == 2 && !st.ofRun("stats_b1.csv"));
	// Runs without a manifest share one "not on record" set that is locked and never re-adjusted.
	makeRun("ancient", "", 30, 36, 100, true, false);
	sync(st, runsIn(), viewAt(100), y, none, "now");
	const Set* u = st.ofRun("stats_ancient.csv");
	CHECK(u && !u->known && u->locked() && !shownScale(*u, viewAt(100), y).valid());
	fs::remove_all(work / "Stats");
}

static void testLockAndStore() {
	const Scale none;
	makeRun("a1", "2026-10-22T09:00:00", 30, 36, 100);
	Store st;
	Scale y = scaleOf(30.0, 33.0), sig = scaleOf(0.02, 0.03);
	sync(st, runsIn(), viewAt(100), y, sig, "now");
	int id = st.sets[0].id;
	CHECK(lock(st, id, viewAt(100), y, sig, "2026-11-01T10:00:00"));
	CHECK(st.sets[0].locked() && st.sets[0].lockReason == "manual" && st.sets[0].scale.same(y) && st.sets[0].sigma.same(sig));
	CHECK(!lock(st, id, viewAt(100), y, sig, "again"));
	// Locked: a recalibration of the same view doesn't move it, but is offered.
	Scale y2 = scaleOf(29.0, 32.0);
	sync(st, runsIn(), viewAt(100), y2, sig, "now");
	CHECK(st.sets[0].scale.same(y));
	CHECK(newerCalibration(st.sets[0], viewAt(100), y2));
	CHECK(!newerCalibration(st.sets[0], viewAt(150), y2));   // another view: not comparable
	CHECK(accept(st, id, viewAt(100), y2, sig) && st.sets[0].scale.same(y2) && !newerCalibration(st.sets[0], viewAt(100), y2));
	// Save and load keep everything.
	st.sets[0].published.push_back("Published/2026-11-01_week-1");
	std::string err;
	CHECK(save((work / "Stats").string(), st, err));
	Store back = load((work / "Stats").string());
	CHECK(back.sets.size() == 1 && back.nextId == st.nextId);
	CHECK(back.sets[0].locked() && back.sets[0].scale.same(y2) && back.sets[0].lockedAt == "2026-11-01T10:00:00");
	CHECK(back.sets[0].view.same(viewAt(100)) && back.sets[0].published.size() == 1 && back.sets[0].has("stats_a1.csv"));
	// Unlock only on today's view with no other open set there.
	CHECK(!canUnlock(back, id, viewAt(150)));
	CHECK(unlock(back, id, viewAt(100)) && !back.sets[0].locked());
	// A garbled file is an empty store, rebuilt by sync.
	std::ofstream(storePath((work / "Stats").string())) << "{ not json";
	CHECK(load((work / "Stats").string()).sets.empty());
	fs::remove_all(work / "Stats");
}

static void testRetime() {
	const double ft = 25 * 5280.0 / 3600.0;   // 36.67 ft: 1 s at 25 mph
	Measured m;
	m.speed = 25;
	m.zoneSeconds = 1.0;
	// Same zone: unchanged.
	Retimed r = retime(m, ft, ft);
	CHECK(r.speed == 25 && !r.adjusted);
	// A zone 8% shorter: exactly 23.0 mph from the time.
	r = retime(m, ft, ft * 0.92);
	CHECK(r.speed == 23 && r.adjusted && std::fabs(r.exact - 23.0) < 1e-9);
	// Exactness: 27.4 mph stored as 27; from ZoneSeconds the re-timed value keeps the 0.4.
	m.speed = 27;
	m.zoneSeconds = ft / (27.4 * 5280.0 / 3600.0);
	r = retime(m, ft, ft * 1.1);
	CHECK(std::fabs(r.exact - 27.4 * 1.1) < 1e-9 && r.speed == 30);
	// Without ZoneSeconds: the whole-mph speed scaled (27 x 1.1 = 29.7 -> 30), within half a mph of exact.
	m.zoneSeconds = 0;
	r = retime(m, ft, ft * 1.1);
	CHECK(std::fabs(r.exact - 29.7) < 1e-9 && r.speed == 30);
	// Unknown scales leave it as measured.
	CHECK(retime(m, 0, ft).speed == 27 && !retime(m, 0, ft).adjusted && retime(m, ft, 0).speed == 27);
	// Trend fields: speeds and acceleration scale; a steady car's projection stays its exit speed; an accelerating one
	// is re-projected over 20 ft of real street.
	m.trend = true;
	m.speed = 25; m.zoneSeconds = 1.0; m.entryMph = 24; m.exitMph = 26; m.mphIn20ft = 26; m.accel = 1.0;
	r = retime(m, ft, ft * 1.2);
	CHECK(r.entryMph == 29 && r.exitMph == 31 && r.mphIn20ft == r.exitMph && std::fabs(r.accel - 1.2) < 1e-9);
	m.mphIn20ft = 28; m.accel = 4.0;
	r = retime(m, ft, ft * 1.2);
	double v = 26 * 1.2 * 5280.0 / 3600.0, a = 4.8 * 5280.0 / 3600.0;
	CHECK(r.mphIn20ft == int(std::sqrt(v * v + 2 * a * 20) / (5280.0 / 3600.0) + 0.5));
}

static void testWords() {
	Set s;
	s.id = 2;
	s.first = "2026-10-22T09:00:00";
	s.last = "2026-10-30T18:00:00";
	s.runs = { "a", "b" };
	s.state = State::Locked;
	CHECK(describe(s, scaleOf(21.2, 21.2)) == "Set 2 \xC2\xB7 Oct 22 to Oct 30 \xC2\xB7 2 runs \xC2\xB7 zone 21.2 ft \xC2\xB7 locked");
	CHECK(describe(s, scaleOf(21.2, 24.3)).find("zone 21.2 / 24.3 ft") != std::string::npos);
	s.known = false;
	CHECK(describe(s, Scale()).find("calibration not on record") != std::string::npos);
	CHECK(shortDate("") == "" && shortDate("2026-01-05T00:00") == "Jan 5");
}

int main(int argc, char** argv) {
	work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "calsets-test";
	fs::remove_all(work);
	fs::create_directories(work);
	testReadRun();
	testSync();
	testLockAndStore();
	testRetime();
	testWords();
	std::printf("%d of %d checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
