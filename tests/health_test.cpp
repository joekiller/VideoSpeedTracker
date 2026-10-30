// Unattended-run health (src/Gui/Health.cpp) unit test.
//
//   health_test <repo root> <scratch dir>
//
// Disk budget: builds a Clips/ and HiLites/ folder of files with staggered modification times and checks that
// pruning removes the oldest first, stops at the target, spares recent files and never looks outside the folders.
// Camera check: frames from the 2016 sample video against each other (cars move, the camera doesn't: same), shifted
// by 30 px (moved), darkened to dusk (same), and a black frame (unclear, never moved).  Daily log: the csv row and
// that appendDay writes the header once.
#include "Health.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace health;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void writeFile(const fs::path& p, size_t bytes, std::chrono::minutes age) {
	std::ofstream(p, std::ios::binary) << std::string(bytes, 'x');
	fs::last_write_time(p, fs::file_time_type::clock::now() - age);
}

static void testPrune(const fs::path& out) {
	fs::path clips = out / "Clips", hl = out / "HiLites", stats = out / "Stats";
	fs::remove_all(out);
	for (auto& d : { clips, hl, stats }) fs::create_directories(d);
	// 6 clips of 100 bytes aged 60..10 minutes (clip0 oldest), one 500-byte highlight 45 min old, one 100-byte clip
	// written 1 minute ago (protected), and a stats csv that must never be touched.
	for (int i = 0; i < 6; i++) writeFile(clips / ("clip" + std::to_string(i) + ".mp4"), 100, std::chrono::minutes(60 - i * 10));
	writeFile(hl / "Hilites_a.mp4", 500, std::chrono::minutes(45));
	writeFile(clips / "fresh.mp4", 100, std::chrono::minutes(1));
	writeFile(stats / "stats_a.csv", 1000, std::chrono::minutes(120));
	std::vector<fs::path> dirs = { clips, hl };

	Usage u = usage(dirs);
	CHECK(u.files == 8);
	CHECK(u.bytes == 1200);
	CHECK(usage({ out / "missing" }).files == 0);

	// Budget 1000: the two oldest clips go (60 and 50 minutes), then the total is 1000.
	Pruned p = pruneTo(dirs, 1000);
	CHECK(p.removed.size() == 2);
	CHECK(p.freed == 200);
	CHECK(!fs::exists(clips / "clip0.mp4") && !fs::exists(clips / "clip1.mp4"));
	CHECK(fs::exists(clips / "clip2.mp4"));
	CHECK(p.left.bytes == 1000);

	// Budget 0: everything but the protected fresh clip goes, oldest first (the 45-minute highlight before the
	// 40-minute clip2); the stats csv stays.
	p = pruneTo(dirs, 0);
	CHECK(p.removed.size() == 5);
	CHECK(p.removed.size() >= 2 && p.removed[0].find("Hilites_a") != std::string::npos && p.removed[1].find("clip2") != std::string::npos);
	CHECK(fs::exists(clips / "fresh.mp4"));
	CHECK(fs::exists(stats / "stats_a.csv"));
	CHECK(p.left.files == 1);

	// Already under budget: nothing happens.
	CHECK(pruneTo(dirs, 1u << 20).removed.empty());

	// Targets: the budget caps usage, 0 means unlimited, and a nearly full disk lowers the target further.
	Usage big{ 50000, 10 };
	CHECK(targetBytes(big, 0, 1u << 30, 1000) == 50000);
	CHECK(targetBytes(big, 20000, 1u << 30, 1000) == 20000);
	CHECK(targetBytes(big, 20000, 400, 1000) == 19400);
	CHECK(targetBytes(big, 0, 400, 1000) == 49400);
	CHECK(targetBytes(Usage{ 300, 1 }, 0, 400, 1000) == 0);
	CHECK(targetBytes(big, 20000, 0, 1000) == 20000);   // free space unknown: the budget alone
	CHECK(freeBytes(out) > 0);
}

