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

namespace calib {

constexpr double kVerifiedMph = 2.0;   // a check within this many mph of the speedometer verifies the calibration

std::string historyPath();             // <configDir>/calibration-history.csv
std::string nowIso();                  // "2026-10-30T14:03:22-04:00"

// Appends one row, writing the header first when the file is new.  Returns false (with err) if it can't write.
bool append(const CalEvent& e, std::string& err);
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

// Calibration from a car of known length (the Calibrate page's "Known car" mode) is framesForDistance with the car's
// two bumpers as the marks and its overall length as the distance.  The catch is depth: on the picture, size goes
// as 1 / distance from the camera, so a car parked `fartherFt` beyond the middle of the lane traffic uses looks
// shorter than the same car driving there.  Its bumpers `markPx` apart stand for
//     markPx × (laneFt + fartherFt) / laneFt
// pixels at the lane's depth, where `laneFt` is the camera's distance to the middle of that lane (the site planner's
// "Window to the lane").  Uncorrected, a car parked 7 ft beyond a lane 30 ft away reads every speed ~23% high.
// fartherFt = 0 (a passing car, or one parked in the lane itself) or an unknown laneFt leaves markPx as it is.
inline double markPxAtLane(double markPx, double laneFt, double fartherFt) {
	if (markPx <= 0 || laneFt <= 0 || fartherFt <= 0) return markPx;
	return markPx * (laneFt + fartherFt) / laneFt;
}

// Common cars' overall (bumper to bumper) lengths for the Known car picker, from the makers' published specs and
// rounded to the inch.  Trims and model years within a generation differ by about an inch (0.5%); different
// generations can differ by several, which is why each entry names its years.
struct KnownCar { const char* name; double inches; };
inline constexpr KnownCar kKnownCars[] = {
	{ "Toyota Highlander 2020-on", 195 },
	{ "Toyota Highlander 2014-2019", 192 },
	{ "Toyota RAV4 2019-2025", 181 },
	{ "Toyota Camry 2018-2024", 192 },
	{ "Toyota Corolla sedan 2019-on", 182 },
	{ "Honda CR-V 2023-on", 185 },
	{ "Honda CR-V 2017-2022", 182 },
	{ "Honda Civic sedan 2022-on", 184 },
	{ "Honda Accord 2018-2022", 192 },
	{ "Subaru Outback 2020-2025", 191 },
	{ "Subaru Forester 2019-2024", 182 },
	{ "Tesla Model 3", 185 },
	{ "Tesla Model Y", 187 },
	{ "Ford F-150 SuperCrew, 5.5 ft bed, 2015-on", 232 },
};

// A direction's state from its history.  `currentFrames` is the CalibrationFrames<dir> now in VST.cfg and
// `calibratedOffRecord` says whether the setup counts as calibrated without a history row (a v1.1 calibration made
// before the history existed, or the bundled sample's VST.cfg).
CalState state(const std::vector<CalEvent>& rows, bool l2r, int currentFrames, bool calibratedOffRecord);

}  // namespace calib
