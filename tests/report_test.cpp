// Street report (src/Gui/Report.cpp) unit test.
//
//   report_test <repo tests dir> <scratch dir>
//
// Builds reports from tests/fixtures/report/ and checks the numbers worked out by hand:
//
//   stats_cam_20261102.csv + run_cam_20261102.json   Mon 2 Nov 2026 07:00-09:00, outage 07:30-07:45 (1.75 h),
//                                                     20 rows (one flagged at 70 mph), 4 of them wrong way (SE)
//   stats_cam_20261103.csv + run_cam_20261103.json   Tue 3 Nov 17:00-18:00 (1 h), 10 rows, 1 wrong way
//   stats_manual_20261104120000.csv                  Wed 4 Nov, a 13-column Foscam csv with no manifest: times from
//                                                     Date/Time + Frame/30, coverage 12:00:10-12:05:00, 2 wrong way
//   calibration-history.csv                          applies and checks for VST.cfg (L2R -1.8, R2L +0.3), one other config
//
// The 34 speeds that count, sorted: 20 22 23 24 24 25 25 25 25 26 26 26 27 27 27 28 28 28 29 29 30 30 30 31 31 32 33 33
// 34 35 36 38 40 41.  Nearest-rank 85th = rank ceil(28.9) = 29 -> 34 mph; median = rank 17 -> 28; over 25 = 25 vehicles;
// 35+ = 5; max 41.  Then the same on the 2016 sample csv (tests/baseline_*.csv), which has neither manifest nor Timestamp.
#include "Report.h"
#include "Results.h"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <sstream>

namespace fs = std::filesystem;
using namespace report;

