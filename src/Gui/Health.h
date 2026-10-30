// Keeping an unattended VST healthy for weeks: the disk budget for video, the "camera moved" check, and the daily
// health log.  No ImGui or OpenGL, so tests/health_test.cpp links it on its own (vst_report library).
//
// Disk: per-vehicle clips (<data>/Clips) and highlight videos (<data>/HiLites) grow without end on a live camera,
// so App prunes them to a budget, oldest first.  Stats csvs, run manifests, reports and the calibration history are
// never touched: they are small and they are the evidence.  When the disk itself runs low (under minFree), the
// target drops further so the stats csv always has room.
//
// Camera moved: a reference view of the street is kept with the zones (<configDir>/view-reference.jpg, rewritten
// whenever the zones change).  During a live run the current frame is compared with it by phase correlation of
// edge images, which ignores passing cars and most day/night changes: a consistent shift of more than ~10 px
// (in 1280x720 frame pixels) means the zones no longer sit on the street.  Too little structure to tell (night,
// fog, a covered lens) is reported as unclear, never as moved.
//
// Daily log: <data>/Stats/health.csv, one row per day a live source was tracked (written when the day ends or the
// app closes), so a gap in the counts can be told apart from a quiet street.
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace health {

namespace fs = std::filesystem;

struct Usage {
	std::uintmax_t bytes = 0;
	size_t files = 0;
};
// Regular files directly in each folder (not recursive); missing folders count as empty.
Usage usage(const std::vector<fs::path>& dirs);

// Free space on the volume holding `path`, or 0 when it can't be read.
std::uintmax_t freeBytes(const fs::path& path);

// How many bytes of video to keep: the budget (0 = unlimited), lowered so that pruning brings free space back up to
// minFree when the disk is nearly full.
std::uintmax_t targetBytes(const Usage& used, std::uintmax_t budget, std::uintmax_t freeNow, std::uintmax_t minFree);

struct Pruned {
	std::vector<std::string> removed;   // full paths, oldest first
	std::uintmax_t freed = 0;
	Usage left;
};
// Removes the oldest files (by modification time) from dirs until they total at most `target` bytes.  Files modified
// within `protect` of now are kept whatever the total: the clip or highlight video still being written.
Pruned pruneTo(const std::vector<fs::path>& dirs, std::uintmax_t target, std::chrono::seconds protect = std::chrono::minutes(10));

struct ViewCheck {
	enum Result { Same, Moved, Unclear } result = Unclear;
	double dx = 0, dy = 0;      // shift of the current view from the reference, in 1280x720 frame pixels
	double confidence = 0;      // phase correlation peak, 0..1; under kMinConfidence the result is Unclear
	const char* name() const { return result == Same ? "same" : result == Moved ? "moved" : "unclear"; }
};
constexpr double kMovedPx = 10;           // shifts above this count as moved
constexpr double kMinConfidence = 0.08;   // below this there isn't enough shared structure to tell
// Compares two BGR or gray frames of any size (each scaled to 1280x720 first, as the tracker does).
ViewCheck compareView(const cv::Mat& reference, const cv::Mat& current);

// One day of a live run, as a health.csv row.
struct Day {
	std::string date;               // YYYY-MM-DD, local
	double trackedHours = 0;        // a live source was running and delivering frames
	double outageHours = 0;         // a live source was running but reconnecting
	int outages = 0;                // times it dropped out
	int vehicles = 0;               // logged that day
	int clipsRemoved = 0;           // video files pruned for the disk budget
	double freeGB = -1;             // lowest free space seen (-1 = unknown)
	std::string camera = "unchecked";   // worst camera check that day: same, unclear, moved or unchecked
	bool empty() const { return trackedHours <= 0 && outageHours <= 0; }
};
const char* csvHeader();            // "Date, TrackedHours, ..."
std::string csvRow(const Day& d);
// Appends d to path, writing the header first when the file is new.  Returns false on an I/O error.
bool appendDay(const fs::path& path, const Day& d);

}  // namespace health
