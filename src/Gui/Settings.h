// VST.cfg as an editable document, and the GUI's own preferences.
//
// ConfigDoc keeps the file's lines so saving changes only the values: comments, ordering and keys the user
// wrote by hand survive a round trip through the GUI.  Values are parsed and validated by the engine's own
// Globals::readConfig()/problems(), so the GUI can never accept a file the tracker would reject.
#pragma once
#include "Globals.h"
#include <string>
#include <vector>

class ConfigDoc {
public:
	bool load(const std::string& path);      // false (and error set) if the tracker would refuse the file
	bool save();                              // writes to path(); creates the file if needed
	bool saveAs(const std::string& path);
	// Undo: the first save in a session keeps the file as it was in <file>.bak; restoreBackup() puts it back.
	bool hasBackup() const;
	bool restoreBackup();
	std::string backupPath() const;
	// Writes the current values to a scratch file for one tracking run, leaving the user's file untouched.
	std::string writeRunCopy(const std::string& dir) const;
	const std::string& path() const { return path_; }
	Globals g;
	std::vector<std::string> problems() { return g.problems(); }
	bool dirty() const;
	void markClean();
	std::string error;
	std::vector<std::string> log;             // what readConfig printed while loading
private:
	std::string render() const;
	std::string path_;
	std::vector<std::string> lines_;
	std::string cleanSnapshot_;
	std::string backedUp_;                    // path whose original is in backupPath() this session
};

// A known key's current value in VST.cfg syntax, e.g. "obstruction" -> "[251,311]".
std::string configValue(const Globals& g, const std::string& key);
const std::vector<std::string>& configKeys();

// Preferences that aren't part of VST.cfg, stored as key = value in <configDir>/vst-studio.ini.
struct AppPrefs {
	std::string configPath;
	std::string lastSource;
	std::vector<std::string> recent;
	std::string siteName = "My street";
	int speedLimit = 25;
	int egregious = 35;
	bool realtime = true;
	bool highlights = true;
	int hlMin = 35, hlMax = 100, hlArea = 100;
	bool trace = false;
	bool showMotion = false;
	bool guideDone = false;
	bool remoteEnabled = false;
	int remotePort = 8787;
	std::string remoteToken;
	std::string remoteAddress;
	int keepVideoGB = 20;        // disk budget for Clips/ + HiLites/, oldest removed first; 0 = keep everything   // the address put in the pairing QR code; "" = automatic (lanAddress())
	// Site planner inputs (metres, degrees), so the guide remembers what was entered.
	double planHfov = 66, planWidth = 1920, planFps = 30, planShutter = 120;
	// The street starts as a rough guess (the "Two-lane street from the house" preset, 35 ft / 50 ft) and is refined
	// later; planSource says where planDist and planClear came from now (plan::Source, Plan.h), planNote says how in
	// words ("6 car lengths across the picture", "Drive-through calibration"), and planCalSig is the calibration
	// the numbers were last measured from, so a new calibration updates them once and a typed edit afterwards sticks.
	double planDist = 35 / 3.28084, planClear = 50 / 3.28084, planCalib = 30, planCarLen = 4.8;
	int planPreset = 0;
	int planSource = 0;
	std::string planNote, planCalSig;
	double planCarLengths = 0;   // the last "car lengths across the picture" estimate (0 = none yet)
	// Pick your spot's map ("Find it on the map", companion/js/views/map.js): where the camera sits (0, 0 = not placed;
	// the lane's spot is siteLat/siteLon below) and the street's name there.
	double planCamLat = 0, planCamLon = 0;
	std::string planStreet;
	// Pick your spot edits a draft and saves it only when asked (App::planCommit); planAutosave saves every edit as it
	// is made instead.  planDraft keeps an unsaved draft across a restart (PlanState::serialize, "" = none).
	bool planAutosave = false;
	std::string planDraft;
	// Mark your street's "Work it out from the camera" (Perspective.h CameraSetup): the camera's height above the road
	// and how far it is turned from square to the street (+ = toward the right).  Distance and lens come from step 2.
	double calcHeightFt = 12, calcYawDeg = 0;
	// Mark your street's parking lane (ParkingLane.h): rows of a line on the parked cars' wheel centres at frame x 0 and
	// 1280 (0, 0 = no parking lane), and how far beyond the moving cars it is if measured (ft; 0 = from the picture).
	double parkY0 = 0, parkY1 = 0, parkFt = 0;
	// The camera zone's spot on the map (degrees; 0, 0 = not set), for matching Tesla dashcam clips by GPS.
	double siteLat = 0, siteLon = 0;
	// Setup progress for the side rail: the source whose zones were checked on the Zones page, and the config
	// a calibration was applied to.  A step counts as done only for the camera/config it was done for.
	std::string zonesFor, calibratedFor;
	std::string calibratedLines;   // speedLineLeft,speedLineRight,AnalysisBoxWidth when calibrated (App::speedLinesSig)
	bool planConfirmed = false;    // Pick your spot has been saved at least once: step 2's check in the rail

	void load();
	void save() const;
	std::string render() const;   // the file's contents as save() would write them
	void addRecent(const std::string& path);
	static std::string file();
	static std::string newToken();   // 24 random base-32 characters (120 bits)
};

// Everything Pick your spot (step 2) edits, as one value: the planner's street and camera, the speed limit, and what
// the map placed.  The page keeps a draft of it apart from the saved copy in AppPrefs, so a change is only a preview
// until it is saved; comparing the two says whether anything is unsaved.
struct PlanState {
	double dist = 35 / 3.28084, clear = 50 / 3.28084;          // metres: camera to the moving lane; clear stretch of it
	double hfov = 66, width = 1920, fps = 30, shutter = 120;   // the camera (shutter is 1/x s)
	double carLen = 4.8, carLengths = 0;
	int preset = 0, source = 0, speedLimit = 25;
	std::string note;
	double camLat = 0, camLon = 0, laneLat = 0, laneLon = 0;   // the map's two pins (0, 0 = not placed)
	std::string street;

	static PlanState of(const AppPrefs& p);
	void store(AppPrefs& p) const;
	// Equal to within rounding: the ini keeps six significant digits, so a saved value read back differs in the 7th.
	bool same(const PlanState& o) const;
	// One line for vst-studio.ini (AppPrefs::planDraft): key=value pairs separated by '|'.  parse() leaves fields the
	// line doesn't name as they are, and returns false for an empty or unreadable line.
	std::string serialize() const;
	bool parse(const std::string& line);
};
