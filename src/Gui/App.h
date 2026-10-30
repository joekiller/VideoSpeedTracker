// VST Studio application state and pages.
//
// One App lives for the life of the window.  Each GUI frame, frame() draws the side rail, the header and the
// current page, then drains work queued by the companion API (Server runs on its own threads and hands anything
// that touches App state to the GUI thread through post()).
//
// Pages follow what a first-time user has to do, in order:
//   Get started (Guide.cpp)  equipment, picking a spot with the site planner, aiming, then a checklist
//   Source                   video files (batch), a USB camera, or a network stream
//   Zones                    drag the analysis box, speed lines, lanes and obstruction on the picture
//   Calibrate                drive-through calibration or counting frames by hand
//   Live                     tracking with overlays, live counts and a feed of logged vehicles
//   Results                  speeders and wrong-way drivers, charts, snapshots, csv and highlights
//   Companion                pairing a phone with the local API (QR code)
#pragma once
#include "Calibration.h"
#include "Media.h"
#include "Report.h"
#include "Results.h"
#include "Settings.h"
#include "Tracker.h"
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Server;

enum class Page { Guide, Source, Zones, Calibrate, Track, Results, Companion, Count };

// The outcome of a check pass (App::checkCalibration), kept so the Calibrate page can show it and offer to average.
struct CalCheck {
	bool valid = false;
	bool l2r = true;
	double drivenMph = 0;       // what the user says they drove
	double measuredMph = 0;     // what the calibration in force says that pass was, to 0.1 mph
	double error = 0;           // measuredMph - drivenMph, to 0.1 mph
	bool verified = false;      // |error| <= calib::kVerifiedMph
	int frames = 0;
	double fps = 30;
	int calFrames = 0;          // CalibrationFrames<dir> the check was made against
};

struct ResultRow {
	int vehicleId = 0;          // session vehicle (has a snapshot), 0 for rows read from a csv
	std::string source;
	int frame = 0;
	bool l2r = true;
	std::string compass;
	bool wrongWay = false;
	int speed = 0;
	int frames = 0;
	int area = 0;
	bool large = false;
	bool flagged = false;
	double fps = 30;
	std::string time;
};

class App {
public:
	App();
	~App();
	void init(bool fresh);
	void frame(float width, float height);
	bool wantsContinuousFrames();
	void shutdown();

	// --- shared actions (also used by the companion API and the screenshot script) ---
	bool openSource(const std::string& path);
	void addToBatch(const std::vector<std::string>& paths);
	void startTracking();
	void stopTracking();
	bool loadConfig(const std::string& path);
	bool saveConfig();
	// Setup progress, shared by the side rail and the header's "next step" button.
	bool zonesDone();
	bool calibrationDone();
	std::string dataDir();
	std::string findSample();
	// Sets the calibration for one direction from a pass at a known speed; keeps the other direction's
	// calibration valid by expressing both against the same CalibrationSpeed/CalibrationFPS.
	// Every apply is also appended to calibration-history.csv (Calibration.h).
	void applyCalibration(bool l2r, double mph, int frames, double fps);
	// A check pass: what the calibration in force says a pass at a known speed was, without changing anything.
	// Appends a "check" row to the history and returns the comparison.
	CalCheck checkCalibration(bool l2r, double mph, int frames, double fps);
	// Replaces the direction's calibration with the mean of what the first pass (the CalibrationFrames the check
	// was made against) and the check pass imply; recorded as an apply.
	void averageCalibration(const CalCheck& check);
	// Not calibrated / not checked / verified / check failed, from the history rows for the current VST.cfg.
	CalState calibrationState(bool l2r);
	void loadResultsCsv(const std::string& path);
	void showSessionResults();
	// The street report (Report.h): what it reads (data folder, config, calibration history, and blurred snapshots of
	// this session's fastest vehicles when asked) and the user's choices.  GUI thread only; the API copies them and
	// builds the report on its own thread.
	report::Input reportInput(bool snapshots, const std::string& from, const std::string& to);
	report::Settings reportSettings();
	void toast(const std::string& text, unsigned color, const char* icon = nullptr);
	void setPage(Page p) { page = p; }
	void setRemote(bool on);

	// Runs fn on the GUI thread at the start of the next frame and waits (up to 5 s) for its result.
	std::string post(std::function<std::string()> fn);

	Page page = Page::Guide;
	AppPrefs prefs;
	ConfigDoc cfg;
	Player player;
	Tracker tracker;
	std::vector<std::string> batch;     // queued inputs for the next run; empty = the current source
	std::string source;                 // current source path / index / URL
	std::vector<ResultRow> rows;
	std::string rowsFrom;               // "" = this session, else the csv path
	int calibMarks[2][2] = { { -1, -1 }, { -1, -1 } };   // [l2r?0:1][start/end] frame numbers marked by hand
	std::unique_ptr<Server> server;
	bool screenshotMode = false;        // documentation captures (main.cpp --shots)
	bool persist = true;                // save preferences on exit

private:
	friend struct DemoScript;
	void drawRail(float h);
	void drawHeader(float w);
	void drawToasts(float w, float h);
	void pageGuide();
	void pageSource();
	void pageZones();
	void pageCalibrate();
	void pageTrack();
	void pageResults();
	void pageCompanion();
	void zoneInspector();
	void runOptions();
	void vehicleCard(const Vehicle& v, float width);
	void resultsCharts(const std::vector<const ResultRow*>& shown, float width);
	void reportDialog();
	Texture* snapshotTexture(int vehicleId);
	void pumpTracker();
	void sitePlanner();
	void autosave();
	void recordCalibration(CalEvent e);
	const std::vector<CalEvent>& calibrationHistory();
	void calibrationCheckCard(float width);

	struct Toast { std::string text; unsigned color; const char* icon; double until; };
	std::deque<Toast> toasts_;
	std::mutex postMu_;
	std::deque<std::pair<std::function<std::string()>, std::shared_ptr<std::promise<std::string>>>> posted_;
	Tracker::Live live_;
	Texture liveTex_, motionTex_;
	std::map<int, std::unique_ptr<Texture>> snapTex_;
	int seenVehicleId_ = 0;
	bool wasRunning_ = false;
	int guideStep_ = 0;
	int resultsFilter_ = 0;
	int selectedRow_ = -1;
	int calibMode_ = 0;
	int calibDir_ = 0;
	int calibPickId_ = 0;
	int calibSpeed_ = 25;
	bool calibChecking_ = false;        // the Calibrate page's pass picker is choosing a check pass, not a calibration
	int calibAppliedId_ = 0;            // the session vehicle the latest calibration came from (not a fair check)
	CalCheck lastCheck_;                // the latest check pass, for the result card
	std::vector<CalEvent> calHistory_;  // calibration-history.csv rows for calHistoryFor_
	std::string calHistoryFor_;
	bool calHistoryLoaded_ = false;
	double autosaveAt_ = 0;             // when the pending autosave fires (0 = nothing pending)
	double savedAt_ = 0;                // last successful autosave, for the rail's "saved" note
	std::string sampleCache_;
	char urlBuf_[512] = "rtsp://";
	int cameraIndex_ = 0;
	char searchBuf_[64] = "";
	bool logOpen_ = false;
	std::string status_;
	// "Make report" dialog on the Results page.
	struct ReportForm {
		char from[16] = "", to[16] = "", cmpFrom[16] = "", cmpTo[16] = "";
		char note[600] = "";
		bool snapshots = false, compare = false;
		std::string saved, error;            // path of the last report written, or why it failed
	} reportForm_;
};
