// Published datasets (src/Gui/Dataset.cpp) unit test.
//
//   dataset_test <work dir>
//
// Joe: a published dataset has a known schema and can be opened in the reviewer; if it later needs recalibrating, every
// point gets a before and after, and a statistical test decides whether the change is too much, marking it tainted.
// These checks pin:
//   - the equivalence test's three verdicts at their edges, and an unknown sigma
//   - a dataset written through archive::write (Published/, extra files, SHA256SUMS) reads back field for field, and
//     verify() catches a changed file
//   - check(): exact before/after per point from ZoneSeconds, the summary, checks/<when>.csv, status.json, the badge in
//     review.html, and the checksums still valid afterwards
//   - SHA-256 against the FIPS 180-4 test vectors; a newer schema is refused in plain words
#include "Archive.h"
#include "Dataset.h"
#include "Sha256.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace dataset;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string slurp(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::ostringstream s; s << in.rdbuf(); return s.str(); }

static void testSha() {
	CHECK(sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(sha256::hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void testEquivalence() {
	// Promised ±3.9% (sigma 2%); the new calibration has sigma 1% (one-sided 95% = ±1.645%).
	Test t = equivalence(20.0, 0.02, 20.0 * 1.01, 0.01);
	CHECK(t.verdict == Verdict::Accurate && std::fabs(t.delta - 0.01) < 1e-12 && std::fabs(t.tolerance - 0.0392) < 1e-12);
	// 3% off: 3% + 1.645% > 3.92% and 3% - 1.645% < 3.92%: can't tell.
	CHECK(equivalence(20.0, 0.02, 20.0 * 0.97, 0.01).verdict == Verdict::Review);
	// 7% off (speeds read 7% high): 7% - 1.645% > 3.92%: tainted.
	t = equivalence(20.0, 0.02, 20.0 * 0.93, 0.01);
	CHECK(t.verdict == Verdict::Tainted && t.delta < 0 && t.lo < t.delta && t.hi > t.delta);
	// A vague new calibration can't confirm anything, even when its value agrees.
	CHECK(equivalence(20.0, 0.02, 20.0, 0.05).verdict == Verdict::Review);
	// Unknown sigmas are taken as 6% (CalEvidence's earlier-calibration value).
	t = equivalence(20.0, 0, 20.0, 0);
	CHECK(std::fabs(t.tolerance - 1.96 * 0.06) < 1e-12 && std::fabs(t.sigmaNew - 0.06) < 1e-12);
	CHECK(equivalence(0, 0.02, 20, 0.01).verdict == Verdict::None);
}

// A small dataset: 10 cars L2R at 20..29 mph in a 20 ft zone, plus one own calibration drive and one flagged car.
static Info sample() {
	Info in;
	in.name = "Week one, before the signs";
	in.site = "7\xC2\xBD St SW";
	in.published = "2026-11-01T12:00:00";
	in.vst = "test";
	in.speedLimit = 25;
	in.wayOver = 35;
	in.first = "2026-10-22T08:00:00";
	in.last = "2026-10-29T18:00:00";
	in.calibrationSet = 2;
	in.cal.zoneFt[0] = 20.0; in.cal.zoneFt[1] = 22.0;
	in.cal.sigma[0] = 0.02; in.cal.sigma[1] = 0.02;
	in.cal.how = "3 measurements agree within 2%";
	for (int i = 0; i < 12; i++) {
		Point p;
		p.n = i + 1;
		p.passed = "2026-10-2" + std::to_string(2 + i % 7) + "T08:0" + std::to_string(i % 10) + ":00";
		p.compass = "NE";
		int mph = 20 + i % 10;
		p.zoneSeconds = 20.0 / (mph * 5280.0 / 3600.0);
		p.speed = mph;
		p.run = "stats_camera0_20261022.csv";
		p.frame = 100 * (i + 1);
		if (i == 10) p.ownDrive = 1;
		if (i == 11) { p.flagged = true; p.speed = 90; p.zoneSeconds = 20.0 / (90 * 5280.0 / 3600.0); }
		if (i == 3) { p.trend = "steady"; p.entryMph = 23; p.exitMph = 23; p.mphIn20ft = 23; p.accel = 0.3; p.passed = "a, \"quoted\" one"; }
		in.points.push_back(p);
	}
	in.summary = summarize(in.points, in.speedLimit, in.wayOver);
	return in;
}

static void testSummary() {
	Info in = sample();
	// Traffic only: 10 cars (own drive and flagged left out); over 25: 26..29 = 4; 85th percentile of 20..29 = 28.
	CHECK(in.summary.cars == 10 && in.summary.over == 4 && in.summary.wayOver == 0 && in.summary.p85 == 28);
	CHECK(accuracyWords(in.cal, 25) == "within \xC2\xB1" "1.0 mph at 25 mph (\xC2\xB1" "3.9%)");
}

static void testPublishAndCheck(const fs::path& work) {
	fs::path data = work / "data";
	fs::create_directories(data / "Stats");
	std::ofstream(data / "Stats" / "stats_camera0_20261022.csv") << "Date, Time, Frame, Direction\n";
	std::ofstream(data / "VST.cfg") << "CalibrationSpeed = 25\n";
	Info in = sample();
	archive::Request req;
	req.dataDir = data.string();
	req.root = "Published";
	req.move = false;
	req.checksums = true;
	req.info.name = in.name;
	req.info.created = in.published;
	req.statsCsvs = { (data / "Stats" / "stats_camera0_20261022.csv").string() };
	req.configFile = (data / "VST.cfg").string();
	in.id = fs::path(archive::folderFor((data / "Published").string(), in.published, in.name)).filename().string();
	req.info.dataset = coverJson(in);
	req.extra = { { "dataset.json", datasetJson(in) }, { "points.csv", pointsCsv(in) } };
	for (const Point& p : in.points) { archive::Source s; s.row.speed = p.speed; s.row.passed = p.passed; req.vehicles.push_back(s); }
	std::string dir, err;
	CHECK(archive::write(req, dir, err));
	CHECK(fs::path(dir).parent_path().filename() == "Published");
	CHECK(fs::exists(data / "Stats" / "stats_camera0_20261022.csv"));   // a copy: the set stays in Results
	CHECK(isDataset(dir));
	CHECK(slurp(fs::path(dir) / "review.html").find("Not re-checked") != std::string::npos);

	// Reads back as written, the quoted cell included.
	Info back;
	CHECK(read(dir, back, err));
	CHECK(back.id == in.id && back.name == in.name && back.site == in.site && back.points.size() == 12 && back.calibrationSet == 2);
	CHECK(back.cal.zoneFt[1] == 22.0 && back.cal.sigma[0] == 0.02 && back.cal.how == in.cal.how);
	CHECK(back.summary.cars == 10 && back.summary.p85 == 28 && back.status() == Verdict::None);
	CHECK(back.points[3].passed == "a, \"quoted\" one" && back.points[3].trend == "steady" && back.points[10].ownDrive == 1 && back.points[11].flagged);
	CHECK(std::fabs(back.points[0].zoneSeconds - in.points[0].zoneSeconds) < 1e-4);

	// Checksums: everything listed verifies; a changed point is caught.
	std::vector<std::string> changed;
	CHECK(verify(dir, changed) && changed.empty());
	std::string sums = slurp(fs::path(dir) / "SHA256SUMS");
	CHECK(sums.find("  points.csv\n") != std::string::npos && sums.find("  dataset.json\n") != std::string::npos
		&& sums.find("  archive.json\n") != std::string::npos && sums.find("review.html") == std::string::npos);

	// A newer calibration 7% shorter in L2R: every L2R speed drops 7% exactly; tainted, speeds read high.
	Calibration newer = back.cal;
	newer.zoneFt[0] = 20.0 * 0.93;
	newer.sigma[0] = 0.01;
	Check c;
	CHECK(check(back, newer, "2026-11-12T09:30:00", c, err));
	CHECK(c.verdict == Verdict::Tainted && c.test[0].verdict == Verdict::Tainted);
	CHECK(c.test[1].verdict == Verdict::None);   // no R2L cars: no test
	CHECK(std::fabs(c.meanDiff - (-0.07 * 24.5)) < 0.005);   // mean of 20..29 is 24.5 (ZoneSeconds is kept to 0.1 ms)
	CHECK(std::fabs(c.maxDiff - (-0.07 * 29)) < 0.005);
	CHECK(c.before.over == 4 && c.after.over == 2);           // 26..29 x 0.93 = 24.2, 25.1, 26.0, 27.0: 26 and 27 are over
	CHECK(c.file == "checks/2026-11-12_0930.csv");
	std::string rows = slurp(fs::path(dir) / c.file);
	CHECK(rows.find("1,L2R,20,18.60,-1.40,1\n") != std::string::npos);
	CHECK(rows.find("11,L2R,20,18.60,-1.40,0\n") != std::string::npos);   // the own drive is listed but not counted
	CHECK(sentence(back).find("read about 7.0% high") != std::string::npos);
	CHECK(slurp(fs::path(dir) / "review.html").find("Tainted: needs correction") != std::string::npos);
	// Recorded: status.json reads back; the checksummed files are untouched by a check.
	Info again;
	CHECK(read(dir, again, err) && again.checks.size() == 1 && again.status() == Verdict::Tainted && again.checks[0].after.over == 2);
	CHECK(verify(dir, changed) && changed.empty());
	// A second, agreeing check: the latest verdict is the status.
	newer.zoneFt[0] = 20.0 * 1.005;
	CHECK(check(again, newer, "2026-11-20T09:30:00", c, err) && again.status() == Verdict::Accurate);
	CHECK(sentence(again).find("still within") != std::string::npos);

	// Tampering is caught.
	{ std::ofstream(fs::path(dir) / "points.csv", std::ios::app) << "99,,L2R,NE,0,,15,,,,,,,0,0,x,1\n"; }
	CHECK(verify(dir, changed) && changed.size() == 1 && changed[0] == "points.csv");

	// list() finds it; a newer schema is refused in plain words.
	CHECK(list(data.string()).size() == 1);
	std::string ds = slurp(fs::path(dir) / "dataset.json");
	ds.replace(ds.find("vst-dataset/1"), 13, "vst-dataset/2");
	std::ofstream(fs::path(dir) / "dataset.json", std::ios::binary | std::ios::trunc) << ds;
	Info nope;
	CHECK(!read(dir, nope, err) && err.find("newer format") != std::string::npos);
}

int main(int argc, char** argv) {
	fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dataset-test";
	fs::remove_all(work);
	fs::create_directories(work);
	testSha();
	testEquivalence();
	testSummary();
	testPublishAndCheck(work);
	std::printf("%d of %d checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
