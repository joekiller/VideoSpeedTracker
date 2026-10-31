// VST Studio application state and pages.
//
// One App lives for the life of the window.  Each GUI frame, frame() draws the side rail, the header and the
// current page, then drains work queued by the companion API (Server runs on its own threads and hands anything
// that touches App state to the GUI thread through post()).
//
// Setup is one flow of five numbered steps (kFlow in App.cpp), drawn the same way in the side rail and in the
// stepper at the top of each step's page, with the header's Next button walking it in order:
//   1 What you need      (Guide.cpp, guideStep_ 0)  what each later step will need, in step order
//   2 Pick your spot     (Guide.cpp, guideStep_ 1)  the site planner: the street first, the camera a guess
//   3 Connect the camera (Source)    video files (batch), a USB camera, or a network stream; how to aim it
//   4 Mark your street   (Zones)     drag the analysis box, speed lines, lanes and obstruction on the picture
//   5 Calibrate                      drive-through calibration or counting frames by hand
// Then the pages used once it's set up:
//   Live                     tracking with overlays, live counts and a feed of logged vehicles
//   Results                  speeders and wrong-way drivers, charts, snapshots, csv and highlights
//   Companion                pairing a phone with the local API (QR code)
#pragma once
#include "Calibration.h"
#include "DashCam.h"
#include "Health.h"
#include "Media.h"
#include <imgui.h>
#include "Plan.h"
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
struct View;

enum class Page { Guide, Source, Zones, Calibrate, Track, Results, Companion, Count };

