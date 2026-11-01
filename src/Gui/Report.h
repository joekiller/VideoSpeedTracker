// The street report: one self-contained HTML file a neighbor can print to PDF and hand to a city council and its
// traffic engineer ("Community traffic study.  Estimated speeds, ±X mph").
//
// Flow:  loadStudy() reads every stats csv in <data>/Stats, the run manifest next to each one (run_<stem>.json, VST 1.2+)
// and the calibration history for the config  ->  analyze() keeps the vehicles inside the date range and works out
// coverage, volumes, speed statistics, charts' data, wrong-way shares, the calibration record and the error band
//  ->  renderHtml() lays it out as a printable document (inline CSS and SVG, images as data URIs, nothing external)
//  ->  writeReport() does all three and saves <data>/Reports/report_<site>_<from>_<to>.html.
//
// It knows nothing about ImGui or the tracker, so tests/report_test.cpp can drive it with fixture files.  Old data
// degrades gracefully: a csv without a manifest gets coverage from its first and last vehicle, a 13-column csv gets
// its times from Date/Time (+ Frame/fps for Foscam recordings), a missing history means "not verified", and the
// report says which of these happened.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cv { class Mat; }

namespace report {

// Nearest-rank percentile of an ascending list: the value at rank ceil(p/100 * n), 1-based (0 for an empty list).
// The one definition used everywhere (Studio's tiles, the phone's Stats view and the report's method appendix).
int percentile(const std::vector<int>& sorted, double p);

// What the user chose in the "Make report" dialog (or the API's query string).
struct Settings {
	std::string siteName = "My street";   // coarse location typed by the user: street and block, never a house number
	int speedLimit = 25;                  // posted limit, mph
	int wayOver = 35;                     // the "way over" threshold, mph
	std::string from, to;                 // "YYYY-MM-DD" local dates, inclusive; empty = from the first / to the last data
	std::string note;                     // optional free text for the cover
	bool includeSnapshots = false;        // example frames (blurred) only when the user ticks the box
	std::string compareFrom, compareTo;   // optional second period for a before/after table; both empty = none
	std::string generated;                // "YYYY-MM-DD" shown as the generated date; empty = today
	std::string version = "dev";          // VST version that made the report
};

// The calibration and direction settings a run used: from its manifest, or the current VST.cfg for older runs.
struct RunConfig {
	std::string l2rCompass = "SE", r2lCompass = "NW";
	std::string legal = "Both";           // "L2R", "R2L" or "Both" (a compass label is resolved by resolveLegal())
	int framesL2R = 35, framesR2L = 40;   // CalibrationFramesL2R / R2L
	double calSpeed = 25, calFps = 30;    // CalibrationSpeed / CalibrationFPS
	int minLoggedSpeed = 18;
	std::string resolveLegal() const;     // "L2R", "R2L" or "Both"
};

// An example frame the caller has already made safe (see blurForReport) and encoded as JPEG.
struct Snapshot {
	std::string jpeg;
	int speed = 0;
	bool l2r = true;
	std::string when;                     // "yyyy-mm-ddThh:mm:ss" or ""
};

struct Input {
	std::string dataDir;                  // folder holding Stats/ (and where Reports/ is written)
	std::string configPath;               // the VST.cfg the history is filtered on
	std::vector<std::string> historyFiles;// candidate calibration-history.csv paths, first existing ones are read
	RunConfig current;                    // the current VST.cfg, for runs without a manifest
	std::vector<Snapshot> snapshots;      // used only when Settings::includeSnapshots
	std::vector<std::string> csvFiles;    // explicit csvs; empty = every *.csv in <dataDir>/Stats
	// Calibration and check drives (passKey of each, from calib::loadPasses): the person calibrating isn't traffic, so
	// these rows are left out of every statistic and counted in Run::calibrationDrives.
	std::set<std::string> calibrationPasses;
};

// How a calibration pass (CalPass) and a stats row are matched: the stats csv's file name and the row's Frame.  A csv
// holds one run of one source, so no two of its rows share a frame.
inline std::string passKey(const std::string& statsFile, int frame) { return statsFile + "|" + std::to_string(frame); }

// ---- what loadStudy() reads -------------------------------------------------------------------------------

constexpr int64_t kNoTime = INT64_MIN;

// One logged vehicle.  Times are "local seconds": the wall-clock fields read as if UTC, so day and hour buckets are the
// site's local day and hour.
struct Obs {
	int64_t t = kNoTime;
	bool l2r = true;
	int speed = 0;
	bool wrongWay = false;
	bool flagged = false;                 // "*****": implausible, excluded from the statistics
	int run = 0;                          // index into Study::runs
};

struct Interval { int64_t from = 0, to = 0; };

struct Run {
	std::string csvPath, name;
	bool manifest = false;                // run_<stem>.json was found and read
	std::string vst;                      // VST version from the manifest
	std::string sourceKind;               // file | camera | stream
	bool complete = true;
	bool timed = false;                   // started and ended both known
	Interval span;                        // started..ended (local seconds), when timed
	std::vector<Interval> outages;        // local seconds
	double fps = 0;                       // manifest fps (0 = unknown)
	RunConfig cfg;
	bool cfgFromManifest = false;
	int logged = -1, belowMin = -1, untimed = -1;   // manifest counts (-1 = no manifest)
	int columns = 15;                     // 13 for csvs from before Flag and Timestamp
	bool timesDerived = false;            // times rebuilt from Date/Time (and Frame/fps)
	int undated = 0;                      // rows with no usable time at all
	int skipped = 0;                      // malformed rows
	int calibrationDrives = 0;            // rows left out as the user's own calibration or check drives (Input::calibrationPasses)
	std::string problem;                  // manifest present but unreadable, etc.
};

// One calibration-history.csv row for this config.
struct CalEvent {
	std::string when;                     // as written (ISO 8601)
	std::string kind;                     // apply | check
	std::string dir;                      // L2R | R2L
	double driven = 0;
	int frames = 0;
	double fps = 0;
	int calFrames = 0;
	bool hasMeasured = false;
	double measured = 0, error = 0;
};

struct SpeedStats {
	int n = 0;
	double mean = 0;
	int median = 0, p85 = 0, max = 0;
	int over = 0, over10 = 0, wayOver = 0;
	double pctOver = 0, pct10 = 0;        // percent of n
};

// The headline numbers of one period, for the before/after table.
struct PeriodSummary {
	std::string from, to;
	double hours = 0;
	SpeedStats all;
	int wrongWay = 0;
	double perHour = 0;                   // vehicles per observed hour
};

struct Study {
	Settings settings;
	std::vector<Run> runs;
	std::string from, to;                 // effective range "YYYY-MM-DD" (empty when nothing is dated)
	int64_t fromDay = 0, toDay = -1;      // days since 1970-01-01, inclusive

