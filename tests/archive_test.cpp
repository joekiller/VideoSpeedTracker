// Archived datasets (src/Gui/Archive.cpp) unit test.
//
//   archive_test <repo tests dir> <scratch dir>
//
// Builds a data folder in the scratch dir from the street-report fixtures (two stats csvs with manifests, one without),
// a highlights video, two clips and a calibration history, then checks:
//   - write(move) puts every file in <data>/Archive/<date>_<slug>/ and only then takes the originals away;
//   - read() gets back the vehicles, snapshots, clips, limits and calibration; list() finds it, newest first;
//   - the street report made from the archive folder sees the same runs as from the data folder (34 counted speeds);
//   - a second archive with the same name gets "-2"; a copy (move = false) leaves the data folder alone;
//   - a csv that is gone or a clip that isn't a file is skipped; a data folder that can't hold Archive/ fails cleanly;
//   - restore() puts the Stats and HiLites files back without overwriting, and clears "moved";
//   - review.html carries the data and can't be broken out of by a source name holding "</script>".
#include "Archive.h"
#include "Report.h"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { checks++; auto va = (a); auto vb = (b); if (!(va == vb)) { failures++; std::printf("FAIL %s:%d  %s == %s\n", \
	__FILE__, __LINE__, #a, #b); } } while (0)