static void testCamera(const fs::path& root) {
	cv::VideoCapture cap((root / "sampleData" / "IPCam" / "20160130" / "manual_20160130172251.avi").string());
	cv::Mat a, b, f;
	for (int i = 0; i < 400 && cap.read(f); i++) {
		if (i == 10) a = f.clone();
		if (i == 390) b = f.clone();
	}
	CHECK(!a.empty() && !b.empty());
	if (a.empty() || b.empty()) return;

	ViewCheck same = compareView(a, b);
	std::printf("  12 s apart: %s, shift %.1f,%.1f px, confidence %.3f\n", same.name(), same.dx, same.dy, same.confidence);
	CHECK(same.result == ViewCheck::Same);

	// The camera knocked 30 px to the right (in 1280-wide frame pixels): the scene appears 30 px to the left... or
	// right, depending on convention, so only the size of the shift is checked.
	cv::Mat shifted, m = (cv::Mat_<double>(2, 3) << 1, 0, 30.0 * a.cols / 1280, 0, 1, 12.0 * a.rows / 720);
	cv::warpAffine(b, shifted, m, b.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
	ViewCheck moved = compareView(a, shifted);
	std::printf("  shifted 30,12: %s, shift %.1f,%.1f px, confidence %.3f\n", moved.name(), moved.dx, moved.dy, moved.confidence);
	CHECK(moved.result == ViewCheck::Moved);
	CHECK(std::fabs(std::hypot(moved.dx, moved.dy) - std::hypot(30.0, 12.0)) < 4);

	// Dusk: the same view at a third of the brightness with sensor noise is still the same view.
	cv::Mat dusk, noise(b.size(), b.type());
	b.convertTo(dusk, -1, 0.35, 0);
	cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(6));
	dusk += noise;
	ViewCheck d = compareView(a, dusk);
	std::printf("  dusk: %s, shift %.1f,%.1f px, confidence %.3f\n", d.name(), d.dx, d.dy, d.confidence);
	CHECK(d.result == ViewCheck::Same);

	// A covered lens or a dead-black night: nothing to compare, so unclear, never "moved".
	cv::Mat black = cv::Mat::zeros(b.size(), b.type()), dark = black.clone();
	cv::randn(dark, cv::Scalar::all(4), cv::Scalar::all(3));
	CHECK(compareView(a, black).result == ViewCheck::Unclear);
	CHECK(compareView(a, dark).result != ViewCheck::Moved);
	CHECK(compareView(cv::Mat(), b).result == ViewCheck::Unclear);
}

static void testDayLog(const fs::path& out) {
	Day d;
	d.date = "2026-11-02";
	d.trackedHours = 23.5;
	d.outageHours = 0.25;
	d.outages = 2;
	d.vehicles = 812;
	d.clipsRemoved = 40;
	d.freeGB = 112.34;
	d.camera = "same";
	CHECK(csvRow(d) == "2026-11-02, 23.50, 0.25, 2, 812, 40, 112.3, same");
	Day unknown;
	unknown.date = "2026-11-03";
	unknown.trackedHours = 1;
	CHECK(csvRow(unknown) == "2026-11-03, 1.00, 0.00, 0, 0, 0, , unchecked");
	CHECK(Day().empty() && !d.empty());

	fs::path log = out / "Stats" / "health.csv";
	fs::remove(log);
	CHECK(appendDay(log, d));
	CHECK(appendDay(log, unknown));
	std::ifstream in(log);
	std::string l1, l2, l3, l4;
	std::getline(in, l1); std::getline(in, l2); std::getline(in, l3);
	CHECK(l1 == csvHeader());
	CHECK(l2 == csvRow(d));
	CHECK(l3 == csvRow(unknown));
	CHECK(!std::getline(in, l4));
}

int main(int argc, char** argv) {
	if (argc < 3) { std::printf("usage: health_test <repo root> <scratch dir>\n"); return 2; }
	fs::path root = argv[1], out = argv[2];
	fs::create_directories(out);
	testPrune(out);
	testCamera(root);
	testDayLog(out);
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
