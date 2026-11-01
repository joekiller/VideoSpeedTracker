// Reopening VST Studio (src/Gui/Resume.cpp) unit test.
//
//   resume_test <work dir>
//
// Joe reopened Studio on macOS and it looked like a fresh install although everything was on disk: the USB camera
// wasn't reconnected, "Connect the camera" and "Mark your street" lost their checks, and Results showed an empty
// "This session".  These checks pin the rules that make reopening continuous:
//   - the remembered source comes back at launch for every kind, cameras included; only a file that is gone stays shut
//   - steps 3 and 4 are judged against the remembered source while the camera is unplugged or still connecting
//   - Results' "All saved runs" finds every stats_*.csv the tracker wrote, newest first, and nothing else in Stats/
#include "Resume.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace resume;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void testReopen() {
	// A USB camera ("0"), a Pi camera and a stream have no file to check: they are always tried (the regression).
	CHECK(reopenAtLaunch("0", false, false));
	CHECK(reopenAtLaunch("picam:0?w=1280", false, false));
	CHECK(reopenAtLaunch("rtsp://cam.local/stream", false, false));
	// A video or photo comes back while it exists; one moved away stays closed instead of failing at launch.
	CHECK(reopenAtLaunch("/videos/a.mp4", true, true));
	CHECK(!reopenAtLaunch("/videos/gone.mp4", true, false));
	// Nothing remembered (first run, or Disconnect was pressed): nothing to open.
	CHECK(!reopenAtLaunch("", false, false));
}

static void testSteps() {
	// Connected: the connected source wins, even over a different remembered one.
	CHECK(setupSource("1", "0") == "1");
	// Unplugged or still connecting at launch: the remembered one stands in, so the checks stay.
	CHECK(setupSource("", "0") == "0");
	CHECK(sourceStepDone(false, false, "0"));
	CHECK(zonesStepDone(setupSource("", "0"), "0", true));
	// The way it looked before the fix: step 4 judged on an empty source lost its check.
	CHECK(!zonesStepDone("", "0", true));
	// A different camera than the zones were drawn on still needs step 4 again, and so do invalid zones.
	CHECK(!zonesStepDone(setupSource("1", "0"), "0", true));
	CHECK(!zonesStepDone("0", "0", false));
	// Never connected / disconnected on purpose (lastSource cleared): step 3 isn't done, step 4 neither.
	CHECK(!sourceStepDone(false, false, ""));
	CHECK(!zonesStepDone(setupSource("", ""), "", true));
	// Connected now, or a batch queued: done.
	CHECK(sourceStepDone(true, false, ""));
	CHECK(sourceStepDone(false, true, ""));
}

static void touch(const fs::path& p, int ageSeconds) {
	std::ofstream(p) << "Date,Time,Frame,Direction\n";
	fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds));
}

static void testSavedRuns(const fs::path& work) {
	fs::remove_all(work);
	CHECK(savedRuns((work / "missing").string()).empty());
	CHECK(savedRuns("").empty());
	fs::path stats = work / "Stats";
	fs::create_directories(stats);
	// Joe's folder: camera runs over several days, their manifests, the daily health log and an exported csv.
	touch(stats / "stats_camera0_20261030_183745.csv", 3 * 86400);
	touch(stats / "stats_camera1_20261031_192859.csv", 2 * 86400);
	touch(stats / "stats_camera0_20261101_133012.csv", 60);
	touch(stats / "stats_20160130_172251.csv", 86400);
	touch(stats / "health.csv", 10);
	touch(stats / "speeders-export.csv", 5);
	std::ofstream(stats / "run_camera0_20261101_133012.json") << "{}";
	fs::create_directories(stats / "stats_folder.csv");   // a directory named like a run isn't one
	std::vector<SavedRun> runs = savedRuns(work.string());
	CHECK(runs.size() == 4);
	if (runs.size() == 4) {
		CHECK(runs[0].name == "camera0_20261101_133012");
		CHECK(runs[1].name == "20160130_172251");
		CHECK(runs[2].name == "camera1_20261031_192859");
		CHECK(runs[3].name == "camera0_20261030_183745");
		CHECK(fs::path(runs[0].csv) == stats / "stats_camera0_20261101_133012.csv");
	}
	// Same write time: the later stamp in the name comes first.
	auto t = fs::last_write_time(stats / "stats_camera0_20261101_133012.csv");
	touch(stats / "stats_camera0_20261101_140000.csv", 0);
	fs::last_write_time(stats / "stats_camera0_20261101_140000.csv", t);
	runs = savedRuns(work.string());
	CHECK(runs.size() == 5 && runs[0].name == "camera0_20261101_140000" && runs[1].name == "camera0_20261101_133012");
}

static void testLabels() {
	CHECK(runLabel("camera0_20261101_133012") == "Camera 0  ·  2026-11-01 13:30");
	CHECK(runLabel("picam1_20261101_133012") == "Pi camera 1  ·  2026-11-01 13:30");
	CHECK(runLabel("picam_20261101_133012") == "Pi camera  ·  2026-11-01 13:30");
	CHECK(runLabel("stream_20261101_133012") == "Stream  ·  2026-11-01 13:30");
	// Video files keep their own names.
	CHECK(runLabel("20160130_172251") == "20160130_172251");
	CHECK(runLabel("driveway_clip") == "driveway_clip");
	CHECK(runLabel("my_video_20261101_133012") == "my_video_20261101_133012");
	CHECK(runLabel("cameraX_20261101_133012") == "cameraX_20261101_133012");
}

int main(int argc, char** argv) {
	fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "vst-resume-test";
	testReopen();
	testSteps();
	testSavedRuns(work);
	testLabels();
	std::printf("resume_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
