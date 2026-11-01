// Archived datasets: a run's results put away in one folder of their own, with everything needed to look at them
// again later, read-only, in Results ("Open" > the archive) or in any web browser (review.html inside the folder).
//
// An archive is a folder <data>/Archive/<YYYY-MM-DD>_<name>/ laid out like the data folder itself, so the street
// report can be made from it unchanged (report::Input::dataDir = the archive):
//
//   archive.json               what this is: name, when, the site, the limits it was judged by, the calibration the
//                              runs used, the original VST.cfg path (the calibration history is filtered on it), the
//                              files it holds, whether they were moved out of the data folder, and one entry per
//                              vehicle (the rows Results showed, with their snapshot and clip)
//   vehicles.csv               the same vehicles as a spreadsheet, for people who just want to open it in Excel
//   review.html                a self-contained viewer: table, filters, snapshots and clips, no install needed
//   VST.cfg                    the config the runs used, as it was when archived
//   calibration-history.csv    the calibration history, as it was
//   Stats/                     the stats csvs and their run_<stem>.json manifests
//   HiLites/                   each run's Hilites_<stem>.mp4, when one was made
//   Clips/                     each vehicle's short clip
//   Snapshots/                 each vehicle's snapshot, vehicle_<n>.jpg (n = its row number, from 1)
//
// Flow:  write() builds the folder under a ".partial" name and renames it into place only once every file is there,
// so a full disk or a crash never leaves a half archive that looks whole  ->  only then, when Request::move is set,
// the originals are removed from the data folder ("archive away": This session, Results' Open and the street report
// start fresh)  ->  read() loads archive.json back for the reviewer  ->  list() finds every archive, newest first  ->
// restore() puts the Stats and HiLites files back into the data folder (never over a file that is already there) and
// marks the archive as no longer moved.  The archive itself is kept: it still holds the snapshots and clips.
//
// No ImGui and no tracker: tests/archive_test.cpp drives it with fixture files.
#pragma once
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace archive {

// One vehicle as Results lists it.  snapshot and clip are paths relative to the archive folder ("" = none).
struct Vehicle {
	std::string passed;                   // "yyyy-mm-ddThh:mm:ss", "yyyymmdd hhmmss" (old csvs) or ""
	int speed = 0;
	bool l2r = true;
	std::string compass;
	bool wrongWay = false;
	int frames = 0, area = 0, frame = 0;
	bool large = false, flagged = false;
	double fps = 30;
	std::string source;                   // the video or camera's display name
	std::string snapshot, clip;
};

// The calibration and directions the runs used, for the report's runs that have no manifest of their own.
struct Calibration {
	std::string l2rCompass = "SE", r2lCompass = "NW";
	std::string legal = "Both";
	int framesL2R = 35, framesR2L = 40;
	double calSpeed = 25, calFps = 30;
	int minLoggedSpeed = 18;
};

struct Info {
	std::string dir;                      // the archive folder (filled in by read() and list())
	std::string name;                     // what the user called it, e.g. "First week, before the signs"
	std::string created;                  // ISO 8601, when it was archived
	std::string from;                     // what it was archived from: "This session" or the csv's file name
	std::string siteName;
	int speedLimit = 25, wayOver = 35;
	std::string vst = "dev";              // the VST version that archived it
	std::string configPath;               // the original VST.cfg path (calibration-history.csv rows are keyed on it)
	Calibration cal;
	bool moved = false;                   // the originals were taken out of the data folder (restore() can put them back)
	std::vector<std::string> files;       // every file in it, relative, '/'-separated (archive.json itself excluded)
	std::vector<Vehicle> vehicles;
	std::string firstPassed, lastPassed;  // the earliest and latest `passed`, "" when none are known
};

// One vehicle to archive: its row, plus where its picture and clip come from.
struct Source {
	Vehicle row;                          // snapshot and clip are ignored; write() fills them in
	cv::Mat snapshot;                     // BGR; empty = none (rows read back from a csv have none)
	std::string clipPath;                 // the clip in the data folder's Clips/; "" = none
};

struct Request {
	std::string dataDir;                  // the data folder: Archive/ goes under it, and moved files come from it
	Info info;                            // name, created, from, site, limits, vst, configPath and cal are used
	std::vector<std::string> statsCsvs;   // the runs' stats csvs; each one's manifest and highlights video go too
	std::string configFile;               // VST.cfg to copy ("" or missing = none)
	std::vector<std::string> historyFiles;// calibration-history.csv candidates; the first that exists is copied
	std::vector<Source> vehicles;
	bool move = true;                     // take the stats csvs, manifests, highlights and clips out of the data folder
};

// The archive folder name for a request: "<YYYY-MM-DD of created>_<slug of name>", "-2", "-3"... if taken.
std::string folderFor(const std::string& archiveRoot, const std::string& created, const std::string& name);

// Writes the archive.  dir gets the new folder; on failure nothing is left behind and nothing is moved.
// warnings lists what was missing but didn't stop it (a csv without a manifest is normal and not listed).
bool write(const Request& req, std::string& dir, std::string& error, std::vector<std::string>* warnings = nullptr);

// Reads an archive folder back (dir is the folder, or its archive.json).
bool read(const std::string& dir, Info& out, std::string& error);

// Every readable archive under <dataDir>/Archive, newest first.  Unreadable folders are skipped.
std::vector<Info> list(const std::string& dataDir);

// Puts a moved archive's Stats and HiLites files back into dataDir.  A file of the same name already there is kept and
// its archived copy left where it is (named in error, which is then a warning: the return value is still true).
bool restore(const std::string& dir, const std::string& dataDir, std::string& error);

// The run manifest and highlights video that go with a stats csv: stats_<stem>.csv -> run_<stem>.json beside it, and
// <data>/HiLites/Hilites_<stem>.mp4 (the tracker's naming, Tracker.cpp).
std::string manifestFor(const std::string& csv);
std::string highlightsFor(const std::string& csv);

// The self-contained browser viewer, as written to review.html.
std::string reviewHtml(const Info& info);

}  // namespace archive
