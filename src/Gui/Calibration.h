// Calibration evidence: the history file and what it says about each direction.
//
// Every calibration VST Studio applies, and every check pass a user drives to test one, is appended as a row to
// <configDir>/calibration-history.csv (the shared v1.2 format; see docs/vst-studio.md "Calibrate"):
//
//   Timestamp, Config, Kind, Direction, DrivenMph, Frames, Fps, CalibrationFrames, MeasuredMph, Error
//
// The file is never rewritten, so it is a record a traffic engineer can be shown: when each calibration was made,
// from which pass, and how far a second, independent pass disagreed with it.  One file serves every VST.cfg on the
// machine; the Config column says which one a row belongs to.
//
// The per-direction state (Not calibrated / Calibrated, not checked / Verified / Check failed) is computed from
// those rows rather than kept anywhere else, so it survives restarts and can't drift from the record.
#pragma once
#include "CalEvidence.h"
#include <algorithm>
#include <string>
#include <vector>

struct CalEvent {
	std::string time;            // ISO 8601 local with the UTC offset
	std::string config;          // the VST.cfg the event belongs to
	bool check = false;          // false: "apply" (set the calibration), true: "check" (a verification pass)
	bool l2r = true;
	double drivenMph = 0;        // what the driver's speedometer / cruise control said
	int frames = 0;              // frames the pass took between the speed lines
	double fps = 30;             // at this frame rate
	int calFrames = 0;           // CalibrationFrames<dir> in force after the event
	double measuredMph = 0;      // check only: what the calibration in force says the pass was
	double error = 0;            // check only: measuredMph - drivenMph
};

// One direction's standing, for the Calibrate page and GET /api/status.
struct CalState {
	enum Kind { None, Unchecked, Verified, Failed } kind = None;
	double error = 0;            // the deciding check's error (Verified / Failed)
	std::string applied;         // time of the latest apply, "" if none on record
	std::string checked;         // time of the deciding check, "" if none
	const char* apiName() const { return kind == None ? "none" : kind == Unchecked ? "unchecked" : kind == Verified ? "verified" : "failed"; }
};

// CalPass (the user's own calibration and check drives, calibration-passes.csv) is in CalEvidence.h, with the
// reading and writing of that file, so archives (Archive.cpp) can carry and restore it too.