static int failures = 0, checks = 0;
template <class T> static std::string show(const T& v) {
	if constexpr (std::is_convertible_v<T, std::string>) return std::string(v);
	else return std::to_string(v);
}
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { checks++; auto va = (a); auto vb = (b); if (!(va == vb)) { failures++; std::printf("FAIL %s:%d  %s == %s  (got %s, want %s)\n", \
	__FILE__, __LINE__, #a, #b, show(va).c_str(), show(vb).c_str()); } } while (0)
#define CHECK_NEAR(a, b, eps) do { checks++; double va = (a), vb = (b); if (std::fabs(va - vb) > (eps)) { failures++; std::printf("FAIL %s:%d  %s ~ %s  (got %.4f, want %.4f)\n", \
	__FILE__, __LINE__, #a, #b, va, vb); } } while (0)

static bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

static const char* kConfig = "/home/joe/.config/videospeedtracker/VST.cfg";

static Input fixtureInput(const fs::path& dir) {
	Input in;
	in.dataDir = dir.string();
	in.configPath = kConfig;
	in.historyFiles = { (dir / "calibration-history.csv").string() };
	// The current VST.cfg: used for the old csv without a manifest.
	in.current.l2rCompass = "SE";
	in.current.r2lCompass = "NW";
	in.current.legal = "NW";     // a compass label, resolved to R2L
	return in;
}

static Settings fixtureSettings() {
	Settings s;
	s.siteName = "Locust Ave, 600 block";
	s.speedLimit = 25;
	s.wayOver = 35;
	s.generated = "2026-11-05";
	s.version = "1.2.0";
	return s;
}

// Every src= and href= must be inline (data: URI) or in-page (#...), and no URL with a scheme may appear anywhere.
static void checkSelfContained(const std::string& html, const char* what) {
	bool ok = !contains(html, "http://") && !contains(html, "https://");
	if (!ok) std::printf("  %s mentions an http(s) URL\n", what);
	CHECK(ok);
	// A plain scan rather than std::regex, whose backtracking overflows the stack on a long base64 image.
	std::string lower = html;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	for (const char* name : { "src=", "href=" })
		for (size_t p = lower.find(name); p != std::string::npos; p = lower.find(name, p + 1)) {
			size_t v = p + std::strlen(name);
			if (v < lower.size() && (lower[v] == '\'' || lower[v] == '"')) v++;
			std::string head = lower.substr(v, 6);
			bool inline_ = head.rfind("data:", 0) == 0 || head.rfind("#", 0) == 0;
			if (!inline_) std::printf("  %s: external reference %s\n", what, html.substr(p, 60).c_str());
			CHECK(inline_);
		}
	CHECK(!contains(lower, "url(http") && !contains(lower, "url(//"));
	CHECK(!contains(html, "@import"));
	// Product rules: never these claims.
	for (const char* banned : { "radar-accurate", "enforcement-grade", "evidence for a ticket" }) CHECK(!contains(html, banned));
}

static void testPercentile() {
	std::vector<int> v;
	for (int i = 1; i <= 20; i++) v.push_back(i);
	CHECK_EQ(percentile(v, 85), 17);       // rank ceil(17) = 17, where speeds[n*0.85] would give 18
	CHECK_EQ(percentile(v, 50), 10);
	CHECK_EQ(percentile(v, 100), 20);
	CHECK_EQ(percentile({ 42 }, 85), 42);
	CHECK_EQ(percentile({}, 85), 0);
	std::vector<int> ten = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	CHECK_EQ(percentile(ten, 85), 9);      // rank ceil(8.5) = 9
}

static void testParsing() {
	int64_t t;
	int off;
	bool has;
	CHECK(parseIso("2026-10-30T14:03:22-04:00", t, off, has));
	CHECK(has);
	CHECK_EQ(off, -4 * 3600);
	CHECK_EQ(t % 86400, int64_t(14 * 3600 + 3 * 60 + 22));
	CHECK(parseIso("2026-11-02 07:00:00", t, off, has));
	CHECK(!has);
	CHECK(!parseIso("not a time", t, off, has));
	int64_t d;
	CHECK(parseDay("2026-11-02", d));
	CHECK_EQ(dayString(d), std::string("2026-11-02"));
	CHECK(!parseDay("2026-11-02T07:00", d));
	CHECK_EQ(slug("Locust Ave, 600 block"), std::string("locust-ave-600-block"));
	CHECK_EQ(slug("  "), std::string("site"));
}

static void testFixtureAllData(const fs::path& fx, const fs::path& out) {
	Study st = analyze(fixtureInput(fx), fixtureSettings());
	CHECK_EQ(st.from, std::string("2026-11-02"));
	CHECK_EQ(st.to, std::string("2026-11-04"));
	CHECK_EQ(int(st.runs.size()), 3);
	// Counts.
	CHECK_EQ(st.all.n, 34);
	CHECK_EQ(st.flaggedExcluded, 1);
	CHECK_EQ(st.l2r.n, 7);
	CHECK_EQ(st.r2l.n, 27);
	// Nearest-rank statistics.
	CHECK_EQ(st.all.p85, 34);
	CHECK_EQ(st.all.median, 28);
	CHECK_EQ(st.all.max, 41);
	CHECK_EQ(st.all.over, 25);
	CHECK_NEAR(st.all.pctOver, 100.0 * 25 / 34, 1e-9);
	CHECK_EQ(st.all.over10, 5);
	CHECK_EQ(st.all.wayOver, 5);
	// Coverage: 2 h minus the 15 min outage, 1 h, and 12:00:10-12:05:00 estimated from the old csv's vehicles.
	CHECK_NEAR(st.hours, 1.75 + 1.0 + 290.0 / 3600, 1e-9);
	CHECK(st.coverageEstimated);
	int64_t mon;
	parseDay("2026-11-02", mon);
	CHECK_NEAR(st.dayHourSecs[mon][7], 45 * 60, 1e-9);
	CHECK_NEAR(st.dayHourSecs[mon][8], 3600, 1e-9);
	CHECK_NEAR(st.dayHourSecs[mon][9], 0, 1e-9);
	CHECK_EQ(int(st.gaps.size()), 2);     // Mon 09:00 -> Tue 17:00 and Tue 18:00 -> Wed 12:00; the outage is under an hour
	// Volumes from the two manifests.
	CHECK_EQ(st.manifestRuns, 2);
	CHECK_EQ(st.plainRuns, 1);
	CHECK_EQ(st.mLogged, 30);
	CHECK_EQ(st.mBelow, 4);
	CHECK_EQ(st.mUntimed, 2);
	// Hours of day, from each row's local time (the old csv's rebuilt from 12:00:00 + frame / 30).
	CHECK_EQ(st.hourN[7], 9);
	CHECK_EQ(st.hourN[8], 10);
	CHECK_EQ(st.hourN[12], 5);
	CHECK_EQ(st.hourN[17], 10);
	CHECK_EQ(st.hourP85[17], 36);         // 23 24 25 26 27 29 31 34 36 40: rank ceil(8.5) = 9
	CHECK_EQ(st.hourP85[12], 33);         // 25 27 28 30 33: rank 5
	CHECK_EQ(st.weekdays, 3);
	// Wrong way: one-way NW (R2L), so SE vehicles.
	CHECK_EQ(st.legal, std::string("R2L"));
	CHECK_EQ(st.wrongTotal, 7);
	CHECK_EQ(st.wrongByHour[7], 2);
	CHECK_EQ(st.wrongByHour[8], 2);
	CHECK_EQ(st.wrongByHour[12], 2);
	CHECK_EQ(st.wrongByHour[17], 1);
	// Calibration: both directions checked within 2 mph; the other config's row is left out.
	CHECK(st.historyFound);
	CHECK_EQ(int(st.calEvents.size()), 4);
	CHECK_EQ(st.historyOtherConfigs, 1);
	CHECK(st.verified[0] && st.verified[1]);
	CHECK_NEAR(st.checkError[0], -1.8, 1e-9);
	// Error band: 35 frames at 25 mph L2R -> 25/34 = 0.735, + |-1.8| check + 0.5 rounding = 3.035 -> 3.1.
	CHECK_NEAR(st.framesAtLimit[0], 35, 1e-9);
	CHECK_NEAR(st.framesAtLimit[1], 40, 1e-9);
	CHECK_NEAR(st.band, 3.1, 1e-9);
	// Caveats name what's missing.
	std::string cav;
	for (auto& c : st.caveats) cav += c + "\n";
	CHECK(contains(cav, "1 of 3 runs has no run manifest"));
	CHECK(contains(cav, "predates the Timestamp column"));
	CHECK(contains(cav, "rebuilt in 1 csv from the Date and Time columns"));
	CHECK(contains(cav, "flagged as implausible"));

	std::string html = renderHtml(st);
	// Kept for a look in a browser; companion/demo/report.html is a copy of it (the phone's demo-mode sample).
	std::ofstream(out / "fixture-report.html", std::ios::binary) << html;
	checkSelfContained(html, "fixture report");
	CHECK(contains(html, "Community traffic study. Estimated speeds, ±3.1 mph"));
	CHECK(contains(html, "Locust Ave, 600 block"));
	CHECK(contains(html, "Calibration verified"));
	CHECK(contains(html, "Wrong-way drivers"));
	CHECK(contains(html, "When speeding happens"));   // more than one weekday
	CHECK(contains(html, "Estimated coverage"));
	CHECK(contains(html, "Capture rate"));
	CHECK(contains(html, "nearest-rank"));
	CHECK(contains(html, "VideoSpeedTracker 1.2.0"));
	CHECK(!contains(html, "Example frames"));       // snapshots are off by default
	CHECK(contains(html, "@media print"));
}

static void testRangeAndCompare(const fs::path& fx) {
	Settings s = fixtureSettings();
	s.from = "2026-11-03";
	s.to = "2026-11-04";
	s.compareFrom = "2026-11-02";
	s.compareTo = "2026-11-02";
	Study st = analyze(fixtureInput(fx), s);
	CHECK_EQ(st.all.n, 15);
	CHECK_EQ(st.flaggedExcluded, 0);
	CHECK_NEAR(st.hours, 1.0 + 290.0 / 3600, 1e-9);
	CHECK_EQ(st.manifestRuns, 1);
	CHECK_EQ(st.mLogged, 10);
	CHECK_EQ(st.wrongTotal, 3);
	CHECK(st.hasCompare);
	CHECK_EQ(st.compare.all.n, 19);
	CHECK_NEAR(st.compare.hours, 1.75, 1e-9);
	CHECK_NEAR(st.compare.perHour, 20 / 1.75, 1e-9);   // logged per observed hour counts the flagged vehicle too
	CHECK_EQ(st.compare.wrongWay, 4);
	std::string html = renderHtml(st);
	CHECK(contains(html, "Before and after"));
	checkSelfContained(html, "range report");
	// A range with no data at all still renders.
	s.from = s.to = "2027-01-01";
	s.compareFrom = s.compareTo = "";
	Study empty = analyze(fixtureInput(fx), s);
	CHECK_EQ(empty.all.n, 0);
	CHECK_NEAR(empty.hours, 0, 1e-9);
	CHECK(contains(renderHtml(empty), "No vehicles were logged"));
}

static void testHistory(const fs::path& fx, const fs::path& out) {
	// No history: unverified, and the band is frame counting + rounding only: 0.735 + 0.5 -> 1.3.
	Input in = fixtureInput(fx);
	in.historyFiles = { (out / "missing.csv").string() };
	Study st = analyze(in, fixtureSettings());
	CHECK(!st.historyFound);
	CHECK(!st.verified[0] && !st.verified[1]);
	CHECK_NEAR(st.band, 1.3, 1e-9);
	std::string html = renderHtml(st);
	CHECK(contains(html, "Calibration not verified"));
	CHECK(contains(html, "No calibration history"));
	CHECK(contains(html, "±1.3 mph"));

	// A check off by more than 2 mph after the apply; and a new apply after a good check resets it.
	fs::path h = out / "history-bad.csv";
	std::ofstream(h) << "Timestamp, Config, Kind, Direction, DrivenMph, Frames, Fps, CalibrationFrames, MeasuredMph, Error\n"
		<< "2026-11-01T10:00:00-05:00, " << kConfig << ", check, R2L, 30, 33, 30, 40, 30.3, 0.3\n"
		<< "2026-11-01T11:00:00-05:00, " << kConfig << ", apply, R2L, 25, 41, 30, 41, ,\n"
		<< "2026-11-01T12:00:00-05:00, " << kConfig << ", apply, L2R, 25, 35, 30, 35, ,\n"
		<< "2026-11-01T12:10:00-05:00, " << kConfig << ", check, L2R, 30, 31, 30, 35, 32.5, 2.5\n";
	in.historyFiles = { h.string() };
	st = analyze(in, fixtureSettings());
	CHECK(st.historyFound);
	CHECK(!st.verified[0]);                // |2.5| > 2
	CHECK(st.hasCheck[0]);
	CHECK(!st.verified[1]);                // the check came before the latest apply
	CHECK(!st.hasCheck[1]);
	CHECK_NEAR(st.band, std::ceil((25.0 / 34 + 2.5 + .5) * 10) / 10, 1e-9);
	CHECK(contains(renderHtml(st), "Calibration not verified"));

	// A good check, but of a different CalibrationFrames than the runs used (VST.cfg edited by hand afterwards: no
	// history row).  The runs' manifests say L2R 35; the check tested 36.  Not checked, and its error leaves the band.
	fs::path h2 = out / "history-edited.csv";
	std::ofstream(h2) << "Timestamp, Config, Kind, Direction, DrivenMph, Frames, Fps, CalibrationFrames, MeasuredMph, Error\n"
		<< "2026-11-01T12:00:00-05:00, " << kConfig << ", apply, L2R, 25, 36, 30, 36, ,\n"
		<< "2026-11-01T12:10:00-05:00, " << kConfig << ", check, L2R, 30, 30, 30, 36, 30.0, 0.0\n"
		<< "2026-11-01T12:20:00-05:00, " << kConfig << ", check, R2L, 30, 34, 30, 40, 29.4, -0.6\n";
	in.historyFiles = { h2.string() };
	st = analyze(in, fixtureSettings());
	CHECK(st.checkMismatch[0]);
	CHECK(!st.verified[0]);
	CHECK_EQ(st.usedCalFrames[0], 35);
	CHECK_EQ(st.checkCalFrames[0], 36);
	CHECK(!st.checkMismatch[1]);
	CHECK(st.verified[1]);
	CHECK_NEAR(st.calMph, 0.6, 1e-9);
	html = renderHtml(st);
	CHECK(contains(html, "Calibration partly verified"));
	CHECK(contains(html, "Not checked: the runs used CalibrationFrames 35, the check tested 36"));
}

static void testWriteAndSnapshots(const fs::path& fx, const fs::path& out) {
	// Copy the fixtures so Reports/ is written in the build tree, never in the source tree.
	fs::path data = out / "data";
	fs::remove_all(data);
	fs::create_directories(data);
	fs::copy(fx, data, fs::copy_options::recursive);
	Input in = fixtureInput(data);
	// A noisy frame: after blurForReport the bottom (plate-height) band must be smooth, the top left alone.
	cv::Mat noise(200, 480, CV_8UC3);
	cv::randu(noise, cv::Scalar::all(0), cv::Scalar::all(255));
	std::string jpeg = blurForReport(noise);
	CHECK(!jpeg.empty());
	cv::Mat back = cv::imdecode(std::vector<uchar>(jpeg.begin(), jpeg.end()), cv::IMREAD_GRAYSCALE);
	CHECK(!back.empty());
	if (!back.empty()) {
		cv::Scalar m, top, bottom;
		cv::meanStdDev(back.rowRange(0, back.rows / 3), m, top);
		cv::meanStdDev(back.rowRange(back.rows * 2 / 3, back.rows), m, bottom);
		CHECK(bottom[0] < top[0] / 3);
	}
	in.snapshots.push_back({ jpeg, 41, true, "2026-11-02T08:46:00" });
	Settings s = fixtureSettings();
	s.includeSnapshots = true;
	std::string path, html, err;
	CHECK(writeReport(in, s, path, html, err));
	CHECK_EQ(fs::path(path).filename().string(), std::string("report_locust-ave-600-block_2026-11-02_2026-11-04.html"));
	CHECK(fs::exists(path));
	CHECK(contains(html, "Example frames"));
	CHECK(contains(html, "data:image/jpeg;base64,"));
	checkSelfContained(html, "report with snapshots");
	// Asked for snapshots, none available (a csv-loaded run): the report says so instead.
	in.snapshots.clear();
	Study st = analyze(in, s);
	std::string cav;
	for (auto& c : st.caveats) cav += c;
	CHECK(contains(cav, "none are available"));
	CHECK(!contains(renderHtml(st), "data:image/jpeg"));
}

// The 2016 Locust Avenue sample: 13 columns, Date/Time = recording start, no manifest, no history, two-way street.
static void testSampleCsv(const fs::path& tests, const fs::path& out) {
	fs::path csv = tests / "baseline_manual_20160130172251.csv";
	std::vector<StatsRow> rows;
	std::string err;
	CHECK(readStatsCsv(csv.string(), rows, err));
	std::vector<int> speeds;
	int over = 0;
	for (auto& r : rows) { speeds.push_back(r.speed); over += r.speed > 25; }
	std::sort(speeds.begin(), speeds.end());
	// Independent nearest rank, written out the long way.
	int n = int(speeds.size());
	int rank = (85 * n + 99) / 100;

	Input in;
	in.dataDir = (out / "sample").string();
	in.csvFiles = { csv.string() };
	in.historyFiles = {};
	Settings s = fixtureSettings();
	s.siteName = "Locust Avenue";
	Study st = analyze(in, s);
	CHECK_EQ(st.all.n, n);
	CHECK(n > 50);
	CHECK_EQ(st.all.p85, speeds[rank - 1]);
	CHECK_EQ(st.all.over, over);
	CHECK_EQ(st.all.max, speeds.back());
	CHECK_EQ(st.from, std::string("2016-01-30"));
	CHECK_EQ(st.to, std::string("2016-01-30"));
	// The clip runs from 17:22:51 for 10 minutes; vehicles span most of it, all in the 5 pm hour.
	CHECK(st.hours > 0.1 && st.hours < 10.0 / 60);
	CHECK(st.coverageEstimated);
	CHECK_EQ(st.hourN[17], n);
	CHECK_EQ(st.legal, std::string("Both"));
	CHECK_EQ(st.wrongTotal, 0);
	CHECK(!st.historyFound);
	std::string html = renderHtml(st);
	checkSelfContained(html, "sample report");
	CHECK(!contains(html, "Wrong-way drivers"));       // two-way street
	CHECK(!contains(html, "When speeding happens"));   // one weekday only
	CHECK(contains(html, "Calibration not verified"));
	std::string path, h2;
	CHECK(writeReport(in, s, path, h2, err));
	std::printf("sample report: %s (%d vehicles, 85th %d mph, ±%.1f mph)\n", path.c_str(), st.all.n, st.all.p85, st.band);
}

int main(int argc, char** argv) {
	if (argc < 3) { std::printf("usage: report_test <tests dir> <scratch dir>\n"); return 2; }
	fs::path tests = argv[1], out = argv[2];
	fs::create_directories(out);
	fs::path fx = tests / "fixtures" / "report";
	testPercentile();
	testParsing();
	testFixtureAllData(fx, out);
	testRangeAndCompare(fx);
	testHistory(fx, out);
	testWriteAndSnapshots(fx, out);
	testSampleCsv(tests, out);
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