// One step of the setup flow: its name (the same in the rail, the stepper, the page header and every Next button),
// the page that shows it, and for the Guide's two steps which one.
struct FlowStep { const char* name; const char* icon; const char* subtitle; Page page; int guideStep; };
constexpr int kFlowSteps = 5;
extern const FlowStep kFlow[kFlowSteps];

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
	// configPath (from --config) is loaded instead of the remembered or per-user VST.cfg.
	void init(bool fresh, const std::string& configPath = "");
	void frame(float width, float height);
	bool wantsContinuousFrames();
	void shutdown();

	// --- shared actions (also used by the companion API and the screenshot script) ---
	bool openSource(const std::string& path);
	// Closes the preview and forgets the source (and any batch), so nothing is connected: the Disconnect buttons.
	void disconnectSource();
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
	void dashcamSpeed(const Vehicle* picked);   // Calibrate: fill the driven speed from a Tesla dashcam clip
	double drivenMph(int vehicleId) const;      // calibSpeed_, or the dashcam's unrounded speed when it set it
	// Replaces the direction's calibration with the mean of what the first pass (the CalibrationFrames the check
	// was made against) and the check pass imply; recorded as an apply.
	void averageCalibration(const CalCheck& check);
	// Sets CalibrationFrames for one or both directions from the two-cone or known-car measurement
	// (calib::framesForDistance); `from` names it in the toast.  Recorded in the history as the equivalent pass: CalibrationFrames frames at CalibrationSpeed.
	void applyDistanceCalibration(bool l2r, bool r2l, double calFrames, const char* from = "the measured spots");
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

	// Unattended-run health (AppHealth.cpp): today's numbers for health.csv, the video folders against the disk
	// budget, and the camera check against the reference view.  GUI thread only (the API reads it via post()).
	struct HealthState {
		health::Day today;
		health::Usage video;            // Clips + HiLites, as of the last prune
		std::uintmax_t budgetBytes = 0; // prefs.keepVideoGB in bytes (0 = keep everything)
		std::uintmax_t freeBytes = 0;   // free space in the data folder, as of the last prune (0 = unknown)
		bool lowDisk = false;           // under kMinFreeBytes even after pruning
		health::ViewCheck view;         // the latest camera check
		std::string viewCheckedAt;      // ISO time of it ("" = none yet)
		int movedStreak = 0;            // consecutive "moved" checks
		bool cameraWarning = false;     // kMovedChecks in a row: the zones probably miss the street now
	} health;
	static constexpr std::uintmax_t kMinFreeBytes = 2ull << 30;   // pruning goes past the budget to keep 2 GB free
	static constexpr int kMovedChecks = 3;                        // ~6 minutes of "moved" before warning
	void useCurrentViewAsReference();

	// The site planner's street (Guide.cpp; model in Plan.h).  It starts as a rough guess and is refined: typed,
	// sized from the camera's picture, or measured by any calibration.  planInputs() is the model's view of prefs;
	// setPlanStreet() records new street numbers and where they came from.  planFromCalibration() works out what the
	// calibration in force says the street is (false when there is none for this camera, or it's the bundled sample's),
	// and syncPlanFromCalibration() applies it once per new calibration, so the planner follows step 5 by itself and
	// a number typed afterwards sticks.  Called after every calibration and whenever the planner or /api/plan is shown.
	plan::Inputs planInputs() const;
	void setPlanStreet(double distM, double clearM, plan::Source source, const std::string& note);
	bool planFromCalibration(plan::Measured& m, std::string& how);
	void syncPlanFromCalibration(bool announce = true);
	// "Cars across the picture" needs a picture of the user's own street: an open camera, stream, video or photo that
	// isn't the bundled 2016 sample.
	bool planPictureAvailable();
	// What a frame rate means for the planned view, in a sentence, with what to do about a low one (the Connect the
	// camera page's frame-rate check and the planner's camera section).
	std::string frameRateAdvice(double fps);
	std::string referenceViewPath();     // <configDir>/view-reference.jpg

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
	// The Calibrate page's "Cones or pole" mode: each cone's base on the picture (1280-wide grid, -1 = not marked), which
	// one the next click on the picture places (-1 = none), the measured distance between them, and the directions
	// it applies to (0 both, 1 left to right, 2 right to left).
	float coneX_[2] = { -1, -1 }, coneY_[2] = { -1, -1 };
	int conePlacing_ = -1;
	double coneFt_ = 10;
	int coneDir_ = 0;
	// The same alone, with one pole and nobody at the computer (or the phone busy being the camera): timed pictures.
	// Start the timer, walk out, hold the pole on spot 1 for the first picture (kConeFirstS after the start) and on
	// spot 2 for the second (kConeGapS later), then come back and click the pole in each picture.  coneTimerAt_ is
	// now() at the start (-1 = no timer); coneView_ is the picture the Calibrate canvas shows (-1 = the camera itself),
	// coneTexOf_ the one uploaded to coneTex_.
	static constexpr double kConeFirstS = 30, kConeGapS = 20;
	// "Level the picture" on Mark your street: levelTool_ shows two dots (frame coordinates) the user lays along the
	// curb, and "Level to this line" turns cfg.g.levelDegrees by their slope; levelGrid_ shows the reference rows while
	// the slider is being dragged (set by the inspector, read by the next frame's canvas).
	bool levelTool_ = false, levelGrid_ = false;
	ImVec2 levelDots_[2] = { { 360, 560 }, { 920, 560 } };
	cv::Mat coneShot_[2];
	double coneTimerAt_ = -1;
	int coneView_ = -1, coneTexOf_ = -1;
	Texture coneTex_;
	// The "Known car" mode (calibMode_ 3) marks a car's two bumpers the way the cones mode marks its spots, sharing
	// conePlacing_ and coneDir_: carX_/carY_ are the bumpers on the picture (-1 = not marked), carModel_ the picker's
	// entry in calib::kKnownCars (-1 = typed by hand), carFt_ the length in feet, and carParked_/carFartherFt_ how far
	// beyond the middle of the traffic lane a parked car sits (calib::markPxAtLane).
	float carX_[2] = { -1, -1 }, carY_[2] = { -1, -1 };
	int carModel_ = -1;
	double carFt_ = 15.5;
	bool carParked_ = false;
	double carFartherFt_ = 7;
	std::unique_ptr<Server> server;
	bool screenshotMode = false;        // documentation captures (main.cpp --shots)
	bool persist = true;                // save preferences on exit