namespace calib {

constexpr double kVerifiedMph = 2.0;   // a check within this many mph of the speedometer verifies the calibration

std::string historyPath();             // <configDir>/calibration-history.csv
std::string nowIso();                  // "2026-10-30T14:03:22-04:00"

// Appends one row, writing the header first when the file is new.  Returns false (with err) if it can't write.
bool append(const CalEvent& e, std::string& err);

std::string passesPath();              // <configDir>/calibration-passes.csv
bool appendPass(const CalPass& p, std::string& err);
std::vector<CalPass> loadPasses();     // every row, oldest first; unreadable lines skipped
// The rows for one VST.cfg (paths compared after canonicalising), oldest first.  Lines it can't parse are skipped.
std::vector<CalEvent> load(const std::string& configPath);

// The CalibrationFrames a pass implies: a pass at `mph` taking `frames` at `fps` reads `mph` when
// CalibrationFrames = frames × mph / CalibrationSpeed × CalibrationFPS / fps (inverse of the engine's formula).
double impliedFrames(double mph, int frames, double fps, double calSpeed, double calFps);
// What a calibration of `calFrames` says a pass of `frames` at `fps` was, unrounded (the engine rounds to whole mph):
// speed = CalibrationSpeed × CalibrationFrames × (fps / CalibrationFPS) / frames.
double measuredMph(int calFrames, int frames, double fps, double calSpeed, double calFps);

// Calibration from a measured distance instead of a pass (the Calibrate page's "Cones or pole" mode): two marks
// `markPx` apart on the picture (1280-wide grid, the same as the speed lines) are `markFt` apart on the street, so the
// zone between the speed lines, `zonePx` wide, is zonePx / markPx × markFt feet, and a car at CalibrationSpeed crosses
// it in that many feet ÷ (CalibrationSpeed in ft/s) seconds, × CalibrationFPS frames.  Unrounded; 0 when the marks
// or the distance can't give an answer.  Only as good as the marks' depth: they must be where the cars' near side runs.
inline double framesForDistance(double markPx, double markFt, double zonePx, double calSpeed, double calFps) {
	if (markPx <= 0 || markFt <= 0 || zonePx <= 0 || calSpeed <= 0 || calFps <= 0) return 0;
	double zoneFt = zonePx / markPx * markFt;
	return zoneFt / (calSpeed * 5280.0 / 3600.0) * calFps;
}

// Calibration from a car of a known model (the Calibrate page's "Known car" mode) is framesForDistance with the car's
// two wheel centres (or bumpers) as the marks and its wheelbase (or overall length) as the distance.  The catch is depth: on the picture, size goes
// as 1 / distance from the camera, so a car parked `fartherFt` beyond the middle of the lane traffic uses looks
// shorter than the same car driving there.  Its bumpers `markPx` apart stand for
//     markPx × (laneFt + fartherFt) / laneFt
// pixels at the lane's depth, where `laneFt` is the camera's distance to the middle of that lane (the site planner's
// "Window to the lane").  Uncorrected, a car parked 7 ft beyond a lane 30 ft away reads every speed ~23% high.
// fartherFt = 0 (a passing car, or one parked in the lane itself) or an unknown laneFt leaves markPx as it is.
// This plain ratio is right for a camera at the cars' height.  From a window above the street the picture shrinks less
// than that with depth, so Studio scales with the camera model instead (ParkingLane.h scaleBeyond, Perspective.h
// laneScale) and keeps this only as the fallback when the camera model can't place the car.
inline double markPxAtLane(double markPx, double laneFt, double fartherFt) {
	if (markPx <= 0 || laneFt <= 0 || fartherFt <= 0) return markPx;
	return markPx * (laneFt + fartherFt) / laneFt;
}

// Common cars' sizes for the Known car picker, from the makers' published specs and rounded to the inch: overall length
// (bumper to bumper) and wheelbase (front wheel centre to rear wheel centre).  Trims and model years within a generation
// differ by about an inch (0.5%); different generations can differ by several, which is why each entry names its years.
//
// The wheelbase is what the page asks for first.  Wheel centres are two sharp points on the side facing the camera, at the
// same height (the lane line's, hubcap height), so where to click is never in doubt: a car at a slight angle to the curb
// or the camera, rounded bumper corners, a tow hitch or bike rack change nothing.  A bumper's "corner facing the camera"
// is a guess on a rounded car, and off to one side the camera sees some of the front or back as well, which reads as
// extra length.  The wheelbase is shorter (about 60% of the length), so a click a pixel off weighs more, which is why the
// page wants the car big in the picture.
struct KnownCar { const char* name; double inches; double wheelbaseIn; };
inline constexpr KnownCar kKnownCars[] = {
	{ "Toyota Highlander 2020-on", 195, 112 },
	{ "Toyota Highlander 2014-2019", 192, 110 },
	{ "Toyota RAV4 2019-2025", 181, 106 },
	{ "Toyota Camry 2018-2024", 192, 111 },
	{ "Toyota Corolla sedan 2019-on", 182, 106 },
	{ "Honda CR-V 2023-on", 185, 106 },
	{ "Honda CR-V 2017-2022", 182, 105 },
	{ "Honda Civic sedan 2022-on", 184, 108 },
	{ "Honda Accord 2018-2022", 192, 111 },
	{ "Subaru Outback 2020-2025", 191, 108 },
	{ "Subaru Forester 2019-2024", 182, 105 },
	{ "Tesla Model 3", 185, 113 },
	{ "Tesla Model Y", 187, 114 },
	{ "Ford F-150 SuperCrew, 5.5 ft bed, 2015-on", 232, 145 },
};
// A typical car's wheelbase, for "Other" before anything is typed (a mid-size sedan's, about 9 ft 3 in).
constexpr double kTypicalWheelbaseFt = 111.0 / 12;

// A direction's state from its history.  `currentFrames` is the CalibrationFrames<dir> now in VST.cfg and
// `calibratedOffRecord` says whether the setup counts as calibrated without a history row (a v1.1 calibration made
// before the history existed, or the bundled sample's VST.cfg).
CalState state(const std::vector<CalEvent>& rows, bool l2r, int currentFrames, bool calibratedOffRecord);

// The drive-by's target speed: what the Calibrate page (and the phone's) suggests driving at.  Any steady speed
// calibrates equally well, since the engine scales every pass by speed × frames, and a slower pass is counted in more
// frames, so its own one-frame uncertainty is smaller.  (The stored CalibrationFrames is still a whole number at
// CalibrationSpeed, whatever was driven.)  The default is the posted limit, the speed a neighbour can
// drive past without being the speeder; the presets cover quiet streets (10-20) and the old 25.
// The pass list includes passes timed under minLoggedSpeed (Tracker::passesSince), so 10 or 15 still shows up there.
inline constexpr int kTargetPresets[] = { 10, 15, 20, 25 };
inline constexpr int kTargetMin = 5, kTargetMax = 80;   // the "I drove at" stepper's range
// The chips to offer: the presets plus the posted limit when it's none of them, in order.
inline std::vector<int> targetPresets(int speedLimit) {
	std::vector<int> out(std::begin(kTargetPresets), std::end(kTargetPresets));
	if (speedLimit >= kTargetMin && speedLimit <= kTargetMax && std::find(out.begin(), out.end(), speedLimit) == out.end()) {
		out.push_back(speedLimit);
		std::sort(out.begin(), out.end());
	}
	return out;
}
// The target to start from: the one the user picked last time (`saved`, 0 for never), else the posted limit.
inline int defaultTarget(int saved, int speedLimit) {
	return std::clamp(saved > 0 ? saved : speedLimit > 0 ? speedLimit : 25, kTargetMin, kTargetMax);
}
// A speed for the check drive, different from the calibration's so the check also shows speeds scale the way the
// engine assumes: 5 mph faster, or 5 slower from 20 up so it stays near a quiet street's limit.
inline int checkTarget(int calibratedAt) {
	return std::clamp(calibratedAt >= 20 ? calibratedAt - 5 : calibratedAt + 5, kTargetMin, kTargetMax);
}
// The engine's minLoggedSpeed for a posted limit: 5 mph under it, never above the engine default of 18 nor below 5.
// 18 was chosen for 25 mph streets (walkers and cyclists out, traffic in); on a 15 mph street it also drops legal
// traffic at 12-17 mph, which skews the share over the limit.  So 15 -> 10, 20 -> 15, 25 and up -> 18.
inline constexpr int kDefaultMinLogged = 18;
inline int minLoggedForLimit(int speedLimit) { return std::clamp(speedLimit - 5, 5, kDefaultMinLogged); }
// Whether a minLoggedSpeed is still the automatic one (the default, or what minLoggedForLimit gave for `speedLimit`),
// so a new limit may move it.  A value the user typed in Run settings or VST.cfg stays as it is.
inline bool minLoggedFollows(int minLogged, int speedLimit) {
	return minLogged == kDefaultMinLogged || minLogged == minLoggedForLimit(speedLimit);
}

// Many cars' cruise control won't engage below about 20-25 mph, so a slower target needs a steady foot, or a speed
// read afterwards from VST Ride-along or a Tesla dashcam clip.
inline bool belowCruiseControl(int mph) { return mph < 25; }

}  // namespace calib
