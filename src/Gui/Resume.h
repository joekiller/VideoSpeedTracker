// Picking up where the user left off when VST Studio reopens.
//
// Everything a novice set up lives on disk (vst-studio.ini, VST.cfg, calibration-history.csv, Data/Stats), but the
// window only shows it if launch reads it back.  Before this, reopening looked like a fresh install: a USB camera was
// never reconnected, so "Connect the camera" and "Mark your street" (whose check is tied to the source the zones were
// drawn on) lost their checks, and Results opened on "This session", empty because nothing had run yet this launch.
//
// The rules, as pure functions so tests/resume_test.cpp can pin them without a window:
//   reopenAtLaunch()   the remembered source comes back at launch whatever its kind: a file or photo that still exists,
//                      a stream, or a camera (USB index or Pi camera).  Cameras and streams connect in the background
//                      (Player), so a missing device never holds up the window; it only fails with a toast.
//   setupSource()      the source the setup steps are judged against: the connected one, or else the remembered one,
//                      so an unplugged camera (or one still connecting) doesn't uncheck steps 3 and 4.
//   sourceStepDone()   step 3: something is connected or queued, or a source has connected before and wasn't
//                      disconnected on purpose (App::disconnectSource forgets it).
//   zonesStepDone()    step 4: the zones were checked on the setup source and the config is valid.
//   savedRuns()        every run the tracker saved (<data>/Stats/stats_*.csv), newest first, for Results'
//                      "All saved runs" (what it opens on when this session has nothing yet).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace resume {

// isPath: the source names a file or photo on disk (not a camera index, Pi camera or stream URL); exists: it does.
bool reopenAtLaunch(const std::string& lastSource, bool isPath, bool exists);

// connected: the source currently open (or opening); remembered: AppPrefs::lastSource, set once a source has opened.
std::string setupSource(const std::string& connected, const std::string& remembered);

// connected: a source is open and no longer connecting; queued: a batch of videos is waiting.
bool sourceStepDone(bool connected, bool queued, const std::string& remembered);

// zonesFor: AppPrefs::zonesFor, the source Mark your street was last shown with valid zones; configOk: no problems.
bool zonesStepDone(const std::string& setupSource, const std::string& zonesFor, bool configOk);

// One run the tracker wrote: its stats csv and the run's name (the csv's stem without "stats_", e.g.
// "camera0_20261101_133012" or "20160130_172251" for a Foscam recording).
struct SavedRun {
	std::string csv;
	std::string name;
	std::int64_t modified = 0;   // the csv's last write, in file-clock ticks: only for ordering, never shown
};

// stats_*.csv files in <dataDir>/Stats, newest first (last written; ties by name, later first).  Other csvs that live
// there (health.csv, exports) are not runs and are left out.  Empty when the folder is missing.
std::vector<SavedRun> savedRuns(const std::string& dataDir);

// A run's name for people: "Camera 0 · 2026-11-01 13:30", "Pi camera 1 · …", "Stream · …", or the name itself for a
// video file's run.
std::string runLabel(const std::string& name);

}  // namespace resume
