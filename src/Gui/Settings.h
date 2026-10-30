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
	// Site planner inputs (metres, degrees), so the guide remembers what was entered.
	double planHfov = 66, planWidth = 1920, planFps = 30, planShutter = 120;
	double planDist = 12, planClear = 18, planCalib = 30, planCarLen = 4.8;
	int planPreset = 0;
	// Setup progress for the side rail: the source whose zones were checked on the Zones page, and the config
	// a calibration was applied to.  A step counts as done only for the camera/config it was done for.
	std::string zonesFor, calibratedFor;

	void load();
	void save() const;
	void addRecent(const std::string& path);
	static std::string file();
	static std::string newToken();   // 24 random base-32 characters (120 bits)
};