static void put(const fs::path& p, const std::string& text) {
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << text;
}
static std::string slurp(const fs::path& p) {
	std::ifstream in(p, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

int main(int argc, char** argv) {
	if (argc < 3) { std::printf("usage: archive_test <tests dir> <scratch dir>\n"); return 2; }
	fs::path fixtures = fs::path(argv[1]) / "fixtures" / "report" / "Stats", scratch = argv[2];
	fs::remove_all(scratch);
	fs::path data = scratch / "data", cfgDir = scratch / "config";
	fs::create_directories(data / "Stats");
	fs::create_directories(data / "Clips");
	const char* runs[] = { "cam_20261102", "cam_20261103", "manual_20261104120000" };
	for (const char* r : runs) {
		std::string stem = r;
		fs::copy_file(fixtures / ("stats_" + stem + ".csv"), data / "Stats" / ("stats_" + stem + ".csv"));
		if (fs::exists(fixtures / ("run_" + stem + ".json"))) fs::copy_file(fixtures / ("run_" + stem + ".json"), data / "Stats" / ("run_" + stem + ".json"));
	}
	put(data / "HiLites" / "Hilites_cam_20261102.mp4", "fake highlights");
	put(data / "Clips" / "vehicle_1.mp4", "clip one");
	put(data / "Clips" / "vehicle_2.mp4", "clip two");
	put(data / "Clips" / "other_run.mp4", "not archived");
	put(cfgDir / "VST.cfg", "CalibrationFramesL2R = 35\n");
	fs::copy_file(fixtures.parent_path() / "calibration-history.csv", cfgDir / "calibration-history.csv");

	// helpers
	CHECK_EQ(archive::manifestFor((data / "Stats" / "stats_cam_20261102.csv").string()), (data / "Stats" / "run_cam_20261102.json").string());
	CHECK_EQ(archive::highlightsFor((data / "Stats" / "stats_cam_20261102.csv").string()), (data / "HiLites" / "Hilites_cam_20261102.mp4").string());

	// The report from the live data folder, to compare with the archive's.
	report::Input live;
	live.dataDir = data.string();
	live.configPath = "VST.cfg";
	live.historyFiles = { (cfgDir / "calibration-history.csv").string() };
	report::Study before = report::analyze(live, report::Settings());

	archive::Request req;
	req.dataDir = data.string();
	req.info.name = "First week, before the signs";
	req.info.created = "2026-11-05T09:30:00-05:00";
	req.info.from = "This session";
	req.info.siteName = "7½ St SW";
	req.info.speedLimit = 15;
	req.info.wayOver = 25;
	req.info.vst = "test";
	req.info.configPath = "VST.cfg";
	req.info.cal.legal = "R2L";
	req.info.cal.framesL2R = 33;
	for (const char* r : runs) req.statsCsvs.push_back((data / "Stats" / ("stats_" + std::string(r) + ".csv")).string());
	req.statsCsvs.push_back(req.statsCsvs[0]);   // named twice: copied once
	req.configFile = (cfgDir / "VST.cfg").string();
	req.historyFiles = { (cfgDir / "missing.csv").string(), (cfgDir / "calibration-history.csv").string() };
	archive::Source a, b, c;
	a.row.speed = 31; a.row.passed = "2026-11-02T07:05:00"; a.row.compass = "SE"; a.row.source = "cam</script><b>x";
	a.snapshot = cv::Mat(40, 96, CV_8UC3, cv::Scalar(20, 120, 220));
	a.clipPath = (data / "Clips" / "vehicle_1.mp4").string();
	b.row.speed = 12; b.row.l2r = false; b.row.wrongWay = true; b.row.passed = "2026-11-03T17:10:00"; b.row.compass = "NW";
	b.clipPath = (data / "Clips" / "vehicle_2.mp4").string();
	c.row.speed = 40; c.row.frame = 900; c.row.fps = 30;   // a recording with no time: "0:30 in video"
	// What Results adds beyond the csv (sizes, speed trend, the user's own drive) survives the round trip.
	a.row.lengthFt = 15.24; a.row.trend = "accelerating"; a.row.entryMph = 29; a.row.exitMph = 33; a.row.accel = 3.5; a.row.mphIn20ft = 34;
	b.row.ownDrive = 1; b.row.drivenMph = 15;
	req.vehicles = { a, b, c };

	std::string dir, err;
	std::vector<std::string> warnings;
	CHECK(archive::write(req, dir, err, &warnings));
	if (!err.empty()) std::printf("write: %s\n", err.c_str());
	fs::path ad(dir);
	CHECK_EQ(ad.filename().string(), std::string("2026-11-05_first-week-before-the-signs"));
	CHECK(!fs::exists(ad.string() + ".partial"));
	for (const char* f : { "archive.json", "vehicles.csv", "review.html", "VST.cfg", "calibration-history.csv",
		"Stats/stats_cam_20261102.csv", "Stats/run_cam_20261102.json", "Stats/stats_cam_20261103.csv", "Stats/run_cam_20261103.json",
		"Stats/stats_manual_20261104120000.csv", "HiLites/Hilites_cam_20261102.mp4", "Clips/vehicle_1.mp4", "Clips/vehicle_2.mp4",
		"Snapshots/vehicle_1.jpg" })
		CHECK(fs::is_regular_file(ad / f));
	CHECK(!fs::exists(ad / "Snapshots" / "vehicle_2.jpg"));
	CHECK(!fs::exists(ad / "Clips" / "other_run.mp4"));
	CHECK(warnings.empty());
	// Moved: the originals are gone, what wasn't archived stays.
	CHECK(!fs::exists(data / "Stats" / "stats_cam_20261102.csv"));
	CHECK(!fs::exists(data / "Stats" / "run_cam_20261102.json"));
	CHECK(!fs::exists(data / "HiLites" / "Hilites_cam_20261102.mp4"));
	CHECK(!fs::exists(data / "Clips" / "vehicle_1.mp4"));
	CHECK(fs::exists(data / "Clips" / "other_run.mp4"));
	CHECK(fs::exists(cfgDir / "VST.cfg"));   // the config and history are copied, never moved
	CHECK(fs::exists(cfgDir / "calibration-history.csv"));
	CHECK(cv::imread((ad / "Snapshots" / "vehicle_1.jpg").string()).cols == 96);

	// Read it back.
	archive::Info info;
	CHECK(archive::read(dir, info, err));
	CHECK(archive::read((ad / "archive.json").string(), info, err));
	CHECK_EQ(info.name, req.info.name);
	CHECK_EQ(info.siteName, std::string("7½ St SW"));
	CHECK_EQ(info.speedLimit, 15);
	CHECK_EQ(info.wayOver, 25);
	CHECK_EQ(info.cal.legal, std::string("R2L"));
	CHECK_EQ(info.cal.framesL2R, 33);
	CHECK(info.moved);
	CHECK_EQ(info.vehicles.size(), size_t(3));
	CHECK_EQ(info.vehicles[0].snapshot, std::string("Snapshots/vehicle_1.jpg"));
	CHECK_EQ(info.vehicles[0].clip, std::string("Clips/vehicle_1.mp4"));
	CHECK_EQ(info.vehicles[0].source, std::string("cam</script><b>x"));
	CHECK(!info.vehicles[1].l2r && info.vehicles[1].wrongWay && info.vehicles[1].snapshot.empty());
	CHECK_EQ(info.vehicles[2].frame, 900);
	CHECK(std::fabs(info.vehicles[0].lengthFt - 15.2) < 1e-9 && info.vehicles[0].trend == "accelerating" && info.vehicles[0].entryMph == 29
		&& info.vehicles[0].exitMph == 33 && std::fabs(info.vehicles[0].accel - 3.5) < 1e-9 && info.vehicles[0].mphIn20ft == 34);
	CHECK(info.vehicles[1].ownDrive == 1 && info.vehicles[1].drivenMph == 15 && info.vehicles[1].trend.empty() && info.vehicles[2].ownDrive == 0);
	CHECK_EQ(info.firstPassed, std::string("2026-11-02T07:05:00"));
	CHECK_EQ(info.lastPassed, std::string("2026-11-03T17:10:00"));
	CHECK(std::find(info.files.begin(), info.files.end(), "Stats/run_cam_20261103.json") != info.files.end());
	CHECK(std::find(info.files.begin(), info.files.end(), "archive.json") == info.files.end());
	std::string csv = slurp(ad / "vehicles.csv");
	CHECK(csv.find("1,2026-11-02T07:05:00,31,L2R,SE,,") != std::string::npos);
	CHECK(csv.find("2,2026-11-03T17:10:00,12,R2L,NW,yes,") != std::string::npos);

	// The street report made from the archive sees the same runs as it did from the data folder.
	report::Input arch = live;
	arch.dataDir = dir;
	arch.historyFiles = { (ad / "calibration-history.csv").string() };
	report::Study after = report::analyze(arch, report::Settings());
	CHECK_EQ(after.runs.size(), before.runs.size());
	CHECK_EQ(after.all.n, before.all.n);
	CHECK_EQ(after.all.n, 34);
	CHECK_EQ(after.all.p85, before.all.p85);
	CHECK_EQ(after.calEvents.size(), before.calEvents.size());
	// And the live folder no longer has those runs.
	CHECK_EQ(report::analyze(live, report::Settings()).runs.size(), size_t(0));

	// review.html: the data is in it, and "</script>" in a name is escaped.
	std::string html = slurp(ad / "review.html");
	CHECK(html.find("First week, before the signs") != std::string::npos);
	CHECK(html.find("cam<\\/script>") != std::string::npos);
	CHECK(html.find("cam</script>") == std::string::npos);
	CHECK(html.find("http") == std::string::npos);   // nothing external

	// A second archive with the same name and day gets "-2"; a copy leaves the data folder alone.
	put(data / "Stats" / "stats_cam_20261110.csv", slurp(fixtures / "stats_cam_20261103.csv"));
	archive::Request copy = req;
	copy.statsCsvs = { (data / "Stats" / "stats_cam_20261110.csv").string() };
	copy.vehicles = { c };
	copy.move = false;
	std::string dir2;
	CHECK(archive::write(copy, dir2, err));
	CHECK_EQ(fs::path(dir2).filename().string(), std::string("2026-11-05_first-week-before-the-signs-2"));
	CHECK(fs::exists(data / "Stats" / "stats_cam_20261110.csv"));
	archive::Info info2;
	CHECK(archive::read(dir2, info2, err) && !info2.moved);

	// list(): both, newest first (same created: by folder name, so "-2" first).
	copy.info.created = "2026-11-06T08:00:00-05:00";
	copy.info.name = "Later";
	std::string dir3;
	CHECK(archive::write(copy, dir3, err));
	std::vector<archive::Info> all = archive::list(data.string());
	CHECK_EQ(all.size(), size_t(3));
	if (all.size() == 3) {
		CHECK_EQ(all[0].name, std::string("Later"));
		CHECK_EQ(fs::path(all[1].dir).filename().string(), std::string("2026-11-05_first-week-before-the-signs-2"));
	}
	// An unrelated folder, a half-written archive and a foreign archive.json are skipped.
	fs::create_directories(data / "Archive" / "holiday photos");
	put(data / "Archive" / "2026-11-07_x.partial" / "archive.json", "{\"format\":\"vst-archive\"}");
	put(data / "Archive" / "junk" / "archive.json", "{\"format\":\"other\"}");
	CHECK_EQ(archive::list(data.string()).size(), size_t(3));
	CHECK(!archive::read((data / "Archive" / "junk").string(), info2, err));
	CHECK(!archive::read((data / "Archive" / "holiday photos").string(), info2, err));

	// Missing pieces don't stop an archive: a csv that is gone and a clip that isn't a file are skipped with a warning
	// (the csv) or silently (the clip), and the vehicles are still archived.
	archive::Request gaps = copy;
	gaps.info.name = "Gaps";
	gaps.move = true;
	gaps.statsCsvs = { (data / "Stats" / "stats_cam_20261110.csv").string(), (data / "Stats" / "stats_gone.csv").string() };
	archive::Source d = c;
	fs::create_directories(data / "Clips" / "folder.mp4");
	d.clipPath = (data / "Clips" / "folder.mp4").string();
	gaps.vehicles = { d };
	std::string dir4;
	warnings.clear();
	CHECK(archive::write(gaps, dir4, err, &warnings));
	CHECK_EQ(warnings.size(), size_t(1));
	CHECK(!warnings.empty() && warnings[0].find("stats_gone.csv") != std::string::npos);
	CHECK(!fs::exists(data / "Stats" / "stats_cam_20261110.csv"));   // moved into "Gaps"
	CHECK(fs::is_directory(data / "Clips" / "folder.mp4"));          // never touched
	archive::Info info4;
	CHECK(archive::read(dir4, info4, err) && info4.vehicles.size() == 1 && info4.vehicles[0].clip.empty());

	// Nothing to write into: a data folder that is a file fails cleanly, with nothing moved.
	put(scratch / "not-a-folder", "x");
	archive::Request nowhere = req;
	nowhere.dataDir = (scratch / "not-a-folder").string();
	std::string dir5;
	CHECK(!archive::write(nowhere, dir5, err));
	CHECK(!err.empty() && dir5.empty());

	// restore(): Stats and HiLites back into the data folder; an existing file is kept and named.
	put(data / "Stats" / "stats_cam_20261103.csv", "user's newer file");
	CHECK(archive::restore(dir, data.string(), err));
	CHECK(err.find("stats_cam_20261103.csv") != std::string::npos);
	CHECK_EQ(slurp(data / "Stats" / "stats_cam_20261103.csv"), std::string("user's newer file"));
	CHECK(fs::exists(data / "Stats" / "stats_cam_20261102.csv"));
	CHECK(fs::exists(data / "Stats" / "run_cam_20261102.json"));
	CHECK(fs::exists(data / "HiLites" / "Hilites_cam_20261102.mp4"));
	CHECK(fs::exists(ad / "Stats" / "stats_cam_20261102.csv"));   // the archive keeps its copy
	CHECK(archive::read(dir, info, err) && !info.moved);
	CHECK_EQ(info.vehicles.size(), size_t(3));

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