private:
	friend struct DemoScript;
	void drawRail(float h);
	void drawHeader(float w);
	void drawToasts(float w, float h);
	void pageGuide();
	// The setup flow (App.cpp).  flowStep() is the step the current page shows, or -1 off the flow.
	int flowStep() const;
	bool flowDone(int step);
	void goFlow(int step);
	void flowStepper();                 // the numbered stepper at the top of each setup page
	void aimCard(float width);          // Guide.cpp: how to aim and mount the camera, on the Connect the camera page
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
	// While tracking, the setup pages show the tracker's own frames through player.feed(), never a second capture
	// (App.cpp, pumpTracker).  sharedFeedBadge() marks such a picture as Live tracking's and says edits apply on the
	// next start; feedHint() is a canvas's empty-state text, or "waiting for tracking's first frame" during a run.
	bool showsSharedFeed() const;
	void sharedFeedBadge(const View& v);
	const char* feedHint(const char* idle) const;
	void sitePlanner();
	void shortViewNote(double coverM, bool on);   // Guide.cpp: the planner's short-view advice and its settings button
	void zeroInCard(float width);       // Guide.cpp: refining the street, from the picture or from a calibration
	void autosave();
	void recordCalibration(CalEvent e);
	const std::vector<CalEvent>& calibrationHistory();
	void calibrationCheckCard(float width);
	void healthTick();
	void healthCard();
	void flushHealthDay();
	std::string zonesSignature();

	struct Toast { std::string text; unsigned color; const char* icon; double until; };
	std::deque<Toast> toasts_;
	std::mutex postMu_;
	std::deque<std::pair<std::function<std::string()>, std::shared_ptr<std::promise<std::string>>>> posted_;
	Tracker::Live live_;
	Texture liveTex_, motionTex_;
	// Each new tracker frame is uploaded once, to whichever texture the current page draws: liveTex_ on Live tracking,
	// the player's on a setup page.  The serials say which frame each one last got (-1 = none since the run started).
	long liveTexSerial_ = -1, fedSerial_ = -1;
	double runLevel_ = 0;               // cfg.g.levelDegrees when the run started: the angle its frames already carry
	std::map<int, std::unique_ptr<Texture>> snapTex_;
	int seenVehicleId_ = 0;
	bool wasRunning_ = false;
	int guideStep_ = 0;                 // which of the Guide's two steps (What you need, Pick your spot) is showing
	bool showZeroIn_ = false;           // scroll Pick your spot to its "Zero in on it" card on the next frame
	int resultsFilter_ = 0;
	int selectedRow_ = -1;
	int calibMode_ = 0;
	int calibDir_ = 0;
	int calibPickId_ = 0;
	int calibSpeed_ = 25;
	bool calibChecking_ = false;        // the Calibrate page's pass picker is choosing a check pass, not a calibration
	int calibAppliedId_ = 0;            // the session vehicle the latest calibration came from (not a fair check)
	std::vector<dashcam::Clip> dashClips_;   // Tesla dashcam clips loaded on the Calibrate page
	int dashFor_ = 0;                   // the vehicle dashMph_ was read for, while calibSpeed_ still shows it rounded
	double dashMph_ = 0;                // the car's own speed through the zone, to 0.1 mph
	CalCheck lastCheck_;                // the latest check pass, for the result card
	std::vector<CalEvent> calHistory_;  // calibration-history.csv rows for calHistoryFor_
	std::string calHistoryFor_;
	bool calHistoryLoaded_ = false;
	// Companion page: this machine's network addresses for the pairing-address picker, re-read every few seconds.
	std::vector<std::string> lanChoices_;
	std::string lanGuess_;
	double lanReadAt_ = -1e9;
	// Health bookkeeping (AppHealth.cpp).
	double healthAt_ = 0;               // last healthTick, for the time accounting
	double nextPrune_ = 0, nextViewCheck_ = 0;
	bool wasOutage_ = false;
	int healthVehicles_ = 0;            // tracker.counts().vehicles at the last tick
	struct PruneJob { health::Pruned pruned; std::uintmax_t freeBytes = 0; };
	std::future<PruneJob> pruning_;
	double autosaveAt_ = 0;             // when the pending autosave fires (0 = nothing pending)
	double savedAt_ = 0;                // last successful autosave, for the rail's "saved" note
	std::string sampleCache_;
	char urlBuf_[512] = "rtsp://";
	int cameraIndex_ = 0;
	// Raspberry Pi cameras (PiCamera.h).  `rpicam-vid --list-cameras` takes a moment, so init() starts it in the
	// background where rpicam-vid exists, and the camera card picks the result up when it's ready.  On a Pi with a
	// camera found, the card's menu starts on it rather than on USB camera 0.
	std::future<std::vector<picam::Camera>> piListing_;
	std::vector<picam::Camera> piCams_;
	bool piListed_ = false;
	int piChoice_ = -1;                 // index into piCams_ when the menu is on a Pi camera, -1 for the USB camera
	picam::Settings piSettings_;        // the Pi camera settings popup; made into the source name on Connect
	void piCameraSettings();            // that popup's contents
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