	// Vehicles in range with a usable speed, flagged rows already removed.
	std::vector<Obs> obs;
	int flaggedExcluded = 0, undatedExcluded = 0, undatedIncluded = 0;

	// Coverage: merged observed intervals clipped to the range, and seconds observed per day and hour of day.
	std::vector<Interval> covered;
	double hours = 0;
	bool coverageEstimated = false;       // some runs had no manifest: first-to-last vehicle was used
	std::map<int64_t, std::array<double, 24>> dayHourSecs;
	std::array<double, 24> hourSecs{};
	std::vector<Interval> gaps;           // stretches inside the range with no observation (>= 1 hour)

	// Volumes.
	int manifestRuns = 0, plainRuns = 0;
	int mLogged = 0, mBelow = 0, mUntimed = 0;     // summed over runs with a manifest (whole runs)
	int minLoggedSpeed = 18;

	SpeedStats all, l2r, r2l;
	std::string l2rCompass = "SE", r2lCompass = "NW";
	std::array<int, 24> hourN{};
	std::array<int, 24> hourP85{};         // -1 where fewer than kMinCell vehicles
	std::array<std::array<int, 24>, 7> wdN{}, wdOver{};   // weekday (0 = Sunday) x hour
	int weekdays = 0;                     // distinct weekdays in the data
	std::vector<std::pair<int, int>> histogram;   // (mph, vehicles), every mph from the lowest to the highest

	std::string legal = "Both";           // L2R | R2L | Both
	int wrongTotal = 0;
	std::array<int, 24> wrongByHour{};

	bool historyFound = false;            // a calibration-history.csv was read
	int historyOtherConfigs = 0;          // its rows for other configs
	std::vector<CalEvent> calEvents;      // this config's, oldest first
	bool verified[2] = { false, false };  // [L2R, R2L]
	bool hasCheck[2] = { false, false };
	double checkError[2] = { 0, 0 };      // latest check after the latest apply
	int checkCalFrames[2] = { 0, 0 };     // the CalibrationFrames that check tested
	// The runs used a different CalibrationFrames than the one checked (a hand edit of VST.cfg writes no history row):
	// that direction is "not checked", and the check's error isn't counted in the band.
	bool checkMismatch[2] = { false, false };
	int usedCalFrames[2] = { 0, 0 };      // CalibrationFrames the runs used (the first that differs, on a mismatch)

	double framesAtLimit[2] = { 0, 0 };   // frames a car at the limit spends between the lines
	double quantMph = 0;                  // one-frame error at the limit, worst direction
	double calMph = 0;                    // |check error| used, worst direction
	double band = 0;                      // ± mph stated on the cover

	bool hasCompare = false;
	PeriodSummary main, compare;
	std::vector<std::string> caveats;     // plain-language list of what's missing or estimated
	std::vector<Snapshot> snapshots;
};

constexpr int kMinCell = 5;               // fewer vehicles than this and an hour's 85th percentile or % is not shown

// Reads the csvs, manifests and history.  Never fails outright: what can't be read becomes a caveat.
std::vector<Run> loadRuns(const Input& in, std::vector<Obs>& obs);
// The settings a stats csv was measured with, from its run manifest (run_<stem>.json beside it).  False, with `out`
// untouched, when it has no readable manifest (runs before VST 1.2): what they were measured with isn't known.
bool runConfigOf(const std::string& csvPath, RunConfig& out);
// The whole pipeline up to the numbers.
Study analyze(const Input& in, const Settings& s);
std::string renderHtml(const Study& st);
// analyze + renderHtml + save.  path gets the file written; returns false with error set if it couldn't be written.
bool writeReport(const Input& in, const Settings& s, std::string& path, std::string& html, std::string& error);

// "Locust Ave, 600 block" -> "locust-ave-600-block".
std::string slug(const std::string& s);
// Parses "2026-10-30T14:03:22-04:00" (offset optional, 'T' or ' '): local seconds and the UTC offset in seconds.
bool parseIso(const std::string& s, int64_t& local, int& offset, bool& hasOffset);
// "YYYY-MM-DD" -> days since 1970-01-01.
bool parseDay(const std::string& s, int64_t& day);
std::string dayString(int64_t day);

// Privacy for example frames: blurs the lower part of the vehicle (plate height) and shrinks the frame so no face or
// plate is legible, then JPEG-encodes it.  Returns "" if encoding fails.
std::string blurForReport(const cv::Mat& bgr);

}  // namespace report
