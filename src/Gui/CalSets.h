// Calibration sets: which runs were measured from the same view, what calibration each set's speeds are shown at, and
// whether a set is still collecting (open) or frozen (locked).  The one model behind Results' colour stripes, Lock,
// Remove and Publish (Dataset.h).
//
// Why it works with almost no extra data: VST times each car between the two speed lines, and its speed is the zone's
// length over that time.  A calibration only changes the zone length, so while the camera and speed lines stay put a
// newer calibration re-times every past car by one factor, new zone feet / old zone feet.  Each run's manifest
// (run_<stem>.json) records the calibration it was measured with, and the csv's ZoneSeconds column the exact time, so
// the re-timed speed is exact: zone feet / ZoneSeconds.  Rows from before ZoneSeconds are rescaled from their whole-mph
// speed instead (within half a mph).  Runs from before manifests (VST 1.1) have no known calibration and are shown as
// measured, in a set of their own that can't be re-adjusted.
//
// A set is the runs whose manifests share one view: speed lines, street edge, level and analysis box.  Flow:
//
//   readRun() each csv's manifest  ->  sync() files new runs into sets, in time order:
//       a run whose view matches an open set joins it; otherwise it starts a new open set, and every other open set
//       locks ("the view moved"), keeping the calibration it last had;
//       the open set whose view is the current VST.cfg's follows the current calibration;
//       csvs that are gone (archived, deleted) leave their set, and an empty set goes
//   ->  shownScale() is the zone feet a set's rows are shown at  ->  retime() turns a row's measurement into that speed.
//
// Dragging the speed lines in Studio locks nothing by itself: only a run recorded from the new view does, so dragging
// back resumes the open set.  Locked sets keep their zone feet until the user accepts a newer calibration for them
// (accept()), which a published set never does: it gets a verdict instead (checkCalibration(), Dataset.h).
//
// Stored in <data>/Stats/calibration-sets.json.  No ImGui and no tracker: tests/calsets_test.cpp drives it.
#pragma once
#include <string>
#include <vector>

namespace calsets {

// Where the camera looks and what it times between: two runs with the same View can share one calibration.
struct View {
	int lineLeft = 0, lineRight = 0;       // speedLineLeft / speedLineRight, analysis-box pixels
	int edge[2] = { 0, 0 };                // streetEdge ([0,0] = level street)
	double level = 0;                      // levelDegrees
	int box[4] = { 0, 0, 0, 0 };           // AnalysisBoxLeft, Top, Width, Height; all 0 = not recorded (manifests before v1.10)
	bool hasBox() const { return box[2] > 0 && box[3] > 0; }
	// Same view.  A manifest without the analysis box matches on the rest, so older runs still join their set.
	bool same(const View& o) const;
};

// Zone feet for each direction: the street between the speed lines as a calibration measured it.
struct Scale {
	double l2r = 0, r2l = 0;
	bool valid() const { return l2r > 0 && r2l > 0; }
	double of(bool leftToRight) const { return leftToRight ? l2r : r2l; }
	bool same(const Scale& o) const;       // within 0.05%: re-expressing one calibration differently isn't a change
};

// Zone feet from VST.cfg's calibration numbers: frames a car at calSpeed took, at calFps (CalEvidence's zoneFtForFrames).
double zoneFeet(double calFrames, double calSpeed, double calFps);

// One stats csv and the manifest beside it.
struct RunInfo {
	std::string csv;                       // the file name, stats_<stem>.csv (sets refer to runs by name)
	std::string path;                      // full path
	bool known = false;                    // the manifest was read and carries the calibration
	View view;
	Scale scale;
	std::string started;                   // the manifest's "started", ISO 8601 ("" = unknown): the order runs are filed in
};
RunInfo readRun(const std::string& csvPath);

enum class State { Open, Locked };

struct Set {
	int id = 0;
	bool known = true;                     // false: the set of runs with no calibration on record (never re-adjusted)
	View view;
	State state = State::Open;
	Scale scale;                           // zone feet its speeds are shown at (an open set on the current view: see shownScale)
	Scale sigma;                           // relative 1-sigma of that calibration (CalEvidence), 0 = unknown
	std::string lockedAt;                  // ISO 8601, when it locked
	std::string lockReason;                // "view" (a run from a new view) or "manual"
	std::string first, last;               // started of its first and last run ("" = unknown)
	std::vector<std::string> runs;         // csv file names, in the order they were filed
	std::vector<std::string> published;    // Published/ folders made from it, oldest first
	bool locked() const { return state == State::Locked; }
	bool has(const std::string& csvName) const;
};

struct Store {
	std::vector<Set> sets;                 // oldest first
	int nextId = 1;
	Set* find(int id);
	const Set* find(int id) const;
	const Set* ofRun(const std::string& csvName) const;   // the set holding that csv (by file name), or null
};

// The file, and reading/writing it.  A missing or unreadable file is an empty store (sync() rebuilds it from the
// manifests; only lock choices and published folders would be lost).
std::string storePath(const std::string& statsDir);
Store load(const std::string& statsDir);
bool save(const std::string& statsDir, const Store& store, std::string& error);

// Files runs into sets as described at the top.  runs need not be sorted.  current = the view and calibration VST.cfg has
// now; currentSigma its uncertainty (0 = unknown).  now stamps a lock.  Returns true when the store changed.
bool sync(Store& store, std::vector<RunInfo> runs, const View& current, const Scale& currentScale, const Scale& currentSigma,
	const std::string& now);

// The zone feet a set's rows are shown at: today's calibration for the open set on today's view, else the set's own.
Scale shownScale(const Set& set, const View& current, const Scale& currentScale);

// A newer calibration exists for this locked set: same view as now, and today's zone feet differ from the set's.
bool newerCalibration(const Set& set, const View& current, const Scale& currentScale);

// Lock (keeping what it shows now), unlock (only a set on today's view, and only when no other open set has that view),
// accept today's calibration for a locked set on today's view.  Each returns false when it doesn't apply.
bool lock(Store& store, int id, const View& current, const Scale& currentScale, const Scale& currentSigma, const std::string& now);
bool canUnlock(const Store& store, int id, const View& current);
bool unlock(Store& store, int id, const View& current);
bool accept(Store& store, int id, const View& current, const Scale& currentScale, const Scale& currentSigma);

// One car re-timed to zoneFt.  zoneSeconds > 0: exact, zoneFt / seconds in mph.  Otherwise the measured speed scaled by
// zoneFt / thenFt.  Unknown scales (0) leave the measurement as it was.
struct Measured {
	int speed = 0;
	double zoneSeconds = 0;
	bool trend = false;                    // the trend fields are known
	int entryMph = 0, exitMph = 0, mphIn20ft = 0;
	double accel = 0;
};
struct Retimed {
	int speed = 0;
	double exact = 0;                      // unrounded mph
	int entryMph = 0, exitMph = 0, mphIn20ft = 0;
	double accel = 0;
	double factor = 1;                     // zoneFt / thenFt
	bool adjusted = false;                 // the shown speed differs from the measured one
};
Retimed retime(const Measured& m, double thenFt, double zoneFt);

// The plain words for a set, e.g. "Set 2 · Oct 22 to Oct 30 · 14 runs · zone 21.2 ft · locked".
std::string describe(const Set& set, const Scale& shown);
std::string shortDate(const std::string& iso);   // "2026-10-22T..." -> "Oct 22", "" -> ""

}  // namespace calsets
