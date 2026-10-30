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
#include "Media.h"
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
	std::string dataDir();
	std::string findSample();
	// Sets the calibration for one direction from a pass at a known speed; keeps the other direction's
	// calibration valid by expressing both against the same CalibrationSpeed/CalibrationFPS.
	void applyCalibration(bool l2r, double mph, int frames, double fps);
	void loadResultsCsv(const std::string& path);
	void showSessionResults();
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
	Texture* snapshotTexture(int vehicleId);
	void pumpTracker();
	void sitePlanner();

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
	float calibSpeed_ = 25;
	char urlBuf_[512] = "rtsp://";
	int cameraIndex_ = 0;
	char searchBuf_[64] = "";
	bool logOpen_ = false;
	std::string status_;
};
