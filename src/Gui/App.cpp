// portable-file-dialogs pulls in the Windows headers; they must be parsed before Globals.h's
// "using namespace std", or rpcndr.h's byte collides with std::byte.
#include <portable-file-dialogs.h>
#include "App.h"
#include "Canvas.h"
#include "Icons.h"
#include "Resume.h"
#include "Server.h"
#include "Theme.h"
#include "UserPaths.h"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <random>

namespace fs = std::filesystem;
using namespace theme;

// Font Awesome "file-lines" for the street report, kept here rather than in Icons.h while only the report uses it.
#define ICON_REPORT "\xef\x85\x9c"
#ifndef VST_VERSION
#define VST_VERSION "dev"
#endif

static ImTextureID tid(Texture& t) { return (ImTextureID)(intptr_t)t.imguiId(); }

// The setup flow, in the order a first-time user does it.  Everything that numbers or names a setup step reads this
// table, so the rail, the stepper, the page headers and the Next buttons can't drift apart.
const FlowStep kFlow[kFlowSteps] = {
	{ "What you need", ICON_FLAG, "Everything you need to measure traffic on your street, in the order you'll use it", Page::Guide, 0 },
	{ "Pick your spot", ICON_LOCATE, "See what your window can measure, before you buy or mount anything", Page::Guide, 1 },
	{ "Connect the camera", ICON_CAMERA, "A phone, USB or Raspberry Pi camera, a network camera, or video you've already recorded", Page::Source, -1 },
	{ "Mark your street", ICON_VECTOR, "Drag the lines on the picture; the tracker only looks inside the box", Page::Zones, -1 },
	{ "Calibrate", ICON_RULER, "Teach VST how long a car at a known speed takes to cross the zone", Page::Calibrate, -1 },
};

static const char* pageTitle(Page p) {
	switch (p) {
	case Page::Guide: return kFlow[0].name;
	case Page::Source: return kFlow[2].name;
	case Page::Zones: return kFlow[3].name;
	case Page::Calibrate: return kFlow[4].name;
	case Page::Track: return "Live tracking";
	case Page::Results: return "Results";
	case Page::Companion: return "Companion app";
	default: return "";
	}
}

static const char* pageIcon(Page p) {
	switch (p) {
	case Page::Guide: return kFlow[0].icon;
	case Page::Source: return kFlow[2].icon;
	case Page::Zones: return kFlow[3].icon;
	case Page::Calibrate: return kFlow[4].icon;
	case Page::Track: return ICON_GAUGE;
	case Page::Results: return ICON_CHART;
	case Page::Companion: return ICON_BROADCAST;
	default: return "";
	}
}

static const char* pageSubtitle(Page p) {
	switch (p) {
	case Page::Guide: return kFlow[0].subtitle;
	case Page::Source: return kFlow[2].subtitle;
	case Page::Zones: return kFlow[3].subtitle;
	case Page::Calibrate: return kFlow[4].subtitle;
	case Page::Track: return "Watch vehicles get measured as they pass";
	case Page::Results: return "Speeders and wrong-way drivers, with snapshots and statistics";
	case Page::Companion: return "Check on the camera and see recent speeders from your phone";
	default: return "";
	}
}

static double now() { return glfwGetTime(); }

// AppPrefs::lastPage: the page (and for the Guide, which of its two steps) as a word, so a reordered Page enum can't
// reopen Studio on the wrong page.
static const char* const kPageKeys[] = { "guide", "source", "zones", "calibrate", "live", "results", "companion" };
static_assert(sizeof kPageKeys / sizeof kPageKeys[0] == size_t(Page::Count), "a key for every page");

static std::string pageKey(Page p, int guideStep) {
	if (p == Page::Guide) return guideStep > 0 ? "guide2" : "guide";
	return int(p) >= 0 && p < Page::Count ? kPageKeys[int(p)] : "";
}

static bool pageFromKey(const std::string& key, Page& p, int& guideStep) {
	if (key == "guide2") { p = Page::Guide; guideStep = 1; return true; }
	for (int i = 0; i < int(Page::Count); i++)
		if (key == kPageKeys[i]) { p = Page(i); guideStep = 0; return true; }
	return false;
}

// ======================================================================================================
// Lifecycle
// ======================================================================================================

App::App() = default;
App::~App() { shutdown(); }

void App::init(bool fresh, const std::string& configPath) {
	// fresh: default preferences and a fixed pairing code, for reproducible documentation screenshots.
	if (fresh) prefs.remoteToken = "demo-pairing-code";
	else prefs.load();
	// An explicit config wins and skips the lookup, which would otherwise seed a per-user VST.cfg (or complain
	// that there is no template to seed it from) only for it to be replaced straight away.
	std::string cfgPath = configPath.empty() ? prefs.configPath : configPath;
	if (configPath.empty() && (cfgPath.empty() || !fs::exists(cfgPath))) cfgPath = userpaths::resolveConfig("VST.cfg");
	if (!cfgPath.empty() && !loadConfig(cfgPath)) toast(cfg.error, c::bad, ICON_WARN);
	// Reopen where the user left off: the page they were on, and the source they last connected.  Every kind of
	// source comes back (resume::reopenAtLaunch); a camera or stream connects in the background, the header says
	// "Connecting to Camera 0…", and if it doesn't answer frame() says the setup is safe and how to reconnect.
	// Before, cameras were skipped here, which unchecked steps 3 and 4 and made a reopened Studio look new.
	page = prefs.guideDone ? Page::Source : Page::Guide;
	if (!fresh) pageFromKey(prefs.lastPage, page, guideStep_);
	SourceInfo::Kind lastKind = classifySource(prefs.lastSource);
	bool isPath = lastKind == SourceInfo::File || lastKind == SourceInfo::Image;
	std::error_code ec;
	if (resume::reopenAtLaunch(prefs.lastSource, isPath, isPath && fs::exists(prefs.lastSource, ec)) && openSource(prefs.lastSource))
		resumedSource_ = prefs.lastSource;
	if (prefs.remoteEnabled) setRemote(true);
	listUsbCameras();
#ifndef _WIN32
	if (!picam::findTool().empty())
		piListing_ = std::async(std::launch::async, [] { bool ok = false; std::string error; return picam::list(ok, error); });
#endif
}

void App::shutdown() {
	tracker.stop();
	if (persist) flushHealthDay();   // today so far; a restart later today adds a second row for the date
	server.reset();
	if (persist) prefs.save();
	persist = false;   // once is enough; the destructor calls this again
}

bool App::wantsContinuousFrames() {
	return tracker.running() || player.playing || player.opening() || (player.isOpen() && player.info().kind != SourceInfo::File && player.info().kind != SourceInfo::Image)
		|| !toasts_.empty() || ImGui::IsAnyItemActive();
}

std::string App::post(std::function<std::string()> fn) {
	auto p = std::make_shared<std::promise<std::string>>();
	auto fut = p->get_future();
	{
		std::lock_guard<std::mutex> l(postMu_);
		posted_.emplace_back(std::move(fn), p);
	}
	glfwPostEmptyEvent();   // wake the GUI loop if it's idling in glfwWaitEventsTimeout
	if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return "";
	return fut.get();
}

// ======================================================================================================
// Actions
// ======================================================================================================

bool App::loadConfig(const std::string& path) {
	if (tracker.running()) { toast("Stop tracking before switching configs", c::warn, ICON_WARN); return false; }
	if (!cfg.load(path)) return false;
	prefs.configPath = path;
	// A config still on the engine's default 18 on a slower street (set before minLoggedSpeed followed the limit):
	// bring it in line once, so legal traffic at 12-17 mph on a 15 mph street is logged.  Autosave writes it.
	if (cfg.g.minLoggedSpeed == calib::kDefaultMinLogged) cfg.g.minLoggedSpeed = calib::minLoggedForLimit(prefs.speedLimit);
	return true;
}

bool App::saveConfig() {
	auto problems = cfg.problems();
	if (!problems.empty()) { toast("Fix the highlighted settings first: " + problems[0], c::bad, ICON_WARN); return false; }
	if (!cfg.save()) { toast(cfg.error, c::bad, ICON_WARN); return false; }
	toast("Saved " + fs::path(cfg.path()).filename().string(), c::ok, ICON_CHECK_CIRCLE);
	return true;
}

// Judged on the connected source, or the remembered one while it's unplugged or reconnecting (resume::setupSource), so
// the check doesn't vanish because a camera is a moment late.
bool App::zonesDone() {
	// By device, not by name: "0" and "0?w=1920&h=1080" are the same camera, so a mode change keeps step 4 done.
	return resume::zonesStepDone(frameio::deviceKey(resume::setupSource(source, prefs.lastSource)), frameio::deviceKey(prefs.zonesFor), cfg.problems().empty());
}

bool App::calibrationDone() {
	// The bundled sample's VST.cfg comes calibrated for that video, so the sample never nags.
	if (sampleCache_.empty()) sampleCache_ = findSample();
	if (!source.empty() && source == sampleCache_) return true;
	return !cfg.path().empty() && prefs.calibratedFor == cfg.path() && cfg.problems().empty() && !calibrationStale();
}

// A calibration counts frames between the speed lines, so moving them (by hand on Mark your street, or short-view
// timing's re-fit on Save in step 2) or resizing the box makes it wrong.  The lines it was made on are kept with it.
std::string App::speedLinesSig() const {
	return std::to_string(cfg.g.speedLineLeft) + "," + std::to_string(cfg.g.speedLineRight) + "," + std::to_string(cfg.g.AnalysisBoxWidth);
}

bool App::calibrationStale() const {
	return !cfg.path().empty() && prefs.calibratedFor == cfg.path() && !prefs.calibratedLines.empty() && prefs.calibratedLines != speedLinesSig();
}

// Settings save themselves.  Edits are debounced: a pending save waits until no control has been active for
// 0.8 s, so dragging a speed line writes once when the mouse is released, not every frame.  Invalid settings
// are never written (the inspector lists what to fix), and the first save of the session keeps the original in
// VST.cfg.bak for "Undo changes".  The documentation tour never writes, so it can't touch a real config.
void App::autosave() {
	if (screenshotMode || cfg.path().empty() || !cfg.dirty() || !cfg.problems().empty()) { autosaveAt_ = 0; return; }
	double t = now();
	if (ImGui::IsAnyItemActive() || autosaveAt_ == 0) { autosaveAt_ = t + 0.8; return; }
	if (t < autosaveAt_) return;
	autosaveAt_ = 0;
	if (cfg.save()) savedAt_ = t;
	else toast(cfg.error, c::bad, ICON_WARN);
}

bool App::remoteOn() const { return server && server->running() && !localServer_; }

void App::savePrefsSoon() {
	double t = now();
	if (!persist || screenshotMode || t < prefsCheckAt_ || ImGui::IsAnyItemActive()) return;
	prefsCheckAt_ = t + 2;
	std::string text = prefs.render();
	if (prefsWritten_.empty()) { prefsWritten_ = text; return; }   // as loaded; nothing to write yet
	if (text == prefsWritten_) return;
	prefs.save();
	prefsWritten_ = text;
}

std::string App::dataDir() {
	// Same rule as the tracker: blank means the per-user data folder.
	return userpaths::dataPath(cfg.g.dataPathPrefix).string();
}

std::string App::findSample() {
	// Installed packages put the 2016 Locust Avenue sample under share/videospeedtracker/sample (Linux) or
	// sample/ next to the exe (Windows, macOS); a source checkout has it in sampleData/.
	if (fs::path installed = userpaths::sampleVideo(); !installed.empty()) return installed.string();
	const char* name = "manual_20160130172251.avi";
	fs::path exe = userpaths::executableDir();
	for (fs::path p : { exe / "sample" / name, exe / ".." / "share" / "videospeedtracker" / "sample" / name,
		fs::path("sampleData/IPCam/20160130") / name, exe / ".." / "sampleData/IPCam/20160130" / name,
		exe / ".." / ".." / "sampleData/IPCam/20160130" / name, exe / ".." / ".." / ".." / "sampleData/IPCam/20160130" / name }) {
		std::error_code ec;
		if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec).string();
	}
	return "";
}

// Files open here and now; a camera or stream starts connecting in the background (Player), so the window never waits
// on a device.  `source` is set straight away so the header and preview name what is connecting; if the connection
// fails, frame() clears it again and says why, rather than leaving the old picture or a stale name behind.
bool App::openSource(const std::string& path) {
	if (tracker.running()) { toast("Stop tracking before changing the source", c::warn, ICON_WARN); return false; }
	resumedSource_.clear();   // a source picked this launch (init() sets it again for the one it reopens)
	if (!player.open(path)) { toast(player.error, c::bad, ICON_WARN); source.clear(); return false; }
	source = path;
	// A Pi camera reopened from Recent brings its settings back into the camera card's settings popup.
	picam::Settings pi;
	std::string piError;
	if (picam::isName(path) && picam::parse(path, pi, piError)) {
		piSettings_ = pi;
		for (int i = 0; i < int(piCams_.size()); i++) if (piCams_[i].index == pi.camera) piChoice_ = i;
	}
	// A USB camera, likewise: the menu and its settings popup follow the camera being opened.
	webcam::Settings usb;
	if (webcam::parse(path, usb)) { usbSettings_ = usb; cameraIndex_ = usb.camera; piChoice_ = -1; }
	if (player.opening()) pendingRecent_ = path;   // remembered once it answers (frame())
	else rememberSource(path);
	return true;
}

// The source opened: reopen it next launch and list it under Recent.
void App::rememberSource(const std::string& path) {
	pendingRecent_.clear();
	prefs.lastSource = path;
	// One Recent entry per camera: a camera reopened with other settings replaces its old entry rather than adding one.
	if (frameio::isCamera(frameio::classify(path))) {
		const std::string key = frameio::deviceKey(path);
		prefs.recent.erase(std::remove_if(prefs.recent.begin(), prefs.recent.end(), [&](const std::string& r) { return r != path && frameio::deviceKey(r) == key; }), prefs.recent.end());
	}
	prefs.addRecent(path);
}

void App::disconnectSource() {
	if (tracker.running()) { toast("Stop tracking before disconnecting", c::warn, ICON_WARN); return; }
	std::string was = source.empty() ? std::string() : friendlySourceName(source);
	player.close();
	source.clear();
	batch.clear();
	prefs.lastSource.clear();
	if (!was.empty()) toast("Disconnected " + was, c::accent2, ICON_LINK);
}

void App::addToBatch(const std::vector<std::string>& paths) {
	for (const std::string& p : paths)
		if (std::find(batch.begin(), batch.end(), p) == batch.end()) batch.push_back(p);
	if (source.empty() && !batch.empty()) openSource(batch.front());
}

void App::startTracking() {
	if (tracker.running()) return;
	std::vector<std::string> inputs = batch.empty() ? std::vector<std::string>{ source } : batch;
	if (inputs.empty() || inputs[0].empty()) { toast("Connect a camera or open a video first (step 3)", c::warn, ICON_VIDEO); goFlow(2); return; }
	if (classifySource(inputs[0]) == SourceInfo::Image) { toast("A photo is fine for planning zones, but tracking needs video", c::warn, ICON_IMAGE); return; }
	auto problems = cfg.problems();
	if (!problems.empty()) { toast("Fix the zone settings first: " + problems[0], c::bad, ICON_WARN); page = Page::Zones; return; }
	// Run with the settings on screen (saved or not) via a scratch copy, so experiments don't touch VST.cfg.
	std::string runCfg = cfg.dirty() ? cfg.writeRunCopy(userpaths::configDir().string()) : cfg.path();
	if (runCfg.empty()) { toast("Can't write the run settings to " + userpaths::configDir().string(), c::bad, ICON_WARN); return; }
	player.closeAndWait(2);   // a camera can only be opened by one reader at a time: let the preview's let go
	Tracker::Options o;
	o.configPath = runCfg;
	o.dataDir = dataDir();
	o.speedLimit = prefs.speedLimit;
	o.egregious = std::max(prefs.egregious, prefs.speedLimit);
	o.realtime = prefs.realtime;
	o.highlights = prefs.highlights;
	o.hlMin = prefs.hlMin;
	o.hlMax = prefs.hlMax;
	o.hlArea = prefs.hlArea;
	o.trace = prefs.trace;
	o.largeArea = cfg.g.largeVehicleArea;
	std::vector<Job> jobs;
	for (auto& in : inputs) { Job j; j.input = in; jobs.push_back(j); }
	live_ = Tracker::Live();
	liveTexSerial_ = fedSerial_ = -1;
	runLevel_ = cfg.g.levelDegrees;   // the run copy carries this angle, so the frames it hands back are already turned by it
	tracker.start(jobs, o);
	showSessionResults();   // Results follows the run (the saved runs are a click away in its menu)
	page = Page::Track;
	wasRunning_ = true;
}

void App::stopTracking() {
	tracker.stop();
}

bool App::followSpeedLimit(int before) {
	Globals& g = cfg.g;
	if (prefs.speedLimit == before || !calib::minLoggedFollows(g.minLoggedSpeed, before)) return false;
	int want = calib::minLoggedForLimit(prefs.speedLimit);
	if (g.minLoggedSpeed == want) return false;
	g.minLoggedSpeed = want;   // autosave writes VST.cfg
	return true;
}

void App::applyCalibration(bool l2r, double mph, int frames, double fps) {
	const Globals& g = cfg.g;
	if (mph <= 0 || frames <= 0) return;
	if (fps <= 0) fps = g.CalibrationFPS;
	// A one-way street has one lane, so a pass either way measures it for both directions: a wrong-way car is timed in
	// the same lane.  On a two-way street each lane is its own distance from the camera, so a pass speaks for its own.
	const bool oneWay = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
	calib::Input in;
	in.method = "pass";
	in.dirs = oneWay ? calib::kBothDirs : l2r ? calib::kL2R : calib::kR2L;
	in.zoneFt = calib::zoneFtFromPass(mph, frames, fps);
	in.sigma = calib::sigmaPass(mph, frames);
	char label[64];
	snprintf(label, sizeof label, "Drive past at %g mph%s", std::round(mph * 10) / 10, oneWay ? "" : l2r ? ", left → right" : ", right → left");
	in.label = label;
	addCalibrationInput(in, "the drive past");
}

CalCheck App::checkCalibration(bool l2r, double mph, int frames, double fps) {
	const Globals& g = cfg.g;
	CalCheck r;
	if (mph <= 0 || frames <= 0) return r;
	if (fps <= 0) fps = g.CalibrationFPS;
	r.valid = true;
	r.l2r = l2r;
	r.drivenMph = mph;
	r.frames = frames;
	r.fps = fps;
	r.calFrames = l2r ? g.CalibrationFramesL2R : g.CalibrationFramesR2L;
	// Measured and error are rounded to 0.1 mph before the verdict, so the number on screen, the one in the history
	// file and the verdict always agree (an error shown as 2.0 is never a failure).
	r.measuredMph = std::round(calib::measuredMph(r.calFrames, frames, fps, g.CalibrationSpeed, g.CalibrationFPS) * 10) / 10;
	r.error = std::round((r.measuredMph - mph) * 10) / 10;
	r.verified = std::fabs(r.error) <= calib::kVerifiedMph + 1e-9;
	CalEvent e;
	e.check = true;
	e.l2r = l2r;
	e.drivenMph = mph;
	e.frames = frames;
	e.fps = fps;
	e.calFrames = r.calFrames;
	e.measuredMph = r.measuredMph;
	e.error = r.error;
	recordCalibration(e);
	lastCheck_ = r;
	return r;
}

void App::averageCalibration(const CalCheck& check) {
	if (!check.valid) return;
	// The check pass is honest evidence too: it joins the inputs like any pass, weighted by its own uncertainty, so
	// the calibration becomes the weighted middle of everything measured (two passes: about their mean).  The page's
	// check result goes, and the direction reads "not checked" until another pass checks the combination.
	applyCalibration(check.l2r, check.drivenMph, check.frames, check.fps);
	lastCheck_ = CalCheck();
}

const std::vector<calib::Input>& App::calibrationInputs() {
	if (calInputsLoaded_ && calInputsFor_ == cfg.path()) return calInputs_;
	calInputsFor_ = cfg.path();
	calInputsLoaded_ = true;
	bool known = false;
	calInputs_ = screenshotMode || cfg.path().empty() ? std::vector<calib::Input>() : calib::loadInputs(calib::inputsPath(), cfg.path(), &known);
	if (known || !calibrationDone()) return calInputs_;
	// A calibration from before inputs were kept: seed it as one input per direction so the next method adds to it
	// rather than wiping it out.  The history's latest apply that set today's value says what it was: a pass whose own
	// numbers imply that value is that pass, with a pass's uncertainty; anything else (a cones or car calibration, an
	// average, a hand edit, the sample's VST.cfg) is "Earlier calibration" at kSigmaEarlier.  The seed is saved with
	// the first change, so until then it's recomputed the same way each time.
	const Globals& g = cfg.g;
	const std::vector<CalEvent>& rows = calibrationHistory();
	std::string lines = speedLinesSig();
	for (bool l2r : { true, false }) {
		int frames = l2r ? g.CalibrationFramesL2R : g.CalibrationFramesR2L;
		if (frames <= 0) continue;
		const CalEvent* last = nullptr;
		for (const CalEvent& e : rows) if (!e.check && e.l2r == l2r && e.calFrames == frames) last = &e;
		calib::Input in;
		in.time = last ? last->time : "";
		in.dirs = l2r ? calib::kL2R : calib::kR2L;
		in.lines = lines;
		double implied = last ? calib::impliedFrames(last->drivenMph, last->frames, last->fps, g.CalibrationSpeed, g.CalibrationFPS) : 0;
		bool wasPass = last && std::lround(implied) == frames
			&& !(std::fabs(last->drivenMph - g.CalibrationSpeed) < 1e-6 && last->frames == frames && std::fabs(last->fps - g.CalibrationFPS) < 1e-6);
		if (wasPass) {
			in.method = "pass";
			in.zoneFt = calib::zoneFtFromPass(last->drivenMph, last->frames, last->fps);
			in.sigma = calib::sigmaPass(last->drivenMph, last->frames);
			char label[64];
			snprintf(label, sizeof label, "Drive past at %g mph", last->drivenMph);
			in.label = label;
		}
		else {
			in.method = "earlier";
			in.zoneFt = calib::zoneFtForFrames(frames, g.CalibrationSpeed, g.CalibrationFPS);
			in.sigma = calib::kSigmaEarlier;
			in.label = "Your earlier calibration";
		}
		// The same measurement both ways (a one-way street's pass): one input for the lane, not two.
		if (!l2r && !calInputs_.empty() && calInputs_[0].method == in.method && std::fabs(calInputs_[0].zoneFt - in.zoneFt) < 1e-6) {
			calInputs_[0].dirs = calib::kBothDirs;
			continue;
		}
		if (in.time.empty()) in.time = calib::nowIso();
		in.id = calib::newInputId(calInputs_, in.time);
		calInputs_.push_back(in);
	}
	return calInputs_;
}

calib::Combined App::calibrationCombined(bool l2r) {
	return calib::combine(calibrationInputs(), l2r, speedLinesSig());
}

void App::saveCalibrationInputs() {
	if (screenshotMode || cfg.path().empty()) return;
	std::string err;
	if (!calib::saveInputs(calib::inputsPath(), cfg.path(), calInputs_, err)) toast(err, c::bad, ICON_WARN);
}

void App::addCalibrationInput(calib::Input in, const char* from) {
	if (!(in.zoneFt > 0) || !(in.sigma > 0) || !(in.dirs & calib::kBothDirs)) return;
	calibrationInputs();   // loaded (and seeded) for this config before adding to it
	if (in.time.empty()) in.time = calib::nowIso();
	if (in.lines.empty()) in.lines = speedLinesSig();
	bool added = calib::addInput(calInputs_, in);
	saveCalibrationInputs();
	applyCombinedCalibration();
	// The toast says what changed and what it all adds up to now, in the same words as the page.
	calib::Combined cmb = calibrationCombined(in.dirs != calib::kR2L);
	std::string head = added ? std::string("Added ") + from + ". " : std::string("That one is already counted. ");
	toast(head + calib::summary(cmb) + ".", cmb.valid && (cmb.n == 1 || cmb.agree()) ? c::ok : c::warn, ICON_CHECK_CIRCLE);
}

bool App::setCalibrationInputRemoved(const std::string& id, bool removed) {
	calibrationInputs();
	for (calib::Input& in : calInputs_) {
		if (in.id != id) continue;
		in.removed = removed;
		saveCalibrationInputs();
		applyCombinedCalibration();
		return true;
	}
	return false;
}

void App::applyCombinedCalibration() {
	Globals& g = cfg.g;
	bool any = false, changed = false;
	for (bool l2r : { true, false }) {
		calib::Combined cmb = calibrationCombined(l2r);
		if (!cmb.valid) continue;
		any = true;
		// The engine counts whole frames, so the combination is rounded here; the inputs keep the exact value.
		int frames = std::max(1, int(std::lround(calib::framesForZone(cmb.zoneFt, g.CalibrationSpeed, g.CalibrationFPS))));
		int& cur = l2r ? g.CalibrationFramesL2R : g.CalibrationFramesR2L;
		// Only a change of value is a new calibration in the history.  An input that agrees to the frame leaves the
		// value, and any check of it, standing.
		bool wasCalibrated = calibrationState(l2r).kind != CalState::None && calibrationDone();
		if (frames == cur && wasCalibrated) continue;
		cur = frames;
		changed = true;
		// Recorded as the pass the value stands for: a car at CalibrationSpeed taking CalibrationFrames frames at
		// CalibrationFPS (the history format has no column for the method; calibration-inputs.json has the detail).
		CalEvent e;
		e.l2r = l2r;
		e.drivenMph = g.CalibrationSpeed;
		e.frames = frames;
		e.fps = g.CalibrationFPS;
		e.calFrames = frames;
		recordCalibration(e);
		if (lastCheck_.l2r == l2r) lastCheck_ = CalCheck();
	}
	if (any) {
		prefs.calibratedFor = cfg.path();
		prefs.calibratedLines = speedLinesSig();   // autosave writes it to VST.cfg on the next frame
	}
	else if (prefs.calibratedFor == cfg.path()) prefs.calibratedFor.clear();   // every input removed: not calibrated
	calibAppliedId_ = 0;
	syncPlanFromCalibration();   // step 2's planner now uses the measured street (Guide.cpp)
	// A live run is restarted only when CalibrationFrames actually moved: an input that agrees to the frame leaves the
	// running csv on the current calibration, so it isn't split into a new run for nothing.
	if (changed || !any) {
		restartAfterCalibration();
		// Results' speeds and feet follow the new scale at once, not on its next refresh: the open set on this view
		// takes the new calibration (CalSets.h) and its rows are re-timed.
		if (!reviewing()) { syncSets(true); applySets(); }
	}
}

// The camera as the Lane card's ghost car describes it: step 2's distance (stored in metres) and lens, with the height
// and angle typed beside the ghost.
perspective::CameraSetup App::cameraSetup() const {
	perspective::CameraSetup c;
	c.distanceFt = prefs.planDist * plan::kFt;
	c.heightFt = prefs.calcHeightFt;
	c.yawDeg = prefs.calcYawDeg;
	c.hfovDeg = prefs.planHfov;
	return c;
}

calib::ParkingLane App::parkingLane() const {
	calib::ParkingLane p;
	p.yLeft = prefs.parkY0;
	p.yRight = prefs.parkY1;
	p.measuredFt = prefs.parkFt;
	return p;
}

double App::laneRowAt(bool l2r, double frameX) const {
	const Globals& g = cfg.g;
	return g.AnalysisBoxTop + g.lane(l2r).at(frameX - g.AnalysisBoxLeft);
}

calib::CarSpot App::carSpot(bool l2r) const {
	if (carX_[0] < 0 || carX_[1] < 0) return calib::CarSpot();
	double midX = (carX_[0] + carX_[1]) * .5, row = (carY_[0] + carY_[1]) * .5;
	return calib::placeCar(cameraSetup(), laneRowAt(l2r, midX), parkingLane(), midX, row);
}

double App::carScale(bool l2r) const {
	if (!carParked_ || carX_[0] < 0 || carX_[1] < 0) return 1;
	double midX = (carX_[0] + carX_[1]) * .5;
	if (parkingLane().set()) return carSpot(l2r).parkedScale;
	return calib::scaleBeyond(cameraSetup(), midX, carFartherFt_);
}

void App::applyDistanceCalibration(bool l2r, bool r2l, double calFramesD, const char* from, const char* method, double sigma,
	const std::string& label) {
	const Globals& g = cfg.g;
	if (!(calFramesD > 0) || (!l2r && !r2l)) return;
	calib::Input in;
	in.method = method ? method : "cones";
	in.dirs = (l2r ? calib::kL2R : 0) | (r2l ? calib::kR2L : 0);
	in.zoneFt = calib::zoneFtForFrames(calFramesD, g.CalibrationSpeed, g.CalibrationFPS);
	in.sigma = sigma > 0 ? sigma : in.method == "camera" ? calib::kSigmaCamera : in.method == "car" ? calib::sigmaCar(200, false, 0, 0)
		: calib::sigmaCones(200);
	in.label = label.empty() ? std::string("Measured from ") + from : label;
	addCalibrationInput(in, from);
}

// Appends a calibration event to calibration-history.csv and to the in-memory copy the page reads.  The
// documentation tour (--shots) keeps it in memory only, as it never writes settings either.
void App::recordCalibration(CalEvent e) {
	e.time = calib::nowIso();
	e.config = cfg.path();
	calibrationHistory();   // make sure the cache is for this config before adding to it
	calHistory_.push_back(e);
	std::string err;
	if (!screenshotMode && !calib::append(e, err)) toast(err, c::bad, ICON_WARN);
}

// ---- the user's own calibration drives, and runs measured before the latest calibration ----------------------

void App::markCalibrationPass(const Vehicle& v, bool check, double mph) {
	CalPass p;
	p.time = v.passed();
	p.stats = fs::path(v.statsPath).filename().string();
	p.frame = v.ev.frame;
	p.check = check;
	p.l2r = v.ev.leftToRight;
	p.drivenMph = mph;
	calibrationPass("", 0);   // load the file before adding to the cache
	calPasses_[report::passKey(p.stats, p.frame)] = p;
	std::string err;
	if (!screenshotMode && !calib::appendPass(p, err)) toast(err, c::bad, ICON_WARN);
	// Results shows the mark straight away, keeping the selection.
	for (ResultRow& r : rows)
		if (r.frame == p.frame && !r.statsPath.empty() && fs::path(r.statsPath).filename() == p.stats) { r.calKind = check ? 2 : 1; r.drivenMph = mph; }
}

const CalPass* App::calibrationPass(const std::string& statsPath, int frame) {
	if (!calPassesLoaded_) {
		calPassesLoaded_ = true;
		if (!screenshotMode)
			for (const CalPass& p : calib::loadPasses()) calPasses_[report::passKey(p.stats, p.frame)] = p;   // a later use of a pass wins
	}
	if (statsPath.empty()) return nullptr;
	auto it = calPasses_.find(report::passKey(fs::path(statsPath).filename().string(), frame));
	return it == calPasses_.end() ? nullptr : &it->second;
}

void App::restartAfterCalibration() {
	if (!tracker.running()) return;
	if (tracker.progress().live && batch.empty()) restartPending_ = true;   // pumpTracker() does it on the next frame
	else toast("This video keeps the calibration it started with. Run it again to measure it with the new one.", c::accent2, ICON_INFO);
}

const std::vector<CalEvent>& App::calibrationHistory() {
	// Read once per config: only this process appends to the file, and it adds each row to the cache as it goes.
	if (!calHistoryLoaded_ || calHistoryFor_ != cfg.path()) {
		calHistory_ = screenshotMode ? std::vector<CalEvent>() : calib::load(cfg.path());
		calHistoryFor_ = cfg.path();
		calHistoryLoaded_ = true;
	}
	return calHistory_;
}

CalState App::calibrationState(bool l2r) {
	// calibrationDone() covers calibrations made before the history existed (v1.1) and the bundled sample, whose
	// VST.cfg comes calibrated for that video: those count as calibrated, not checked, until a check says more.
	// With inputs on record, a direction none of them counts for (all removed, or made on other speed lines) is not
	// calibrated, whatever the history's last apply was.
	bool done = calibrationDone();
	if (!done && !calibrationInputs().empty() && !calibrationCombined(l2r).valid) return CalState();
	return calib::state(calibrationHistory(), l2r, l2r ? cfg.g.CalibrationFramesL2R : cfg.g.CalibrationFramesR2L, done);
}

void App::toast(const std::string& text, unsigned color, const char* icon) {
	// The same note twice in a row (a two-way calibration applies each direction) shows once, its timer restarted.
	for (auto it = toasts_.begin(); it != toasts_.end(); ++it)
		if (it->text == text) { toasts_.erase(it); break; }
	toasts_.push_back({ text, color, icon, now() + 4.5 });
	if (toasts_.size() > 3) toasts_.pop_front();
}

void App::setRemote(bool on) {
	prefs.remoteEnabled = on;
	// The map's local-only server makes way for the real one (or just stops).
	if (localServer_) { server.reset(); localServer_ = false; }
	if (!on) { server.reset(); return; }
	if (server && server->running()) return;
	server = std::make_unique<Server>(*this);
	if (!server->start(prefs.remotePort)) {
		toast("Can't listen on port " + std::to_string(prefs.remotePort) + ": " + server->error(), c::bad, ICON_WARN);
		server.reset();
		prefs.remoteEnabled = false;
	}
}

// ---- results --------------------------------------------------------------------------------------------

// One stats csv into `rows`, appended.  The csv names directions by compass label, mapped back with the current
// config's labels; a run's own labels (in its manifest) are the street report's business.
bool App::appendStatsRows(const std::string& path, const std::string& label, int* skipped) {
	std::vector<StatsRow> stats;
	std::string err;
	if (!readStatsCsv(path, stats, err, skipped)) { toast(err, c::bad, ICON_WARN); return false; }
	rowsFromStats(stats, calsets::readRun(path), path, label, rows);
	return true;
}

// `run` is the manifest beside the csv: the zone feet this run was measured with, which retimeRows() rescales from.
void App::rowsFromStats(const std::vector<StatsRow>& stats, const calsets::RunInfo& run, const std::string& path,
	const std::string& label, std::vector<ResultRow>& out) {
	const Globals& g = cfg.g;
	out.reserve(out.size() + stats.size());
	for (const StatsRow& s : stats) {
		ResultRow r;
		r.source = label;
		r.statsPath = path;
		r.frame = s.frame;
		r.compass = s.compass;
		r.l2r = s.compass == g.L2RDirection || (s.compass != g.R2LDirection && s.endPix > s.startPix);
		r.wrongWay = cfg.g.isWrongWay(r.l2r);
		r.speed = s.speed;
		r.frames = s.frames;
		r.area = s.area;
		r.large = s.area >= g.largeVehicleArea;
		r.flagged = s.flagged;
		r.fps = g.CalibrationFPS;
		// The Timestamp column says when this vehicle passed; drop its UTC offset to match session rows ("yyyy-mm-ddThh:mm:ss").
		// Older csvs only have Date/Time, which for Foscam recordings is when the recording started.
		r.time = !s.timestamp.empty() ? s.timestamp.substr(0, 19) : s.date.empty() ? "" : s.date + " " + s.time;
		if (const CalPass* p = calibrationPass(path, r.frame)) { r.calKind = p->check ? 2 : 1; r.drivenMph = p->drivenMph; }
		r.trend = s.trend;
		r.entryMph = s.entryMph;
		r.exitMph = s.exitMph;
		r.accel = s.accel;
		r.mphIn20ft = s.mphIn20ft;
		r.measured = { s.speed, s.zoneSeconds, !s.trend.empty(), s.entryMph, s.exitMph, s.mphIn20ft, s.accel };
		r.thenFt = run.known ? run.scale.of(r.l2r) : 0;
		r.widthPx = s.lengthPx;
		out.push_back(r);
	}
}

void App::loadResultsCsv(const std::string& path) {
	int skipped = 0;
	std::vector<ResultRow> was;
	was.swap(rows);
	if (!appendStatsRows(path, fs::path(path).filename().string(), &skipped)) { rows.swap(was); return; }
	rowsScope = ResultsScope::File;
	archive_ = archive::Info();
	dataset_ = dataset::Info();
	rowsFrom = path;
	applySets();
	rowsRuns = 1;
	selectedRow_ = -1;
	if (skipped) toast(std::to_string(skipped) + " rows in that file weren't in VST's format and were skipped", c::warn, ICON_WARN);
}

// All saved runs, newest run first: what the tracker has written to <data>/Stats over every launch.  Each row's Source
// is its run in words (resume::runLabel), e.g. "Camera 0 · 2026-11-01 13:30".  A run whose csv can't be read is
// skipped with a toast; malformed rows are counted once for all runs.
void App::showSavedRuns() {
	rows.clear();
	rowsRuns = 0;
	int skipped = 0;
	if (!screenshotMode)
		for (const resume::SavedRun& run : resume::savedRuns(dataDir())) {
			int bad = 0;
			if (appendStatsRows(run.csv, resume::runLabel(run.name), &bad)) rowsRuns++;
			skipped += bad;
		}
	rowsScope = ResultsScope::Saved;
	rowsFrom.clear();
	archive_ = archive::Info();
	dataset_ = dataset::Info();
	applySets();
	selectedRow_ = -1;
	if (skipped) toast(std::to_string(skipped) + " rows in the saved runs weren't in VST's format and were skipped", c::warn, ICON_WARN);
}

// How Results shows a speed trend: an icon, its colour and the words.  Slowing down is the good news (green), steady is
// neutral, speeding up is a warning and rapidly speeding up is red.  Unknown is a faint dash.
struct TrendLook { const char* icon; ImU32 color; const char* label; };
static TrendLook trendLook(const std::string& trend) {
	vst::Trend t = vst::trendFromName(trend.c_str());
	switch (t) {
	case vst::Trend::Decelerating: return { ICON_TREND_DOWN, c::ok, vst::trendLabel(t) };
	case vst::Trend::Steady: return { ICON_MINUS, c::dim, vst::trendLabel(t) };
	case vst::Trend::Accelerating: return { ICON_TREND_UP, c::warn, vst::trendLabel(t) };
	case vst::Trend::RapidlyAccelerating: return { ICON_ANGLES_UP, c::bad, vst::trendLabel(t) };
	default: return { ICON_MINUS, c::faint, "Can't tell" };
	}
}

void App::showSessionResults() {
	rows.clear();
	std::map<std::string, calsets::RunInfo> runs;   // each run's manifest, read once
	for (const Vehicle& v : tracker.vehiclesSince(0)) {
		ResultRow r;
		r.vehicleId = v.id;
		r.source = v.video;
		r.frame = v.ev.frame;
		r.l2r = v.ev.leftToRight;
		r.compass = v.ev.compass;
		r.wrongWay = v.ev.wrongWay;
		r.speed = v.ev.speed;
		r.frames = v.frames();
		r.area = v.ev.area;
		r.large = v.large;
		r.fps = v.fps;
		r.time = v.passed();   // empty for recordings without a start time
		r.widthPx = v.ev.width;
		r.statsPath = v.statsPath;
		if (const CalPass* p = calibrationPass(v.statsPath, r.frame)) { r.calKind = p->check ? 2 : 1; r.drivenMph = p->drivenMph; }
		const vst::SpeedTrend& t = v.ev.trend;
		if (t.known()) {
			// Rounded the way the csv rounds them, so a session row and the same car read back from the csv match.
			r.trend = vst::trendName(t.trend);
			r.entryMph = int(t.entryMph + 0.5);
			r.exitMph = int(t.exitMph + 0.5);
			r.accel = std::round(t.accelMphPerSec * 10) / 10;
			r.mphIn20ft = int(t.projectedMph + 0.5);
		}
		r.measured = { r.speed, v.ev.zoneSeconds, t.known(), r.entryMph, r.exitMph, r.mphIn20ft, r.accel };
		// A run still being written has no manifest yet (a video's is written at the end): shown as measured until then.
		if (!v.statsPath.empty()) {
			auto it = runs.find(v.statsPath);
			if (it == runs.end()) it = runs.emplace(v.statsPath, calsets::readRun(v.statsPath)).first;
			r.thenFt = it->second.known ? it->second.scale.of(r.l2r) : 0;
		}
		r.clipPath = v.clipPath;
		rows.push_back(r);
	}
	rowsScope = ResultsScope::Session;
	rowsFrom.clear();
	rowsRuns = 0;
	archive_ = archive::Info();
	dataset_ = dataset::Info();
	applySets();
}

// Each vehicle's length in feet when the calibration gives the scale (session rows, and csv rows from v1.10's LengthPx), and
// truck-or-bus by that real size: about 24 ft long or more.  A csv row keeps the area test, against the threshold
// retuneLargeVehicles() set from the same calibration.
// A row from a calibration set is sized at that set's scale, the same one its speed is re-timed to, so a run measured
// before a recalibration (or from another view) never shows old-scale speeds beside new-scale sizes: pixels per foot
// scale with the zone's pixels and inversely with its feet.
void App::sizeRows() {
	double ppf = pxPerFtAtLane();
	const calsets::View now = currentView();
	const calsets::Scale nowFt = currentScale();
	const int nowPx = now.lineRight - now.lineLeft;
	for (ResultRow& r : rows) {
		double k = 1;
		if (const calsets::Set* set = setOfRow(r)) {
			calsets::Scale shown = calsets::shownScale(*set, now, nowFt);
			int setPx = set->view.lineRight - set->view.lineLeft;
			if (set->known && shown.valid() && nowFt.valid() && setPx > 0 && nowPx > 0)
				k = double(setPx) / nowPx * nowFt.of(r.l2r) / shown.of(r.l2r);
		}
		r.lengthFt = ppf > 0 && r.widthPx > 0 ? r.widthPx / (ppf * k) : 0;
		if (r.lengthFt > 0) r.large = r.lengthFt >= kLargeVehicleFt;
	}
}

Texture* App::snapshotTexture(int id) {
	auto it = snapTex_.find(id);
	if (it != snapTex_.end()) return it->second.get();
	Vehicle v;
	if (!tracker.vehicle(id, v) || v.snapshot.empty()) return nullptr;
	auto t = std::make_unique<Texture>();
	t->update(v.snapshot);
	Texture* raw = t.get();
	snapTex_[id] = std::move(t);
	if (snapTex_.size() > 300) snapTex_.erase(snapTex_.begin());   // oldest ids first
	return raw;
}

// ---- archived datasets (Archive.h) --------------------------------------------------------------------------

bool App::openArchive(const std::string& dir) {
	archive::Info a;
	std::string err;
	if (!archive::read(dir, a, err)) { toast(err, c::bad, ICON_WARN); return false; }
	rows.clear();
	for (const archive::Vehicle& v : a.vehicles) {
		ResultRow r;
		r.source = v.source;
		r.frame = v.frame;
		r.l2r = v.l2r;
		r.compass = v.compass;
		r.wrongWay = v.wrongWay;   // as judged when it was archived, by the one-way setting of the time
		r.speed = v.speed;
		r.frames = v.frames;
		r.area = v.area;
		r.large = v.large;
		r.flagged = v.flagged;
		r.fps = v.fps > 0 ? v.fps : 30;
		r.time = v.passed;
		r.lengthFt = v.lengthFt;   // feet at the archive's own calibration; sizeRows() isn't run on an archive
		r.trend = v.trend;
		r.entryMph = v.entryMph;
		r.exitMph = v.exitMph;
		r.accel = v.accel;
		r.mphIn20ft = v.mphIn20ft;
		r.calKind = v.ownDrive;
		r.drivenMph = v.drivenMph;
		if (const calsets::Set* set = a.setOf(v)) r.setId = set->id;   // its band, from the archive's own sets
		if (!v.snapshot.empty()) r.snapshotPath = (fs::path(a.dir) / fs::path(v.snapshot)).string();
		if (!v.clip.empty()) r.clipPath = (fs::path(a.dir) / fs::path(v.clip)).string();
		rows.push_back(r);
	}
	archive_ = a;
	// A published dataset is an archive folder with dataset.json beside archive.json: its badge and layers go on top.
	dataset_ = dataset::Info();
	if (dataset::isDataset(a.dir) && !dataset::read(a.dir, dataset_, err)) { toast(err, c::warn, ICON_WARN); dataset_ = dataset::Info(); }
	setFilter_ = 0;
	selectedSet_ = 0;
	rowsScope = ResultsScope::File;   // one dataset; reviewing() tells it apart from a csv
	rowsFrom = a.dir;
	rowsRuns = 0;
	selectedRow_ = -1;
	fileTex_.clear();
	page = Page::Results;
	return true;
}

Texture* App::fileTexture(const std::string& path) {
	auto it = fileTex_.find(path);
	if (it != fileTex_.end()) return it->second.get();   // a null entry: tried, unreadable
	// imdecode of the bytes rather than imread, which can't open non-ASCII paths on Windows.  A snapshot is a still for
	// the detail card, not a frame to track, so it doesn't go through the frame input port.
	std::unique_ptr<Texture> t;
	std::ifstream in(fs::path(path), std::ios::binary);
	std::vector<uchar> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	cv::Mat img = bytes.empty() ? cv::Mat() : cv::imdecode(bytes, cv::IMREAD_COLOR);
	if (!img.empty()) { t = std::make_unique<Texture>(); t->update(img); }
	Texture* raw = t.get();
	fileTex_[path] = std::move(t);
	return raw;
}

// What "Archive" takes from what Results shows: the stats csvs behind the rows (with their manifests and highlights),
// every row with this session's snapshot and clip when it has them, the config and calibration history, and the
// limits and calibration in force, so the archive can be judged and reported on the same way later.
archive::Request App::archiveRequest(const std::string& name, bool move, const std::vector<ResultRow>* from) {
	const Globals& g = cfg.g;
	archive::Request req;
	req.dataDir = dataDir();
	req.move = move;
	archive::Info& i = req.info;
	i.name = name;
	i.created = calib::nowIso();
	i.from = rowsScope == ResultsScope::Saved ? "All saved runs" : rowsScope == ResultsScope::Session ? "This session" : fs::path(rowsFrom).filename().string();
	i.siteName = prefs.siteName;
	i.speedLimit = prefs.speedLimit;
	i.wayOver = prefs.egregious;
	i.vst = VST_VERSION;
	i.configPath = cfg.path();
	i.cal.l2rCompass = g.L2RDirection;
	i.cal.r2lCompass = g.R2LDirection;
	i.cal.legal = g.LegalDirection;
	i.cal.framesL2R = g.CalibrationFramesL2R;
	i.cal.framesR2L = g.CalibrationFramesR2L;
	i.cal.calSpeed = g.CalibrationSpeed;
	i.cal.calFps = g.CalibrationFPS;
	i.cal.minLoggedSpeed = g.minLoggedSpeed;
	i.cal.lines = speedLinesSig();
	req.configFile = cfg.path();
	if (!cfg.path().empty()) req.historyFiles.push_back((fs::path(cfg.path()).parent_path() / "calibration-history.csv").string());
	req.historyFiles.push_back((userpaths::configDir() / "calibration-history.csv").string());
	for (const ResultRow& r : from ? *from : rows) {
		if (!r.statsPath.empty() && std::find(req.statsCsvs.begin(), req.statsCsvs.end(), r.statsPath) == req.statsCsvs.end())
			req.statsCsvs.push_back(r.statsPath);
		archive::Source s;
		archive::Vehicle& v = s.row;
		v.passed = r.time;
		v.speed = r.speed;
		v.l2r = r.l2r;
		v.compass = r.compass;
		v.wrongWay = r.wrongWay;
		v.frames = r.frames;
		v.area = r.area;
		v.frame = r.frame;
		v.large = r.large;
		v.flagged = r.flagged;
		v.fps = r.fps;
		v.source = r.source;
		v.lengthFt = r.lengthFt;
		v.trend = r.trend;
		v.entryMph = r.entryMph;
		v.exitMph = r.exitMph;
		v.accel = r.accel;
		v.mphIn20ft = r.mphIn20ft;
		v.ownDrive = r.calKind;
		v.drivenMph = r.drivenMph;
		v.run = r.statsPath.empty() ? std::string() : fs::path(r.statsPath).filename().string();
		Vehicle tv;
		if (r.vehicleId && tracker.vehicle(r.vehicleId, tv)) { s.snapshot = tv.snapshot; s.clipPath = tv.clipPath; }
		req.vehicles.push_back(std::move(s));
	}
	// The user's own calibration and check drives (write() keeps those among these runs), so the archive still marks them
	// and its street report still leaves them out, wherever it is opened.
	calibrationPass("", 0);   // load calibration-passes.csv
	for (const auto& kv : calPasses_) req.passes.push_back(kv.second);
	// The calibration sets of those runs, frozen at the speeds they are archived with: the street report made from the
	// archive re-times its csvs to these, so it agrees with the archived rows (archiveReportInput).  And what the
	// calibration in force was measured from, so the reviewer can say how these speeds were calibrated.
	if (!reviewing()) {
		req.calInputs = calibrationInputs();
		syncSets();
		calsets::Store frozen;
		for (const calsets::Set& set : sets_.sets) {
			calsets::Set f = set;
			f.runs.clear();
			for (const std::string& csv : req.statsCsvs)
				if (set.has(fs::path(csv).filename().string())) f.runs.push_back(fs::path(csv).filename().string());
			if (f.runs.empty()) continue;
			f.scale = calsets::shownScale(set, currentView(), currentScale());
			if (!f.locked()) { f.state = calsets::State::Locked; f.lockReason = "archived"; f.lockedAt = i.created; }
			frozen.sets.push_back(f);
			frozen.nextId = std::max(frozen.nextId, f.id + 1);
		}
		if (!frozen.sets.empty()) req.extra.push_back({ "Stats/calibration-sets.json", calsets::dump(frozen) });
	}
	return req;
}

// "Archive these results": a name and move-or-copy, then archive::write, then the new archive opens in Results so the
// user sees at once that everything is there.
void App::archiveDialog() {
	ImGui::SetNextWindowSize(ImVec2(px(560), 0), ImGuiCond_Always);   // 0 = fit the height every frame, as options open and close
	if (!ImGui::BeginPopupModal("Archive these results", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
	ArchiveForm& f = archiveForm_;
	int snaps = 0;
	for (const ResultRow& r : rows) snaps += r.vehicleId != 0;
	dimText("Puts these %zu vehicles away in a folder of their own, with the results files, %s, and the setup and calibration "
		"they were measured with. Open them again any time from Open in Results, or double-click review.html in the folder.",
		rows.size(), snaps ? "their snapshots and clips" : "the highlights video");
	ImGui::Dummy(ImVec2(0, px(6)));
	float col = px(110);
	formLabel("Name", col);
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##aname", "e.g. First week, before the new signs", f.name, sizeof f.name);
	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::SetCursorPosX(col);
	if (ImGui::RadioButton("Move them into the archive", f.move)) f.move = true;
	ImGui::SetCursorPosX(col + px(28));
	dimText("Start fresh: the street report and Open stop counting them. You can put them back.");
	ImGui::SetCursorPosX(col);
	if (ImGui::RadioButton("Keep them here too (archive a copy)", !f.move)) f.move = false;
	if (!f.error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad)); ImGui::TextWrapped("%s", f.error.c_str()); ImGui::PopStyleColor(); }
	ImGui::Dummy(ImVec2(0, px(6)));
	if (primaryButton(ICON_ARCHIVE "  Archive")) {
		std::string name = f.name;
		if (name.find_first_not_of(" \t") == std::string::npos) name = "Results " + calib::nowIso().substr(0, 10);
		// From this session or from all saved runs (which include this session's csvs): a move takes the session's
		// files, so the session lets go of its vehicles too.
		bool fromSession = rowsScope != ResultsScope::File;
		std::string dir;
		std::vector<std::string> warnings;
		f.error.clear();
		if (archive::write(archiveRequest(name, f.move), dir, f.error, &warnings)) {
			// Moved out of a session: its clips and csvs are in the archive now, so the session lets go of those vehicles.
			if (f.move && fromSession) { tracker.clearVehicles(); snapTex_.clear(); seenVehicleId_ = 0; }
			ImGui::CloseCurrentPopup();
			openArchive(dir);
			toast("Archived " + std::to_string(rows.size()) + " vehicles as \"" + name + "\"", c::ok, ICON_ARCHIVE);
			for (const std::string& w : warnings) toast(w, c::warn, ICON_WARN);
		}
	}
	ImGui::SameLine();
	if (ghostButton("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// The Open menu: every archive (newest first), then the plain csv and "another folder" for an archive kept elsewhere
// (a USB stick, a neighbour's copy).
void App::openMenu() {
	ImGui::SetNextWindowSizeConstraints(ImVec2(px(360), 0), ImVec2(px(720), px(460)));
	if (!ImGui::BeginPopup("openmenu")) return;
	sectionLabel("ARCHIVES");
	if (archives_.empty()) dimText("None yet. Archive puts the results you're looking at here.");
	for (size_t i = 0; i < archives_.size(); i++) {
		const archive::Info& a = archives_[i];
		ImGui::PushID(int(i));
		std::string label = std::string(ICON_ARCHIVE "  ") + (a.name.empty() ? fs::path(a.dir).filename().string() : a.name);
		if (ImGui::Selectable(label.c_str(), archive_.dir == a.dir)) { openArchive(a.dir); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine(0, px(16));
		std::string when = a.firstPassed.empty() ? "archived " + a.created.substr(0, std::min<size_t>(10, a.created.size()))
			: a.firstPassed.substr(0, 10) + (a.lastPassed.substr(0, 10) != a.firstPassed.substr(0, 10) ? " to " + a.lastPassed.substr(0, 10) : "");
		std::string detail = std::to_string(a.vehicles.size()) + " vehicles · " + when + (a.moved ? "" : " · a copy");
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
		ImGui::TextUnformatted(detail.c_str());   // one line: the menu grows to fit rather than wrapping under the name
		ImGui::PopStyleColor();
		ImGui::PopID();
	}
	// Published datasets (Dataset.h): the badge's words beside each, so a tainted one is seen before it is opened.
	sectionLabel("PUBLISHED");
	if (publishedList_.empty()) dimText("None yet. Publish, in the calibration set bar, freezes a locked set for others to check.");
	for (size_t i = 0; i < publishedList_.size(); i++) {
		const dataset::Info& d = publishedList_[i];
		ImGui::PushID(int(1000 + i));
		std::string label = std::string(ICON_CHECK_CIRCLE "  ") + (d.name.empty() ? d.id : d.name);
		if (ImGui::Selectable(label.c_str(), dataset_.dir == d.dir)) { openArchive(d.dir); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine(0, px(16));
		dataset::Verdict v = d.status();
		ImGui::PushStyleColor(ImGuiCol_Text, v4(v == dataset::Verdict::Tainted ? c::bad : v == dataset::Verdict::Review ? c::warn
			: v == dataset::Verdict::Accurate ? c::ok : c::dim));
		std::string detail = std::to_string(d.summary.cars) + " vehicles · published " + d.published.substr(0, std::min<size_t>(10, d.published.size()))
			+ " · " + dataset::verdictLabel(v);
		ImGui::TextUnformatted(detail.c_str());
		ImGui::PopStyleColor();
		ImGui::PopID();
	}
	ImGui::Separator();
	if (ImGui::Selectable(ICON_FILE_CSV "  A stats csv…")) {
		ImGui::CloseCurrentPopup();
		auto r = pfd::open_file("Open stats csv", (fs::path(dataDir()) / "Stats").string(), { "Stats csv", "*.csv", "All files", "*" }).result();
		if (!r.empty()) loadResultsCsv(r[0]);
	}
	if (ImGui::Selectable(ICON_FOLDER_OPEN "  An archive or published folder somewhere else…")) {
		ImGui::CloseCurrentPopup();
		std::string r = pfd::select_folder("Open an archive or published dataset folder", (fs::path(dataDir()) / "Archive").string()).result();
		if (!r.empty()) openArchive(r);
	}
	ImGui::EndPopup();
}

// Over an open archive: what it is, that it's read-only, and the two things to do with it besides looking.
void App::reviewBanner(float width) {
	const archive::Info& a = archive_;
	// A published dataset (dataset.json beside archive.json) says "published", with its own icon; the rest is shared.
	const bool isPub = !dataset_.dir.empty();
	beginCard("review", isPub ? ICON_PUBLISH : ICON_ARCHIVE, a.name.empty() ? (isPub ? "Published dataset" : "Archive") : a.name.c_str(), ImVec2(width, 0));
	std::string what = std::string(isPub ? "Read-only · published " : "Read-only · archived ") + a.created.substr(0, std::min<size_t>(10, a.created.size())) + " from "
		+ (a.from == "This session" ? std::string("this session") : a.from) + " · judged at " + std::to_string(a.speedLimit) + " mph";
	if (!a.siteName.empty() && a.siteName != report::Settings().siteName) what = a.siteName + " · " + what;
	if (a.moved) what += " · moved out of your data";
	dimText("%s", what.c_str());
	if (a.moved) {
		if (ghostButton(ICON_UNDO "  Put back into my data")) {
			std::string msg;
			syncSets(true);   // sets_ as they stand before the runs come back
			int passesBack = 0;
			if (archive::restore(a.dir, dataDir(), msg, screenshotMode ? std::string() : calib::passesPath(), &passesBack)) {
				archive_.moved = false;
				// Drives the config folder didn't have (an archive from another computer) are in its file now: read it again.
				if (passesBack) { calPasses_.clear(); calPassesLoaded_ = false; }
				// Its runs come back in the calibration sets they were archived in, locked at the speeds they had, rather
				// than being filed afresh (which could re-time them to today's calibration).
				for (calsets::Set set : calsets::load((fs::path(a.dir) / "Stats").string()).sets) {
					bool filed = false;
					for (const std::string& r : set.runs) filed |= sets_.ofRun(r) != nullptr;
					if (filed || set.runs.empty()) continue;
					// Its old number when that is free (numbers are never reused, so it is this set coming home, and the
					// archive named "Set 2, ..." puts back Set 2); otherwise a new one.
					if (set.id <= 0 || sets_.find(set.id)) set.id = sets_.nextId;
					sets_.nextId = std::max(sets_.nextId, set.id + 1);
					set.state = calsets::State::Locked;
					set.published.clear();
					sets_.sets.push_back(set);
				}
				std::sort(sets_.sets.begin(), sets_.sets.end(), [](const calsets::Set& x, const calsets::Set& y) { return x.id < y.id; });
				std::string err;
				calsets::save(setsFor_, sets_, err);
				syncSets(true);
				applySetsDue_ = true;
				toast("Put back: the street report and Open count these runs again. The archive stays as it was.", c::ok, ICON_UNDO);
				if (!msg.empty()) toast(msg, c::warn, ICON_WARN);
			}
			else toast(msg, c::bad, ICON_WARN);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies the results files back into your data folder. The archive keeps its own copy.");
		ImGui::SameLine();
	}
	if (ghostButton(ICON_EXTERNAL "  Open in browser")) openWithSystem((fs::path(a.dir) / "review.html").string());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("review.html: the same table, snapshots and clips in any web browser, no install needed.");
	ImGui::SameLine();
	if (ghostButton(dataset_.dir.empty() ? ICON_XMARK "  Close archive" : ICON_XMARK "  Close")) showSessionResults();
	archiveCalibration();
	endCard();
}

// "How these speeds were calibrated", under the review banner: the zone the archived calibration measured, the
// calibration sets (each row's band), the user's own drives (left out of the counts), and the measurements behind
// it, from the archive's own files (archive::Info sets, passes and inputs).  review.html shows the same.
void App::archiveCalibration() {
	const archive::Info& a = archive_;
	char head[96];
	snprintf(head, sizeof head, ICON_RULER "  How these speeds were calibrated%s###archcal",
		a.passes.empty() ? "" : a.passes.size() == 1 ? "  \xC2\xB7  1 drive was you" : ("  \xC2\xB7  " + std::to_string(a.passes.size()) + " drives were you").c_str());
	if (archiveCalOpen_) { ImGui::SetNextItemOpen(true); archiveCalOpen_ = false; }   // the --shots tour opens it
	if (!ImGui::TreeNode(head)) return;
	ImGui::PushTextWrapPos(0);
	const archive::Calibration& k = a.cal;
	double l2r = calib::zoneFtForFrames(k.framesL2R, k.calSpeed, k.calFps), r2l = calib::zoneFtForFrames(k.framesR2L, k.calSpeed, k.calFps);
	// The calibration in force when archived; a locked set's speeds keep the zone it was locked at, listed with it.
	char zone[96];
	if (std::fabs(l2r - r2l) < 0.05) snprintf(zone, sizeof zone, "%.1f ft apart", l2r);
	else snprintf(zone, sizeof zone, "%.1f ft apart left to right and %.1f ft right to left", l2r, r2l);
	dimText("The camera times each car between the two speed lines, and a speed is the distance between them over that time. "
		"The calibration in force when this was archived put them %s.%s", zone,
		a.sets.sets.empty() ? "" : " Each calibration set below shows the distance its own speeds use.");
	if (!a.sets.sets.empty()) {
		sectionLabel("CALIBRATION SETS");
		for (const calsets::Set& s : a.sets.sets) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			float h = ImGui::GetTextLineHeight();
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + px(2)), ImVec2(p.x + px(5), p.y + h - px(2)), setColor(s.id));
			ImGui::SetCursorScreenPos(ImVec2(p.x + px(12), p.y));
			// describe() already says "locked" or "calibration not on record"; this adds why, in words.
			const char* why = !s.known ? "" : !s.locked() ? "open: it followed the latest calibration until archived"
				: s.lockReason == "view" ? "the camera view changed after it" : s.lockReason == "archived" ? "when it was archived"
				: s.lockReason == "manual" ? "you locked it" : "";
			ImGui::Text("%s", calsets::describe(s, s.scale).c_str());
			if (*why) { ImGui::SameLine(); dimText("(%s)", why); }
		}
	}
	sectionLabel("YOUR OWN DRIVES");
	if (a.passes.empty())
		dimText(a.hasPasses ? "None of these passes was marked as your own calibration or check drive."
			: "This archive was made before VST kept your calibration drives with it. Rows already marked as yours still say so.");
	else {
		dimText("Passes you drove yourself to calibrate or check the camera. They are marked in the table and left out of the counts and the street report.");
		for (const CalPass& p : a.passes) {
			// What the camera read: the archived row of that pass, when the pass was fast enough to be logged.
			int read = 0;
			for (const archive::Vehicle& v : a.vehicles) if (v.run == p.stats && v.frame == p.frame) read = v.speed;
			char camera[48];
			if (read) snprintf(camera, sizeof camera, "camera read %d mph", read);
			else snprintf(camera, sizeof camera, "under the logging speed");
			ImGui::Text("%s  %s drive %s, you drove %g mph", p.check ? ICON_BULLSEYE : ICON_RULER, p.check ? "Check" : "Calibration",
				p.l2r ? "\xE2\x86\x92" : "\xE2\x86\x90", p.drivenMph);
			ImGui::SameLine();
			std::string when = p.time;
			std::replace(when.begin(), when.end(), 'T', ' ');
			dimText("%s%s%s", camera, when.empty() ? "" : "  \xC2\xB7  ", when.c_str());
		}
	}
	if (!a.inputs.empty()) {
		sectionLabel("MEASURED FROM");
		std::string l2rSays = archive::calibrationSummary(a, true), r2lSays = archive::calibrationSummary(a, false);
		dimText("The calibration is the weighted middle of these, the more careful ones counting for more. %s.",
			l2rSays == r2lSays ? l2rSays.c_str() : ("Left to right: " + l2rSays + ". Right to left: " + r2lSays).c_str());
		for (const calib::Input& i : a.inputs) {
			std::string role = archive::inputRole(a, i);
			char line[320];
			snprintf(line, sizeof line, "%s: %.1f ft, \xC2\xB1%.1f%%%s%s%s", i.label.empty() ? calib::methodName(i.method) : i.label.c_str(), i.zoneFt,
				i.sigma * 100, i.dirs == calib::kL2R ? ", left to right" : i.dirs == calib::kR2L ? ", right to left" : "",
				role.empty() ? "" : "  \xC2\xB7  ", role.c_str());
			if (role.find("not counted") != std::string::npos) dimText("%s", line);
			else ImGui::TextUnformatted(line);
		}
	}
	ImGui::PopTextWrapPos();
	ImGui::TreePop();
}

// Pulls new frames and vehicles from the tracker each GUI frame: updates textures, raises toasts for the
// vehicles worth calling out, and refreshes the session results when a run ends.
//
// One feed for every page.  The engine's capture is the only reader of the camera during a run (Player lets go in
// startTracking), and the tracker already hands the GUI a copy of its newest frame (Tracker::onFrame, at most ~40 a
// second).  That copy goes to whichever page is on screen: Live tracking's own texture, or, on Pick your spot,
// Connect the camera, Mark your street and Calibrate, the Player via feed(), so those pages keep the running picture
// with no second capture, no reopen and nothing that waits.  Each new frame is uploaded once, only for the page that
// draws it, so moving between pages costs no more than staying on Live.  When the run ends the source is opened
// again in the background (Player::open never waits), so a camera keeps showing after Stop as well.
void App::pumpTracker() {
	bool run = tracker.running();
	tracker.latest(live_, prefs.showMotion);
	if (page == Page::Track && live_.serial > 0 && liveTexSerial_ != live_.serial) {
		liveTexSerial_ = live_.serial;
		liveTex_.update(live_.frame);
		if (prefs.showMotion && !live_.motion.empty()) {
			// Cyan where the frame difference shows motion, transparent elsewhere.
			cv::Mat tint(live_.motion.size(), CV_8UC4, cv::Scalar(238, 211, 34, 0));
			std::vector<cv::Mat> ch;
			cv::split(tint, ch);
			ch[3] = live_.motion * 0.55;
			cv::merge(ch, tint);
			motionTex_.update(tint);
		}
	}
	else if (run && showsSharedFeed() && live_.serial > 0 && fedSerial_ != live_.serial) {
		fedSerial_ = live_.serial;
		// What is being tracked: the preview's own facts when it is the same source, else (a later video of a batch)
		// what the tracker says about it.
		Tracker::Progress prog = tracker.progress();
		std::vector<Job> jobs = tracker.jobs();
		SourceInfo about = player.info();
		if (prog.job >= 0 && prog.job < int(jobs.size()) && jobs[prog.job].input != about.path) {
			about = SourceInfo();
			about.path = jobs[prog.job].input;
			about.kind = classifySource(about.path);
			about.label = friendlySourceName(about.path);
			about.fps = prog.videoFps;
			about.frames = prog.totalFrames;
		}
		player.feed(live_.frame, runLevel_, about, live_.frameNumber);
	}
	for (const Vehicle& v : tracker.vehiclesSince(seenVehicleId_)) {
		seenVehicleId_ = v.id;
		if (v.ev.wrongWay) toast("Wrong-way driver, " + std::to_string(v.ev.speed) + " mph", c::wrong, ICON_WARN);
		else if (v.egregious) toast(std::to_string(v.ev.speed) + " mph " + (v.ev.leftToRight ? "→" : "←") + "  well over the limit", c::bad, ICON_BOLT);
		if (rowsScope == ResultsScope::Session) showSessionResults();
	}
	if (restartPending_) {
		// A calibration was applied during a live run: stop it (its csv keeps the calibration it was measured with, which
		// its calibration set re-times, CalSets.h) and start a fresh one with the new value, staying on the page the user is on.
		restartPending_ = false;
		if (tracker.running() && tracker.progress().live) {
			Page was = page;
			stopTracking();
			wasRunning_ = false;
			startTracking();
			page = was;
			toast("Tracking restarted so the cars from now on are measured with the new calibration", c::ok, ICON_PLAY);
			return;
		}
	}
	if (wasRunning_ && !tracker.running()) {
		wasRunning_ = false;
		auto jobs = tracker.jobs();
		int failed = int(std::count_if(jobs.begin(), jobs.end(), [](const Job& j) { return j.state == Job::Failed; }));
		if (failed) toast(std::to_string(failed) + " video(s) failed: " + jobs.back().message, c::bad, ICON_WARN);
		else toast("Tracking finished: " + std::to_string(tracker.counts().vehicles) + " vehicles this session", c::ok, ICON_FLAG);
		showSessionResults();
		// Back to the preview's own reader: the file at its start, the photo, or the camera or stream reconnecting in the
		// background.
		player.close();
		if (!source.empty()) player.open(source);
	}
}

// The pages whose picture is the Player's: the planner's "cars across the picture" (Pick your spot), Connect the
// camera, Mark your street and Calibrate.  Results and Companion show none, so they get no frames.
bool App::showsSharedFeed() const {
	return page == Page::Guide || page == Page::Source || page == Page::Zones || page == Page::Calibrate;
}

const char* App::feedHint(const char* idle) const {
	return tracker.running() ? "Waiting for Live tracking's first frame…" : idle;
}

// A chip on the picture while it is Live tracking's feed: whose picture it is, and that the run keeps the settings it
// started with (startTracking runs a copy), so nobody expects a dragged line to change the speeds being logged now.
void App::sharedFeedBadge(const View& v) {
	if (!player.fed() || !tracker.running()) return;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// The same words and colours as the Live tracking page's own badge.
	Tracker::Progress prog = tracker.progress();
	const char* badge = prog.reconnecting ? "RECONNECTING…" : prog.live ? "● LIVE" : tracker.paused() ? "PAUSED" : "● PLAYING";
	ImU32 bc = prog.reconnecting ? c::warn : prog.live ? c::bad : tracker.paused() ? c::warn : c::ok;
	const char* note = "Live tracking's feed  ·  changes here apply the next time you start tracking";
	ImVec2 p = v.origin + ImVec2(px(14), v.size.y - px(14) - fsz(fonts.strong) - px(10));
	ImVec2 bs = fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, badge);
	dl->AddRectFilled(p, p + bs + ImVec2(px(18), px(10)), alpha(c::bg0, .78f), px(7));
	dl->AddText(fonts.strong, fsz(fonts.strong), p + ImVec2(px(9), px(5)), bc, badge);
	ImVec2 ip = p + ImVec2(bs.x + px(26), 0);
	ImVec2 is = fonts.body->CalcTextSizeA(fsz(fonts.body), FLT_MAX, 0, note);
	if (ip.x + is.x + px(18) > v.origin.x + v.size.x - px(14)) return;   // too narrow for the note: the badge says enough
	dl->AddRectFilled(ip, ip + is + ImVec2(px(18), px(10)), alpha(c::bg0, .78f), px(7));
	dl->AddText(fonts.body, fsz(fonts.body), ip + ImVec2(px(9), px(5)), c::text, note);
}

// ======================================================================================================
// Frame
// ======================================================================================================

void App::frame(float w, float h) {
	// API requests that need App state run here, between frames, on the GUI thread.
	{
		std::deque<std::pair<std::function<std::string()>, std::shared_ptr<std::promise<std::string>>>> work;
		{ std::lock_guard<std::mutex> l(postMu_); work.swap(posted_); }
		for (auto& [fn, p] : work) p->set_value(fn());
	}
	// The preview shows the picture turned as the tracker will see it; during a run this re-turns Live tracking's frame
	// by however far the slider has moved from the angle the run started with (Player::feed).
	player.setLevel(cfg.g.levelDegrees);
	pumpTracker();
	healthTick();
	if (!tracker.running()) {
		// Never waits: a live source's frames come from its reader thread.  A connection that fails, or a picture that
		// is lost, ends here: say why and drop the source, so nothing stale stays on screen.
		bool live = player.opening() || (player.isOpen() && player.info().kind != SourceInfo::File && player.info().kind != SourceInfo::Image);
		player.tick(now());
		if (live && !player.opening() && !player.isOpen() && !player.error.empty()) {
			toast(player.error, c::bad, ICON_WARN);
			// The camera from last time didn't answer (unplugged, asleep, busy in another app).  prefs.lastSource keeps
			// it, so steps 3 and 4 stay checked and Recent still lists it; say that nothing was lost.
			if (!resumedSource_.empty() && resumedSource_ == source)
				toast("Couldn't reconnect " + friendlySourceName(source) + " from last time. Your setup and results are safe: "
					"plug it in, then pick it under Recent on Connect the camera.", c::warn, ICON_CAMERA);
			resumedSource_.clear();
			source.clear();
			pendingRecent_.clear();
		}
		if (!resumedSource_.empty() && player.isOpen() && !player.opening()) resumedSource_.clear();
		if (!pendingRecent_.empty() && pendingRecent_ == source && player.isOpen() && !player.opening()) rememberSource(source);
		else if (!pendingRecent_.empty() && pendingRecent_ != source) pendingRecent_.clear();   // cancelled or replaced
	}
	if (page == Page::Zones && !source.empty() && cfg.problems().empty()) {
		prefs.zonesFor = source;
		if (player.isOpen() && player.info().width > 0) prefs.zonesFrame = std::to_string(player.info().width) + "x" + std::to_string(player.info().height);
	}
	if (!screenshotMode) prefs.lastPage = pageKey(page, guideStep_);   // written with the other prefs (savePrefsSoon)
	autosave();
	// Pick your spot's draft: follow the saved plan where untouched, save it as it's edited when autosave is on (once no
	// control is being dragged or typed in), and say so when the page is left with changes unsaved.
	planSync();
	if (prefs.planAutosave && planDirty() && !ImGui::IsAnyItemActive()) planCommit(true);
	bool planShown = page == Page::Guide && guideStep_ == 1;
	if (planWasShown_ && !planShown && planDirty())
		toast("Pick your spot has unsaved changes. They're kept as a preview until you save or discard them there.", c::warn, ICON_WARN);
	planWasShown_ = planShown;
	savePrefsSoon();

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar(2);

	float railW = px(236);
	drawRail(h);
	ImGui::SameLine(0, 0);
	ImGui::BeginGroup();
	drawHeader(w - railW);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(18)));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(c::bg0));
	// One child per page (and per Guide step), so each keeps its own scroll position when you leave and come back.
	char pageId[24];
	snprintf(pageId, sizeof pageId, "page%d_%d", int(page), page == Page::Guide ? guideStep_ : 0);
	ImGui::BeginChild(pageId, ImVec2(w - railW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, page == Page::Guide || page == Page::Source || page == Page::Companion ? 0 : ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleColor();
	switch (page) {
	case Page::Guide: pageGuide(); break;
	case Page::Source: pageSource(); break;
	case Page::Zones: pageZones(); break;
	case Page::Calibrate: pageCalibrate(); break;
	case Page::Track: pageTrack(); break;
	case Page::Results: pageResults(); break;
	case Page::Companion: pageCompanion(); break;
	default: break;
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::EndGroup();
	drawToasts(w, h);
	ImGui::End();
}

// ======================================================================================================
// The setup flow
// ======================================================================================================

int App::flowStep() const {
	switch (page) {
	case Page::Guide: return std::clamp(guideStep_, 0, 1);
	case Page::Source: return 2;
	case Page::Zones: return 3;
	case Page::Calibrate: return 4;
	default: return -1;
	}
}

// A step is done when what it's for is in place: the first two once the user has moved past them (they only inform),
// the rest by the same tests that let a run start and that mark a calibration current.
bool App::flowDone(int step) {
	switch (step) {
	case 0: return prefs.guideDone || (page == Page::Guide && guideStep_ > 0);
	case 1: return prefs.planConfirmed;   // saved at least once (a rough guess counts)
	// Connected (not still connecting), queued, or connected before and not disconnected since: the step was done,
	// and the header's chip says when the camera isn't answering right now.
	case 2: return resume::sourceStepDone(!source.empty() && !player.opening(), !batch.empty(), prefs.lastSource);
	case 3: return zonesDone();
	case 4: return calibrationDone() && !(sampleCache_.size() && source == sampleCache_);   // the sample's own calibration isn't yours
	default: return false;
	}
}

void App::goFlow(int step) {
	step = std::clamp(step, 0, kFlowSteps - 1);
	if (step >= 2) prefs.guideDone = true;   // past the Guide: open on Connect the camera next time
	page = kFlow[step].page;
	if (kFlow[step].guideStep >= 0) guideStep_ = kFlow[step].guideStep;
}

// The flow as a row of numbered circles joined by a line, the current step filled, done steps checked.  Drawn at the
// top of every setup page, with the same numbers and names as the rail; clicking a step goes there.
void App::flowStepper() {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float avail = ImGui::GetContentRegionAvail().x;
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = avail / kFlowSteps, h = px(52);
	int here = flowStep();
	for (int i = 0; i < kFlowSteps; i++) {
		ImVec2 c0 = p + ImVec2(w * i + px(14), px(14));
		bool done = flowDone(i), cur = i == here;
		if (i < kFlowSteps - 1) dl->AddLine(c0 + ImVec2(px(16), 0), ImVec2(p.x + w * (i + 1) + px(2), c0.y), done ? c::accent : c::border, 2);
		dl->AddCircleFilled(c0, px(12), cur ? c::accent : done ? alpha(c::accent, .25f) : c::bg3);
		std::string num = done && !cur ? ICON_CHECK : std::to_string(i + 1);
		ImFont* f = fonts.caption;
		ImVec2 ts = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, num.c_str());
		dl->AddText(f, fsz(f), c0 - ts * .5f, cur ? c::rgb(0xFFFFFF) : done ? c::accent : c::dim, num.c_str());
		dl->AddText(cur ? fonts.strong : fonts.body, fsz(fonts.body), c0 + ImVec2(-px(12), px(18)), cur ? c::text : c::dim, kFlow[i].name);
		ImGui::SetCursorScreenPos(p + ImVec2(w * i, 0));
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("flowstep", ImVec2(w, h))) goFlow(i);
		if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::PopID();
	}
	ImGui::SetCursorScreenPos(p + ImVec2(0, h + px(14)));
}

// Side rail: logo, the workflow as numbered steps, and a status block at the bottom.
void App::drawRail(float h) {
	float railW = px(236);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(c::bg1));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(18)));
	ImGui::BeginChild("rail", ImVec2(railW, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
	dl->AddLine(ImVec2(wp.x + railW - 1, wp.y), ImVec2(wp.x + railW - 1, wp.y + h), c::border);

	// Logo: a speedometer arc with the needle in the red.
	{
		ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(px(6), 0);
		float r = px(19);
		ImVec2 cen = p + ImVec2(r, r + px(4));
		dl->AddCircleFilled(cen, r + px(3), c::bg3);
		dl->PathArcTo(cen, r - px(3), IM_PI * .8f, IM_PI * 1.55f, 16);
		dl->PathStroke(c::ok, 0, px(3.5f));
		dl->PathArcTo(cen, r - px(3), IM_PI * 1.6f, IM_PI * 1.95f, 12);
		dl->PathStroke(c::warn, 0, px(3.5f));
		dl->PathArcTo(cen, r - px(3), IM_PI * 2.0f, IM_PI * 2.2f, 8);
		dl->PathStroke(c::bad, 0, px(3.5f));
		float a = IM_PI * 2.05f;
		dl->AddLine(cen, cen + ImVec2(std::cos(a), std::sin(a)) * (r - px(6)), c::text, px(2.5f));
		dl->AddCircleFilled(cen, px(3), c::text);
		ImGui::SetCursorScreenPos(p + ImVec2(r * 2 + px(12), px(2)));
		ImGui::BeginGroup();
		ImGui::PushFont(fonts.title);
		ImGui::TextUnformatted("VST Studio");
		ImGui::PopFont();
		ImGui::PushFont(fonts.caption);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
		ImGui::TextUnformatted("Video Speed Tracker");
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::EndGroup();
	}
	ImGui::Dummy(ImVec2(0, px(18)));

	auto group = [&](const char* label) {
		ImGui::Dummy(ImVec2(0, px(6)));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(10));
		sectionLabel(label);
	};
	// One row: icon, label, and for a setup step its number (a check once it's done).
	// `unsaved` swaps the number for an amber dot (Pick your spot with a draft), with a tooltip saying what it means.
	auto row = [&](int id, const char* icon, const char* label, bool sel, int number, bool done, bool unsaved = false) -> bool {
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float iw = ImGui::GetContentRegionAvail().x, ih = px(40);
		ImGui::PushID(id);
		bool clicked = ImGui::InvisibleButton("nav", ImVec2(iw, ih));
		bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		if (sel) {
			dl->AddRectFilled(pos, pos + ImVec2(iw, ih), alpha(c::accent, .16f), px(8));
			dl->AddRectFilled(pos + ImVec2(0, px(9)), pos + ImVec2(px(3), ih - px(9)), c::accent, px(2));
		}
		else if (hov) dl->AddRectFilled(pos, pos + ImVec2(iw, ih), alpha(c::text, .05f), px(8));
		ImFont* f = sel ? fonts.strong : fonts.body;
		ImU32 col = sel ? c::text : (hov ? c::text : c::dim);
		dl->AddText(f, fsz(f), pos + ImVec2(px(14), (ih - fsz(f)) * .5f), sel ? c::accent : col, icon);
		dl->AddText(f, fsz(f), pos + ImVec2(px(44), (ih - fsz(f)) * .5f), col, label);
		if (unsaved) {
			ImVec2 cc = pos + ImVec2(iw - px(18), ih * .5f);
			dl->AddCircleFilled(cc, px(9), alpha(c::warn, .25f));
			dl->AddCircleFilled(cc, px(4), c::warn);
			ImGui::PushID(id);
			if (hov) ImGui::SetTooltip("Unsaved changes: they're a preview until you save them on this page.");
			ImGui::PopID();
		}
		else if (number > 0) {
			ImVec2 cc = pos + ImVec2(iw - px(18), ih * .5f);
			ImFont* sf = fonts.caption;
			const std::string n = done ? std::string(ICON_CHECK) : std::to_string(number);
			ImVec2 ts = sf->CalcTextSizeA(fsz(sf), FLT_MAX, 0, n.c_str());
			if (done) dl->AddCircleFilled(cc, px(9), alpha(c::ok, .2f));
			else dl->AddCircle(cc, px(9), sel ? c::accent : c::faint, 0, 1.5f);
			dl->AddText(sf, fsz(sf), cc - ts * .5f, done ? c::ok : sel ? c::accent : c::faint, n.c_str());
		}
		return clicked;
	};
	auto item = [&](Page p) {
		if (row(int(p), pageIcon(p), pageTitle(p), page == p, 0, false)) page = p;
	};
	group("SET UP");
	int here = flowStep();
	for (int i = 0; i < kFlowSteps; i++)
		if (row(100 + i, kFlow[i].icon, kFlow[i].name, here == i, i + 1, flowDone(i), i == 1 && planDirty())) goFlow(i);
	group("RUN");
	item(Page::Track);
	item(Page::Results);
	group("SHARE");
	item(Page::Companion);

	// Status block pinned to the bottom.
	float bh = px(86);
	ImGui::SetCursorPosY(h - bh - px(14));
	ImVec2 p = ImGui::GetCursorScreenPos();
	float bw = ImGui::GetContentRegionAvail().x;
	dl->AddRectFilled(p, p + ImVec2(bw, bh), c::bg2, px(10));
	bool run = tracker.running();
	ImU32 dot = run ? (tracker.paused() ? c::warn : c::ok) : c::faint;
	float pulse = run && !tracker.paused() ? .5f + .5f * std::sin(float(now()) * 4) : 1;
	dl->AddCircleFilled(p + ImVec2(px(16), px(20)), px(7), alpha(dot, .25f * pulse));
	dl->AddCircleFilled(p + ImVec2(px(16), px(20)), px(4), dot);
	const char* st = run ? (tracker.paused() ? "Paused" : "Tracking") : "Idle";
	dl->AddText(fonts.strong, fsz(fonts.strong), p + ImVec2(px(30), px(11)), c::text, st);
	std::string cfgName = cfg.path().empty() ? "no config" : fs::path(cfg.path()).filename().string();
	// Settings save themselves, so the only states worth showing are "saving", "can't save yet" and "saved".
	bool broken = !cfg.path().empty() && !cfg.problems().empty();
	const char* cfgState = cfg.path().empty() ? "" : broken ? "  needs fixing" : cfg.dirty() && autosaveAt_ > 0 ? "  saving…"
		: (savedAt_ > 0 && now() - savedAt_ < 3) ? "  saved " ICON_CHECK : "  saved";
	std::string l1 = std::string(ICON_GEAR "  ") + cfgName + cfgState;
	dl->AddText(fonts.caption, fsz(fonts.caption), p + ImVec2(px(12), px(38)), broken ? c::bad : c::dim, l1.c_str());
	std::string l2 = std::string(ICON_BROADCAST "  companion ") + (remoteOn() ? "on" : localServer_ ? "off (map open)" : "off");
	dl->AddText(fonts.caption, fsz(fonts.caption), p + ImVec2(px(12), px(60)), remoteOn() ? c::accent2 : c::faint, l2.c_str());
	ImGui::EndChild();
}

// Header: page title and subtitle on the left; the source chip and the run controls on the right, so tracking
// can be started or stopped from any page.
void App::drawHeader(float w) {
	float hh = px(76);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(c::bg0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(14)));
	ImGui::BeginChild("header", ImVec2(w, hh), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
	dl->AddLine(ImVec2(wp.x, wp.y + hh - 1), ImVec2(wp.x + w, wp.y + hh - 1), c::border);

	ImGui::BeginGroup();
	ImGui::PushFont(fonts.title);
	int here = flowStep();
	ImGui::TextUnformatted(here >= 0 ? kFlow[here].name : pageTitle(page));
	ImGui::PopFont();
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextUnformatted(here >= 0 ? kFlow[here].subtitle : pageSubtitle(page));
	ImGui::PopStyleColor();
	ImGui::EndGroup();

	// Right-aligned controls, laid out from the right edge.
	bool run = tracker.running();
	// The main button walks the setup flow.  On a setup step it goes to the step after it, by the same name the rail
	// and the stepper use, until steps 3-5 are all checked (a source, zones checked on it, a calibration); then it
	// starts tracking.  Off the flow (Live, Results, Companion) it goes to the first step a run can't do without,
	// a source or valid zones.  A missing calibration doesn't block a run (results are still useful relative to
	// each other); it shows as an amber chip that opens Calibrate.
	bool haveSource = !source.empty() || !batch.empty();
	bool runnable = haveSource && cfg.problems().empty();
	int nextStep = -1;
	if (!run) {
		// Setup walks step by step to Calibrate; only there (or off the flow) does the button start a run.
		if (here >= 0 && here < kFlowSteps - 1) nextStep = here + 1;
		else if (!haveSource) nextStep = 2;
		else if (!cfg.problems().empty()) nextStep = 3;
	}
	std::string runText = run ? ICON_STOP "  Stop" : ICON_PLAY "  Start tracking";
	if (nextStep >= 0)
		runText = std::string(here > nextStep ? "Back to: " : "Next: ") + kFlow[nextStep].name + "  " + (here > nextStep ? ICON_ARROW_LEFT : ICON_ARROW_RIGHT);
	const char* runLabel = runText.c_str();
	bool calNag = !run && nextStep < 0 && !calibrationDone() && page != Page::Calibrate;
	const char* calText = calibrationStale() ? ICON_GAUGE "  Speed lines moved: calibrate again" : ICON_GAUGE "  Not calibrated yet";
	ImVec2 calSize = calNag ? pillSize(calText, fonts.body) : ImVec2(0, 0);
	float bw = ImGui::CalcTextSize(runLabel).x + px(30);
	float pauseW = run ? ImGui::GetFrameHeight() + px(10) : 0;
	// Unplugged since last time, or a camera that just failed to open: name the remembered source rather than
	// "No source", which reads as forgotten.  A camera or stream is "not connected"; a video file (the sample, say,
	// after a camera attempt replaced it) is just "closed", since nothing about it is broken.
	const std::string& shown = !source.empty() ? source : prefs.lastSource;
	SourceInfo::Kind shownKind = shown.empty() ? SourceInfo::None : classifySource(shown);
	bool live = shownKind == SourceInfo::Camera || shownKind == SourceInfo::Stream;
	std::string chip = !source.empty() ? shortSourceName(source, 36)
		: !shown.empty() ? shortSourceName(shown, 28) + (live ? " · not connected" : " · closed") : std::string("No source");
	if (player.opening()) chip = "Connecting to " + chip + "…";
	if (batch.size() > 1) chip = std::to_string(batch.size()) + " videos queued";
	const char* chipIcon = shown.empty() ? ICON_VIDEO : shownKind == SourceInfo::Camera ? ICON_CAMERA
		: shownKind == SourceInfo::Stream ? ICON_BROADCAST : ICON_FILE_VIDEO;
	std::string chipText = std::string(chipIcon) + "  " + chip;
	ImVec2 cs = pillSize(chipText.c_str(), fonts.body);
	float right = w - px(24);
	float y = (hh - px(40)) * .5f;
	ImGui::SetCursorPos(ImVec2(right - bw, y));
	if (primaryButton(runLabel, ImVec2(bw, px(40)), run ? c::bad : c::accent)) {
		if (run) stopTracking();
		else if (nextStep >= 0) goFlow(nextStep);
		else startTracking();
	}
	if (run) {
		ImGui::SetCursorPos(ImVec2(right - bw - pauseW, y + (px(40) - ImGui::GetFrameHeight()) * .5f));
		if (iconButton(tracker.paused() ? ICON_PLAY : ICON_PAUSE, tracker.paused() ? "Resume" : "Pause", tracker.paused()))
			tracker.setPaused(!tracker.paused());
	}
	ImGui::SetCursorPos(ImVec2(right - bw - pauseW - cs.x - px(14), (hh - cs.y) * .5f));
	pill(chipText.c_str(), source.empty() ? c::dim : c::text, c::bg2, fonts.body);
	if (ImGui::IsItemClicked()) page = Page::Source;
	// While tracking, the same slot warns when the camera seems to have been knocked (AppHealth.cpp).
	if (!calNag && run && health.cameraWarning) {
		const char* moved = ICON_WARN "  Camera moved?";
		ImVec2 ms = pillSize(moved, fonts.body);
		ImGui::SetCursorPos(ImVec2(right - bw - pauseW - cs.x - ms.x - px(24), (hh - ms.y) * .5f));
		pill(moved, c::bad, alpha(c::bad, .14f), fonts.body);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("The view no longer lines up with the one the zones were drawn on.\nClick for details.");
		if (ImGui::IsItemClicked()) page = Page::Track;
	}
	if (calNag) {
		ImGui::SetCursorPos(ImVec2(right - bw - pauseW - cs.x - calSize.x - px(24), (hh - calSize.y) * .5f));
		pill(calText, c::warn, alpha(c::warn, .14f), fonts.body);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speeds use the calibration in %s, which wasn't made for this camera.\nClick to calibrate: one drive past, or no car at all with cones or a pole.",
			cfg.path().empty() ? "VST.cfg" : fs::path(cfg.path()).filename().string().c_str());
		if (ImGui::IsItemClicked()) page = Page::Calibrate;
	}
	ImGui::EndChild();
}

void App::drawToasts(float w, float h) {
	double t = now();
	while (!toasts_.empty() && toasts_.front().until < t) toasts_.pop_front();
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	float y = px(88);
	for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
		// The icon gets its own column, so a message that wraps lines up under its first word.
		const char* icon = it->icon ? it->icon : ICON_INFO;
		ImFont* f = fonts.strong;
		float iconW = px(26);
		ImVec2 ts = f->CalcTextSizeA(fsz(f), px(520), px(520), it->text.c_str());
		ImVec2 size = ts + ImVec2(px(36) + iconW, px(24));
		float fade = float(std::min(1.0, (it->until - t) / .4));
		ImVec2 p(w - size.x - px(24), y);
		dl->AddRectFilled(p + ImVec2(0, px(4)), p + size + ImVec2(0, px(4)), IM_COL32(0, 0, 0, int(90 * fade)), px(10));
		dl->AddRectFilled(p, p + size, alpha(c::bg3, .98f * fade), px(10));
		dl->AddRectFilled(p, p + ImVec2(px(4), size.y), alpha(it->color, fade), px(10), ImDrawFlags_RoundCornersLeft);
		dl->AddText(f, fsz(f), p + ImVec2(px(20), px(12)), alpha(it->color, fade), icon);
		dl->AddText(f, fsz(f), p + ImVec2(px(20) + iconW, px(12)), alpha(c::text, fade), it->text.c_str(), nullptr, px(520));
		y += size.y + px(10);
	}
}

// ======================================================================================================
// Source
// ======================================================================================================

// "Check what this camera can do" and, after it, "Report this camera" (frameio::probeCamera, FrameInput.h).  The probe
// needs the camera to itself, so the preview closes, the probe runs on a worker for a few seconds, and the camera
// opens again when it's done.  The report is shown here and goes nowhere by itself: the button opens a new GitHub issue
// in the browser with the details filled in, for the person to read, add the camera's name to and submit (or not).
// `failure` is the connection error when the camera didn't open, which goes in the report too.
void App::cameraCheck(const std::string& path, const std::string& failure) {
	ImGui::Dummy(ImVec2(0, px(4)));
	if (cameraProbe_.valid()) {
		if (cameraProbe_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
			ImGui::TextUnformatted(ICON_GAUGE "  Checking what this camera can do… (about 10 s)");
			ImGui::PopStyleColor();
			return;
		}
		cameraReport_ = cameraProbe_.get();
		haveCameraReport_ = true;
		std::string reopen = probeSource_;
		probeSource_.clear();
		if (!reopen.empty()) openSource(reopen);
		return;
	}
	ImGui::BeginDisabled(tracker.running());
	if (ghostButton(ICON_GAUGE "  Check what this camera can do", ImVec2(-FLT_MIN, 0))) {
		int index = std::atoi(path.c_str());
		probeSource_ = path;
		haveCameraReport_ = false;
		player.closeAndWait(3);
		cameraProbe_ = std::async(std::launch::async, [index] { return frameio::probeCamera(index); });
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(tracker.running() ? "Live tracking is using the camera. Stop tracking first."
			: "Tries the camera at 720p and 1080p, 30 and 60 fps, times what it really sends, and lists the settings its driver takes.");
	if (!haveCameraReport_ || cameraReport_.index != std::atoi(path.c_str())) return;

	// The short version: the best mode for timing, and the shutter.
	const frameio::CameraReport& r = cameraReport_;
	const frameio::CameraReport::Mode* best = nullptr;
	for (const auto& m : r.modes)
		if (m.measured > 0 && m.h >= 480 && (!best || m.measured > best->measured + 5 || (std::fabs(m.measured - best->measured) <= 5 && m.h > best->h))) best = &m;
	ImGui::PushTextWrapPos(0);
	if (!r.opened) dimText(ICON_WARN "  %s", r.error.c_str());
	else if (best) dimText(ICON_CHECK_CIRCLE "  Best for timing: %d × %d at about %.0f fps.", best->w, best->h, best->measured);
	else dimText(ICON_WARN "  It opened but sent no picture in any mode tried.");
	if (r.opened) {
		int sets = 0;
		for (const auto& k : r.controls) sets += k.settable;
		dimText("%d of %d settings take a value from VST. Shutter as VST opens it: %s.", sets, int(r.controls.size()),
			r.policy.shutter == frameio::Shutter::Fixed ? "fixed" : r.policy.shutter == frameio::Shutter::CantSet ? "can't be set here" : "automatic");
	}
	dimText("If this camera isn't working well, a report helps us add support for it. It opens a GitHub issue in your browser "
		"with these details filled in; nothing is sent unless you submit it.");
	ImGui::PopTextWrapPos();
	float bw = (ImGui::GetContentRegionAvail().x - px(8)) / 2;
	if (ghostButton(ICON_EXTERNAL "  Report this camera", ImVec2(bw, 0))) openWithSystem(cameraIssueUrl(failure));
	ImGui::SameLine(0, px(8));
	if (ghostButton(ICON_LAYERS "  Copy details", ImVec2(bw, 0))) {
		ImGui::SetClipboardText(r.markdown().c_str());
		toast("Camera details copied", c::accent2, ICON_CHECK_CIRCLE);
	}
}

// A new-issue link with the title and body filled in.  The body is percent-encoded and kept under about 6 KB so the
// link stays inside what browsers and GitHub accept.
std::string App::cameraIssueUrl(const std::string& failure) const {
	auto encode = [](const std::string& t) {
		std::string o;
		char b[4];
		for (unsigned char ch : t) {
			if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += char(ch);
			else { snprintf(b, sizeof b, "%%%02X", ch); o += b; }
		}
		return o;
	};
#if defined(_WIN32)
	const char* os = "Windows";
#elif defined(__APPLE__)
	const char* os = "macOS";
#else
	const char* os = "Linux";
#endif
	const frameio::CameraReport& r = cameraReport_;
	std::string title = "Camera support: " + (r.name.empty() ? "camera " + std::to_string(r.index) : r.name) + " (" + (r.backend.empty() ? os : r.backend) + ")";
	std::string body = "<!-- Filled in by VST Studio's \"Report this camera\". Add what you know, then submit. -->\n\n"
		"**Camera make and model:** \n\n**What happens:** (for example: no picture, only 30 fps, too dark)\n\n"
		"**VST** " VST_VERSION " on " + std::string(os) + ", OpenCV " CV_VERSION "\n\n";
	if (!failure.empty()) body += "**Connecting said:** " + failure + "\n\n";
	body += r.markdown();
	std::string encoded = encode(body);
	if (encoded.size() > 6000) encoded = encode(body.substr(0, 4000) + "\n\n(cut short: use Copy details for the rest)");
	return "https://github.com/joekiller/VideoSpeedTracker/issues/new?title=" + encode(title) + "&body=" + encoded;
}

// The Pi camera settings popup on the camera card.  Defaults suit a street seen from the side (PiCamera.h): 60 fps, a
// 1/1000 s shutter so cars don't smear, and Camera Module 3's focus locked so it never hunts as cars pass.
void App::piCameraSettings() {
	const picam::Camera& cam = piCams_[piChoice_];
	ImGui::PushFont(fonts.strong);
	ImGui::Text("%s  (%s)", cam.model.c_str(), cam.sensor.c_str());
	ImGui::PopFont();
	ImGui::PushTextWrapPos(px(340));

	picam::Settings best = piSettings_;
	best.fps = 0;
	double top = picam::chooseFps(best, &cam);
	char buf[96];
	snprintf(buf, sizeof buf, "Best it can do (%.0f fps)", top);
	ImGui::TextUnformatted("Frame rate");
	if (ImGui::RadioButton(buf, piSettings_.fps == 0)) piSettings_.fps = 0;
	if (top > 30) {
		ImGui::SameLine();
		if (ImGui::RadioButton("30 fps", piSettings_.fps == 30)) piSettings_.fps = 30;
	}
	dimText("More frames per second times fast cars better, especially on a short view.");

	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::TextUnformatted("Shutter");
	static const struct { int us; const char* label; } shutters[] = {
		{ 500, "1/2000 s" }, { 1000, "1/1000 s (recommended)" }, { 2000, "1/500 s" }, { 4000, "1/250 s" }, { 0, "Automatic" },
	};
	const char* current = "Custom";
	for (auto& sh : shutters) if (sh.us == piSettings_.shutterUs) current = sh.label;
	ImGui::SetNextItemWidth(px(220));
	if (ImGui::BeginCombo("##shutter", current)) {
		for (auto& sh : shutters) if (ImGui::Selectable(sh.label, sh.us == piSettings_.shutterUs)) piSettings_.shutterUs = sh.us;
		ImGui::EndCombo();
	}
	dimText("A short shutter keeps cars sharp; the camera turns its gain up to make up the light. At night, a longer one or "
		"automatic still counts cars, with blurrier pictures.");

	if (cam.autofocus) {
		ImGui::Dummy(ImVec2(0, px(4)));
		ImGui::TextUnformatted("Focus (locked, so it never hunts as cars pass)");
		float feet = float(1 / (std::max(piSettings_.focus, 0.01) * 0.3048));
		ImGui::SetNextItemWidth(px(220));
		if (ImGui::SliderFloat("##focus", &feet, 5, 100, "%.0f ft to the lane", ImGuiSliderFlags_Logarithmic)) {
			piSettings_.focus = 1 / (feet * 0.3048);
			piSettings_.focusGiven = true;
		}
		dimText("The distance from the camera to the middle of the street. The default (13 ft) keeps about 7 ft to 100 ft sharp.");
	}
	else {
		ImGui::Dummy(ImVec2(0, px(4)));
		dimText("This camera focuses by hand: turn the lens until a parked car's number plate is crisp in the preview, then "
			"tighten the lens's lock screw.");
	}

	ImGui::Dummy(ImVec2(0, px(4)));
	bool flipped = piSettings_.rotation == 180;
	if (ImGui::Checkbox("Mounted upside down", &flipped)) piSettings_.rotation = flipped ? 180 : 0;
	ImGui::PopTextWrapPos();
	dimText("Press Connect to start the camera with these settings.");
}

// ---- USB cameras (Webcam.h) ------------------------------------------------------------------------------------

void App::listUsbCameras() {
	if (usbListing_.valid() || now() - usbListedAt_ < 3) return;
	usbListedAt_ = now();
	usbListing_ = std::async(std::launch::async, [] { return webcam::list(); });
}

void App::pumpUsbListing() {
	if (!usbListing_.valid() || usbListing_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	usbCams_ = usbListing_.get();
	usbListed_ = true;
	usbListedAt_ = now();
}

const webcam::Device* App::usbDevice(int index) const {
	for (const webcam::Device& d : usbCams_) if (d.index == index) return &d;
	return nullptr;
}

// The settings a camera was last connected with come back with it (Recent keeps one entry per camera, rememberSource).
void App::pickUsbCamera(int index) {
	cameraIndex_ = index;
	piChoice_ = -1;
	usbSettings_ = webcam::Settings();
	usbSettings_.camera = index;
	const std::string key = "usb:" + std::to_string(index);
	for (const std::string& r : prefs.recent)
		if (frameio::classify(r) == frameio::Kind::Camera && frameio::deviceKey(r) == key) { webcam::parse(r, usbSettings_); break; }
}

webcam::Mode App::usbModeFor(const webcam::Settings& s) const {
	const webcam::Device* d = usbDevice(s.camera);
	std::vector<webcam::Mode> modes = d ? d->modes : std::vector<webcam::Mode>();
	if (s.picksMode()) {
		webcam::Mode m = webcam::best(modes);
		if (!m.valid()) { m.width = frameio::kWorkingWidth; m.height = frameio::kWorkingHeight; m.fps = 60; }   // the old ask
		if (s.fps > 0) m.fps = s.fps;
		return m;
	}
	webcam::Mode m;
	m.width = s.width; m.height = s.height; m.fps = s.fps; m.format = s.format;
	return m;
}

// The USB camera settings popup.  VST picks the mode by default (webcam::best: frames first, then 16:9, then size) and
// says why; every mode the driver lists can be chosen instead.  Nothing here touches the street: marks, distance and
// calibration belong to the camera (frameio::deviceKey), not to its mode, so the only thing said about them is what
// webcam::changeNote() knows a change of shape or size can do to the picture.
void App::usbCameraSettings(bool inUse) {
	const webcam::Device* d = usbDevice(cameraIndex_);
	usbSettings_.camera = cameraIndex_;
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(d ? d->label().c_str() : ("USB camera " + std::to_string(cameraIndex_)).c_str());
	ImGui::PopFont();
	ImGui::PushTextWrapPos(px(380));
	if (const webcam::Model* m = d ? webcam::recognize(*d) : nullptr) dimText(ICON_INFO "  %s", m->note);
	if (d && d->isVirtual())
		dimText(ICON_WARN "  A picture made by other software, not a camera. VST times it like any camera; speeds are right when that "
			"software passes the camera's frames through at a steady rate.");

	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::TextUnformatted("Picture size and frame rate");
	std::vector<webcam::Mode> listed = d ? d->modes : std::vector<webcam::Mode>();
	webcam::Mode best = webcam::best(listed);
	char b[200];
	if (best.valid()) snprintf(b, sizeof b, "Best for timing: %s##best", best.text().c_str());
	else snprintf(b, sizeof b, "Let VST choose (asks for 1280 × 720 at 60 fps)##best");
	if (ImGui::RadioButton(b, usbSettings_.picksMode() && usbSettings_.fps <= 0)) { usbSettings_.width = usbSettings_.height = 0; usbSettings_.fps = 0; usbSettings_.format.clear(); }
	ImGui::Indent(px(26));
	dimText("%s", webcam::whyBest(best, listed).c_str());
	ImGui::Unindent(px(26));
	std::vector<webcam::Mode> offered = webcam::offered(listed);
	offered.erase(std::remove(offered.begin(), offered.end(), best), offered.end());   // that's the first choice already
	if (offered.empty() && listed.empty()) {
		// Nothing listed (this system can't say, or the camera is busy): the sizes most cameras have, as asks.
		offered = { { 1280, 720, 60, "MJPG" }, { 1280, 720, 30, "MJPG" }, { 1920, 1080, 30, "MJPG" }, { 640, 480, 30, "" } };
		dimText("%s", d && !d->error.empty() ? (d->error + " Or ask for one of these: the camera takes its nearest.").c_str() : "Or ask for one of these: the camera takes its nearest.");
	}
	if (ImGui::BeginChild("##modes", ImVec2(px(380), std::min(ImGui::GetFrameHeightWithSpacing() * float(offered.size()) + px(4), px(260))), ImGuiChildFlags_None)) {
		for (const webcam::Mode& m : offered) {
			bool on = !usbSettings_.picksMode() && usbSettings_.width == m.width && usbSettings_.height == m.height
				&& std::lround(usbSettings_.fps) == std::lround(m.fps) && (usbSettings_.format.empty() || usbSettings_.format == m.format);
			if (ImGui::RadioButton(m.text().c_str(), on)) { usbSettings_.width = m.width; usbSettings_.height = m.height; usbSettings_.fps = m.fps; usbSettings_.format = m.format; }
		}
	}
	ImGui::EndChild();

	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::TextUnformatted("Shutter");
	static const struct { int us; const char* label; } shutters[] = {
		{ -1, "1/1000 s, slower if dark (recommended)" }, { 500, "1/2000 s" }, { 1000, "1/1000 s, always" }, { 2000, "1/500 s" }, { 4000, "1/250 s" }, { 0, "Automatic" },
	};
	const char* current = "Custom";
	for (auto& sh : shutters) if (sh.us == usbSettings_.shutterUs) current = sh.label;
	ImGui::SetNextItemWidth(px(300));
	if (ImGui::BeginCombo("##usbshutter", current)) {
		for (auto& sh : shutters) if (ImGui::Selectable(sh.label, sh.us == usbSettings_.shutterUs)) usbSettings_.shutterUs = sh.us;
		ImGui::EndCombo();
	}
	dimText("A short shutter keeps cars sharp and stops the camera lowering its frame rate in shade. Where the system won't "
		"let VST set it (macOS, some cameras), it stays automatic.");

	// What the change does to the street set up for this camera.
	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok));
	ImGui::TextWrapped(ICON_CHECK_CIRCLE "  These change how the camera sends pictures, not your street: your marks, distance and calibration stay.");
	ImGui::PopStyleColor();
	const std::string wanted = webcam::name(usbSettings_);
	if (inUse && player.isOpen()) {
		webcam::Mode was;
		was.width = player.info().width; was.height = player.info().height; was.fps = player.info().fps;
		std::string note = webcam::changeNote(was, usbModeFor(usbSettings_));
		bool marked = !prefs.zonesFor.empty() && frameio::deviceKey(prefs.zonesFor) == frameio::deviceKey(source);
		if (!note.empty() && (marked || calibrationDone())) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(note.find("shape") != std::string::npos ? c::warn : c::dim));
			ImGui::TextWrapped("%s", note.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(wanted == source || tracker.running());
		if (primaryButton(ICON_LINK "  Reconnect with these settings", ImVec2(-FLT_MIN, 0), c::ok)) { openSource(wanted); ImGui::CloseCurrentPopup(); }
		ImGui::EndDisabled();
		if (tracker.running() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Live tracking is using the camera. Stop tracking first.");
	}
	else dimText("Press Connect to start the camera with these settings.");
	ImGui::PopTextWrapPos();
}

// The marks (and calibration) were made on a picture of one shape; a mode of another shape stretches the working frame
// differently, so lanes sit at other heights.  Nothing is changed: it says so and how to get back.
void App::shapeNote() {
	if (!player.isOpen() || prefs.zonesFrame.empty() || frameio::deviceKey(prefs.zonesFor) != frameio::deviceKey(source)) return;
	webcam::Mode was, now;
	if (std::sscanf(prefs.zonesFrame.c_str(), "%dx%d", &was.width, &was.height) != 2) return;
	now.width = player.info().width; now.height = player.info().height;
	if (!was.valid() || !now.valid() || was.wide() == now.wide()) return;
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
	ImGui::PushTextWrapPos(0);
	ImGui::TextWrapped(ICON_WARN "  The picture is %d × %d now, another shape than the %d × %d you marked your street on. Your marks and "
		"calibration are kept: look at step 4 to check the box still sits on the lane, or pick a %s size in the camera settings.",
		now.width, now.height, was.width, was.height, was.wide() ? "16:9" : "4:3");
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

void App::pageSource() {
	flowStepper();
	float avail = ImGui::GetContentRegionAvail().x;
	float rightW = std::max(px(360), avail * .36f);
	float leftW = avail - rightW - px(20);

	ImGui::BeginGroup();
	{
		float cw = (leftW - px(32)) / 3.f, ch = px(232);
		auto sourceCard = [&](const char* id, const char* icon, ImU32 col, const char* title, const char* desc, auto body) {
			beginCard(id, nullptr, nullptr, ImVec2(cw, ch));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 p = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(p, p + ImVec2(px(44), px(44)), alpha(col, .14f), px(10));
			ImFont* f = fonts.title;
			ImVec2 is = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, icon);
			dl->AddText(f, fsz(f), p + (ImVec2(px(44), px(44)) - is) * .5f, col, icon);
			ImGui::Dummy(ImVec2(px(44), px(50)));
			ImGui::PushFont(fonts.strong);
			ImGui::TextUnformatted(title);
			ImGui::PopFont();
			ImGui::PushTextWrapPos(0);
			dimText("%s", desc);
			ImGui::PopTextWrapPos();
			ImGui::SetCursorPosY(ch - px(16) - ImGui::GetFrameHeight() - px(4));
			body();
			endCard();
		};
		sourceCard("srcfile", ICON_FILE_VIDEO, c::accent, "Video files", "Clips from any camera. Add several and they run as a batch.", [&] {
			float bw = (ImGui::GetContentRegionAvail().x - px(8)) * .5f;
			if (primaryButton(ICON_FOLDER_OPEN " Open", ImVec2(bw, 0))) {
				auto r = pfd::open_file("Open video", "", { "Video", "*.avi *.mp4 *.mkv *.mov *.m4v *.ts *.webm", "Photo of the view", "*.jpg *.jpeg *.png", "All files", "*" }).result();
				if (!r.empty()) { batch.clear(); openSource(r[0]); }
			}
			ImGui::SameLine(0, px(8));
			if (ghostButton(ICON_LAYERS " Batch", ImVec2(bw, 0))) {
				auto r = pfd::open_file("Add videos to the batch", "", { "Video", "*.avi *.mp4 *.mkv *.mov *.m4v *.ts *.webm", "All files", "*" }, pfd::opt::multiselect).result();
				addToBatch(r);
			}
		});
		ImGui::SameLine(0, px(16));
		if (piListing_.valid() && piListing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			piCams_ = piListing_.get();
			piListed_ = true;
			if (!piCams_.empty()) piChoice_ = 0;
		}
		sourceCard("srccam", ICON_CAMERA, c::ok, "USB, phone or Pi camera",
			piCams_.empty() ? "A webcam, a phone in USB webcam mode, or a Raspberry Pi camera. Tracks live, in real time."
				: "A Raspberry Pi camera, a webcam, or a phone in USB webcam mode. Tracks live, in real time.", [&] {
			bool pi = piChoice_ >= 0 && piChoice_ < int(piCams_.size());
			pumpUsbListing();
			// The menu takes what the Connect/Disconnect button leaves, so the button's label is never cut off in a narrow
			// window (three cards share the left two thirds of the page).  Both kinds of camera have a settings popup.
			bool inUse = pi ? picam::isName(source) : frameio::deviceKey(source) == "usb:" + std::to_string(cameraIndex_);
			float connectW = buttonWidth(inUse ? ICON_XMARK " Disconnect" : ICON_LINK " Connect");
			// The camera's settings sit in the card's top-right corner, beside its icon, where the row below has no room.
			{
				ImVec2 back = ImGui::GetCursorScreenPos();
				const char* label = ICON_SLIDERS " Settings";
				ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - px(18) - buttonWidth(label), ImGui::GetWindowPos().y + px(16)));
				if (ghostButton(label)) ImGui::OpenPopup(pi ? "##pisettings" : "##usbsettings");
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
					ImGui::SetTooltip("%s", pi ? "Pi camera settings: frame rate, shutter, focus"
						: "Camera settings: picture size, frame rate, shutter. VST picks the best for timing unless you choose.");
				if (pi && ImGui::BeginPopup("##pisettings")) { piCameraSettings(); ImGui::EndPopup(); }
				if (!pi && ImGui::BeginPopup("##usbsettings")) { usbCameraSettings(inUse); ImGui::EndPopup(); }
				ImGui::SetCursorScreenPos(back);
			}
			ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - connectW - px(8), px(76), px(130)));
			char camLabel[160];
			const webcam::Device* usb = usbDevice(cameraIndex_);
			if (pi) snprintf(camLabel, sizeof camLabel, ICON_CAMERA " Pi %d", piCams_[piChoice_].index);
			else if (usb && !usb->name.empty()) snprintf(camLabel, sizeof camLabel, "%s", usb->name.c_str());   // clipped to the box; the tooltip has it all
			else snprintf(camLabel, sizeof camLabel, "USB %d", cameraIndex_);   // "USB camera N" in the menu
			if (ImGui::IsWindowAppearing() || ImGui::IsItemHovered()) listUsbCameras();   // a camera plugged in since: there by the time the menu opens
			if (ImGui::BeginCombo("##cam", camLabel, ImGuiComboFlags_HeightLarge)) {
				listUsbCameras();
				for (int i = 0; i < int(piCams_.size()); i++) {
					snprintf(camLabel, sizeof camLabel, "Pi camera %d: %s", piCams_[i].index, piCams_[i].model.c_str());
					if (ImGui::Selectable(camLabel, piChoice_ == i)) piChoice_ = i;
				}
				// Never a dead end: on a Pi where none was found, say what usually fixes it.
				if (piListed_ && piCams_.empty() && picam::onRaspberryPi()) {
					ImGui::BeginDisabled();
					ImGui::Selectable("No Pi camera found: reseat the ribbon cable, then restart");
					ImGui::EndDisabled();
				}
				if (!piCams_.empty() || (piListed_ && picam::onRaspberryPi())) ImGui::Separator();
				// USB cameras by the name the system gives them, with the best frame rate their driver lists (Webcam.h).
				// Where nothing is listed (an unlisted platform, or still looking) the numbers are offered as before.
				for (const webcam::Device& d : usbCams_) {
					double top = d.topFps();
					if (top > 0) snprintf(camLabel, sizeof camLabel, "%s  ·  up to %.0f fps##usb%d", d.label().c_str(), top, d.index);
					else snprintf(camLabel, sizeof camLabel, "%s##usb%d", d.label().c_str(), d.index);
					if (ImGui::Selectable(camLabel, !pi && cameraIndex_ == d.index)) pickUsbCamera(d.index);
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
						const webcam::Model* m = webcam::recognize(d);
						ImGui::SetTooltip("Camera %d%s%s", d.index, m ? ": " : "", m ? m->note : "");
					}
				}
				if (!usbCams_.empty()) {
					ImGui::BeginDisabled();
					ImGui::Selectable("Not listed? Try a number:");
					ImGui::EndDisabled();
				}
				else if (!usbListed_) {
					ImGui::BeginDisabled();
					ImGui::Selectable("Looking for cameras…");
					ImGui::EndDisabled();
				}
				for (int i = 0; i < 6; i++) {
					if (usbDevice(i)) continue;
					snprintf(camLabel, sizeof camLabel, "USB camera %d%s", i, i == 0 && usbCams_.empty() ? " (usually built-in)" : "");
					if (ImGui::Selectable(camLabel, !pi && cameraIndex_ == i)) pickUsbCamera(i);
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
				ImGui::SetTooltip("%s", pi ? "Which camera to use." : usb && !usb->name.empty() ? (usb->label() + ", camera " + std::to_string(usb->index)).c_str()
					: "Which camera to use. USB camera 0 is usually the built-in or first one plugged in.");
			ImGui::SameLine(0, px(8));
			// The button follows the menu: Disconnect while the camera it names is the source (or connecting), Connect
			// otherwise.  A Pi camera's source name carries its settings, so any Pi camera source counts as "it".
			if (inUse) {
				if (ghostButton(ICON_XMARK " Disconnect", ImVec2(-FLT_MIN, 0))) disconnectSource();
			}
			else if (primaryButton(ICON_LINK " Connect", ImVec2(-FLT_MIN, 0), c::ok)) {
				batch.clear();
				if (pi) {
					piSettings_.camera = piCams_[piChoice_].index;
					openSource(picam::name(piSettings_));
				}
				else { usbSettings_.camera = cameraIndex_; openSource(webcam::name(usbSettings_)); }
			}
		});
		ImGui::SameLine(0, px(16));
		sourceCard("srcnet", ICON_BROADCAST, c::accent2, "Network camera", "An IP or PoE camera's RTSP or HTTP address, as its manual gives it.", [&] {
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(48));
			ImGui::InputTextWithHint("##url", "rtsp://camera/stream", urlBuf_, sizeof urlBuf_);
			ImGui::SameLine(0, px(8));
			ImGui::PushStyleColor(ImGuiCol_Button, v4(c::accent2, .85f));
			if (ImGui::Button(ICON_LINK, ImVec2(-FLT_MIN, 0)) && std::string(urlBuf_).size() > 8) { batch.clear(); openSource(urlBuf_); }
			ImGui::PopStyleColor();
		});
	}

	ImGui::Dummy(ImVec2(0, px(4)));
	std::string sample = findSample();
	if (!sample.empty()) {
		beginCard("sample", nullptr, nullptr, ImVec2(leftW, 0));
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::TextUnformatted(ICON_MAGIC);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextUnformatted("No camera yet? Try the 2016 Locust Avenue sample, the clip VST was built on.");
		// Flush with the card's right padding: the button's label is semibold, plus its frame padding.
		ImGui::SameLine(leftW - px(18) - fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, ICON_PLAY " Use the sample").x - px(28));
		if (ghostButton(ICON_PLAY " Use the sample")) { batch.clear(); openSource(sample); }
		endCard();
	}

	// Batch queue / recent.
	ImGui::Dummy(ImVec2(0, px(4)));
	auto jobs = tracker.jobs();
	beginCard("queue", ICON_LIST, batch.size() > 1 ? "Batch" : "Recent", ImVec2(leftW, 0));
	if (batch.size() > 1) {
		dimText("These run one after another when you press Start tracking. Each gets its own csv in %s.",
			(fs::path(dataDir()) / "Stats").string().c_str());
		int remove = -1;
		for (size_t i = 0; i < batch.size(); i++) {
			ImGui::PushID(int(i));
			const char* state = "queued";
			ImU32 sc = c::faint;
			for (auto& j : jobs)
				if (j.input == batch[i]) {
					state = j.state == Job::Running ? "running" : j.state == Job::Done ? "done" : j.state == Job::Failed ? "failed" : j.state == Job::Stopped ? "stopped" : "queued";
					sc = j.state == Job::Running ? c::accent : j.state == Job::Done ? c::ok : j.state == Job::Failed ? c::bad : c::faint;
				}
			ImGui::AlignTextToFramePadding();
			ImGui::Text(ICON_FILE_VIDEO "  %s", friendlySourceName(batch[i]).c_str());
			ImGui::SameLine(leftW - px(180));
			pill(state, c::bg0, sc);
			ImGui::SameLine(leftW - px(70));
			if (iconButton(ICON_XMARK, "Remove from batch")) remove = int(i);
			ImGui::PopID();
		}
		if (remove >= 0) batch.erase(batch.begin() + remove);
		if (ghostButton(ICON_TRASH " Clear batch")) batch.clear();
	}
	else if (prefs.recent.empty()) dimText("Videos and cameras you open show up here.");
	else
		for (size_t i = 0; i < prefs.recent.size(); i++) {
			const std::string& r = prefs.recent[i];
			ImGui::PushID(int(i));
			SourceInfo::Kind k = classifySource(r);
			std::string label = std::string(k == SourceInfo::Camera ? ICON_CAMERA : k == SourceInfo::Stream ? ICON_BROADCAST : k == SourceInfo::Image ? ICON_IMAGE : ICON_FILE_VIDEO)
				+ "   " + friendlySourceName(r);
			if (ImGui::Selectable(label.c_str(), r == source, 0, ImVec2(0, px(26)))) openSource(r);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", r.c_str());
			ImGui::PopID();
		}
	endCard();
	ImGui::Dummy(ImVec2(0, px(4)));
	aimCard(leftW);
	ImGui::EndGroup();

	// Right column: preview, facts about the source, config and output folders.
	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	beginCard("preview", ICON_EYE, "Preview", ImVec2(rightW, 0));
	float pw = ImGui::GetContentRegionAvail().x;
	std::string shortName = shortSourceName(player.info().path, 34);
	std::string waiting = player.opening() ? "Connecting to " + shortName + "…" : std::string(feedHint("Pick a source"));
	sharedFeedBadge(drawFrame(player.isOpen() ? player.texture().imguiId() : nullptr, ImVec2(pw, pw * 9 / 16), waiting.c_str()));
	// What is on screen, by name, and the way out of it: Cancel while connecting, Disconnect (or Close) after.
	if (player.opening() || player.isOpen()) {
		bool liveSrc = player.info().kind == SourceInfo::Camera || player.info().kind == SourceInfo::Stream;
		const char* out = player.opening() ? ICON_XMARK " Cancel" : liveSrc ? ICON_XMARK " Disconnect" : ICON_XMARK " Close";
		float ow = ImGui::CalcTextSize(out).x + px(28);
		ImGui::AlignTextToFramePadding();
		ImGui::PushFont(fonts.strong);
		if (player.opening()) ImGui::Text("Connecting to %s…  %.0f s", shortName.c_str(), player.openingSeconds(now()));
		else ImGui::TextUnformatted(shortName.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", player.info().label.c_str());
		ImGui::PopFont();
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ow);
		ImGui::BeginDisabled(tracker.running());   // Live tracking is using it: Stop there first
		if (ghostButton(out, ImVec2(ow, 0))) disconnectSource();
		ImGui::EndDisabled();
		if (tracker.running() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Live tracking is using this source. Stop tracking to disconnect.");
	}
	if (player.isOpen()) {
		const SourceInfo& in = player.info();
		auto fact = [&](const char* k, const std::string& v) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
			ImGui::TextUnformatted(k);
			ImGui::PopStyleColor();
			ImGui::SameLine(px(130));
			ImGui::TextUnformatted(v.c_str());
		};
		if (!in.deviceName.empty()) fact("Camera", in.deviceName);
		fact("Resolution", std::to_string(in.width) + " × " + std::to_string(in.height));
		if (in.kind != SourceInfo::Image) {
			char buf[64];
			snprintf(buf, sizeof buf, in.fps > 0 ? "%.2f fps" : "unknown", in.fps);
			fact("Frame rate", buf);
			if (in.seconds() > 0) {
				snprintf(buf, sizeof buf, "%d:%02d  (%d frames)", int(in.seconds()) / 60, int(in.seconds()) % 60, in.frames);
				fact("Length", buf);
			}
			if (!in.codec.empty()) fact("Codec", in.codec);
			if (in.shutter == frameio::Shutter::Fixed && in.shutterUs > 0) {
				snprintf(buf, sizeof buf, "1/%d s, fixed", int(std::lround(1e6 / in.shutterUs)));
				fact("Shutter", buf);
			}
			else if (in.shutter != frameio::Shutter::Unknown) fact("Shutter", "automatic");
			if (in.kind == SourceInfo::Camera && frameio::classify(in.path) == frameio::Kind::Camera) {
				webcam::Settings us;
				webcam::parse(in.path, us);
				fact("Mode", in.modePicked ? "picked by VST for timing" : us.picksMode() ? "as the camera gave it" : "your choice (camera settings)");
				if (in.modePicked && !in.modeWhy.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", in.modeWhy.c_str());
			}
		}
		ImGui::Dummy(ImVec2(0, px(4)));
		// Plain-language checks against what the tracker needs (README "Set-up Checklist").
		auto check = [&](bool good, const char* okText, const char* badText) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(good ? c::ok : c::warn));
			ImGui::TextUnformatted(good ? ICON_CHECK_CIRCLE : ICON_WARN);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::PushTextWrapPos(0);
			ImGui::TextUnformatted(good ? okText : badText);
			ImGui::PopTextWrapPos();
		};
		check(in.height >= 720, "HD or better: plenty of detail", "Below 720p: it works, with a little less detail; 1280×720 or better sharpens the bumpers");
		if (in.kind == SourceInfo::Image) check(false, "", "A still photo: use it to plan zones; tracking needs video");
		else if (in.fps <= 0) check(true, "Frame rate is good for timing", "");
		else { std::string fr = frameRateAdvice(in.fps); check(in.fps >= 24, fr.c_str(), fr.c_str()); }   // what it means for the planned view (Guide.cpp)
		// The shutter, for cameras (FrameInput.h's opening policy fixes it when it can).  On automatic, a webcam can drop
		// to half its frame rate in shade while still reporting the full one, and speeds are counted in frames.
		if (in.shutter == frameio::Shutter::Fixed)
			check(true, "Shutter fixed: cars stay sharp and the frame rate holds steady in shade", "");
		else if (in.shutter == frameio::Shutter::Automatic)
			check(false, "", "Shutter on automatic: the picture was too dark at a fixed fast shutter. It still counts cars. "
				"In daylight, Disconnect and connect again and VST fixes it for you.");
		else if (in.shutter == frameio::Shutter::CantSet)
			check(false, "", "Shutter on automatic: VST can't fix it on this camera here. Lock exposure in the camera's own app "
				"(Logitech G HUB, for one) so it keeps its frame rate in shade.");
		if (in.measuredFps > 0 && in.fps > 0 && in.measuredFps < in.fps * .8) {
			char b[240];
			snprintf(b, sizeof b, "Sending about %.0f fps though it reports %.0f. Speeds are counted in frames, so fix the shutter "
				"or add light before you calibrate.", in.measuredFps, in.fps);
			check(false, "", b);
		}
		// What VST knows about this model beyond what its driver says (the registry, Webcam.h): a hint, never a setting.
		if (frameio::classify(in.path) == frameio::Kind::Camera)
			if (const webcam::Model* m = webcam::recognize(in.deviceName, 0, 0)) {
				ImGui::PushStyleColor(ImGuiCol_Text, v4(std::string(m->kind) == "virtual" ? c::warn : c::dim));
				ImGui::PushTextWrapPos(0);
				ImGui::TextWrapped(ICON_INFO "  %s: %s", m->name, m->note);
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
		shapeNote();
		// Short-view timing's look-ahead was fitted to step 2's frame rate; a slower camera moves cars farther per frame
		// than it looks for.  Fitting it again changes only that, not the speed lines, so the calibration stays.
		if (cfg.g.subFrameTiming && in.fps > 0 && in.kind != SourceInfo::Image) {
			int need = lookAheadFor(in.fps);
			if (std::min(cfg.g.maxL2RDistOnEntry, cfg.g.maxR2LDistOnEntry) < need) {
				char b[240];
				snprintf(b, sizeof b, "Short-view timing looks ahead for a %.0f fps camera; at %.0f fps fast cars move farther each frame "
					"than that and can be missed.", planSaved().fps, in.fps);
				check(false, "", b);
				snprintf(b, sizeof b, ICON_SLIDERS "  Fit the look-ahead to %.0f fps", in.fps);
				if (ghostButton(b, ImVec2(-FLT_MIN, 0))) {
					cfg.g.maxL2RDistOnEntry = cfg.g.maxR2LDistOnEntry = need;
					toast("Look-ahead fitted to this camera. Speed lines and calibration are unchanged.", c::ok, ICON_CHECK);
				}
			}
		}
		if (frameio::classify(in.path) == frameio::Kind::Camera) cameraCheck(in.path, "");
	}
	else if (!player.error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad));
		ImGui::TextUnformatted(ICON_WARN);
		ImGui::SameLine();
		ImGui::TextWrapped("%s", player.error.c_str());
		ImGui::PopStyleColor();
		// A USB camera that won't open is the case a report helps most: the probe says what the driver does offer.
		std::string failed = !probeSource_.empty() ? probeSource_ : player.info().path;
		if (frameio::classify(failed) == frameio::Kind::Camera) cameraCheck(failed, player.error);
	}
	else if (cameraProbe_.valid()) cameraCheck(probeSource_, "");   // the preview let go of the camera for the check
	endCard();

	ImGui::Dummy(ImVec2(0, px(4)));
	beginCard("files", ICON_FOLDER, "Settings and output", ImVec2(rightW, 0));
	formLabel("Config", px(112));
	ImGui::TextUnformatted(cfg.path().empty() ? "(none)" : fs::path(cfg.path()).filename().string().c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cfg.path().c_str());
	float bw = (ImGui::GetContentRegionAvail().x - px(16)) / 3;
	if (ghostButton(ICON_FILE_IMPORT " Open", ImVec2(bw, 0))) {
		auto r = pfd::open_file("Open VST.cfg", cfg.path(), { "VST config", "*.cfg", "All files", "*" }).result();
		if (!r.empty() && !loadConfig(r[0])) toast(cfg.error, c::bad, ICON_WARN);
	}
	ImGui::SameLine(0, px(8));
	ImGui::BeginDisabled(!cfg.hasBackup());
	if (ghostButton(ICON_UNDO " Undo", ImVec2(bw, 0))) { if (cfg.restoreBackup()) toast("Settings restored to how they were when Studio opened", c::ok, ICON_UNDO); else toast(cfg.error, c::bad, ICON_WARN); }
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Changes save automatically. Undo puts back the settings from when Studio opened this file.");
	ImGui::SameLine(0, px(8));
	if (ghostButton(ICON_EXPORT " Save as", ImVec2(bw, 0))) {
		std::string r = pfd::save_file("Save config as", cfg.path(), { "VST config", "*.cfg" }).result();
		if (!r.empty()) { if (cfg.saveAs(r)) prefs.configPath = r; else toast(cfg.error, c::bad, ICON_WARN); }
	}
	ImGui::Dummy(ImVec2(0, px(2)));
	formLabel("Results go to", px(112));
	{
		// A per-user data folder is a long path: keep its end (the part that names the folder) and show it all on hover.
		std::string dir = dataDir();
		float room = ImGui::GetContentRegionAvail().x;
		std::string shown = dir;
		for (size_t cut = 0; cut < dir.size() && ImGui::CalcTextSize(shown.c_str()).x > room;) {
			do cut++; while (cut < dir.size() && (dir[cut] & 0xC0) == 0x80);   // whole UTF-8 characters only
			shown = "…" + dir.substr(cut);
		}
		ImGui::TextUnformatted(shown.c_str());
		if (shown != dir && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dir.c_str());
	}
	if (ghostButton(ICON_FOLDER_OPEN " Open folder", ImVec2(bw * 1.5f + px(4), 0))) openWithSystem(dataDir());
	ImGui::SameLine(0, px(8));
	if (ghostButton(ICON_FOLDER " Change", ImVec2(-FLT_MIN, 0))) {
		std::string r = pfd::select_folder("Where should results go?", dataDir()).result();
		if (!r.empty()) cfg.g.dataPathPrefix = r;
	}
	endCard();
	ImGui::EndGroup();
}

// ======================================================================================================
// Zones
// ======================================================================================================

void App::pageZones() {
	flowStepper();
	float avail = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
	float inspW = px(372);
	float canvasW = avail - inspW - px(20);
	float barH = px(48);
	ImGui::BeginGroup();
	float canvasH = std::min(availH - barH - px(10), canvasW * 9 / 16);
	ImGui::BeginChild("zcanvas", ImVec2(canvasW, canvasH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	View v = drawFrame(player.isOpen() ? player.texture().imguiId() : nullptr, ImGui::GetContentRegionAvail(),
		feedHint("Open a video, camera or photo of your street to draw on"));
	OverlayStyle st;
	const char* hovered = nullptr;
	st.editable = !levelTool_ && !parkingLineDragging();   // the level dots get the mouse while they're out
	if (player.isOpen()) zoneOverlay(v, cfg.g, st, &hovered);   // with no picture, the lines would only cover the hint
	// The parking lane (ParkingLane.h) gets the mouse only where the zones don't want it: it usually lies outside the
	// analysis box, and a zone handle on top of it stays reachable.
	if (player.isOpen() && parkingLane().set()) {
		double py[2] = { prefs.parkY0, prefs.parkY1 };
		if (parkingLineOverlay(v, py, !levelTool_ && !hovered, &hovered)) { prefs.parkY0 = py[0]; prefs.parkY1 = py[1]; savePrefsSoon(); }
	}
	if (player.isOpen() && ghostCar_) ghostCars(v, cfg.g, cameraSetup(), ghostGoesRight());
	if (player.isOpen() && (levelTool_ || levelGrid_)) {
		levelGuide(v, levelDots_, levelTool_);
		if (levelTool_) hovered = "Lay the line along the curb or a lane stripe, then press Level it";
	}
	sharedFeedBadge(v);   // over the zones, so their dimming doesn't fade it
	ImGui::EndChild();

	// Scrub bar under the canvas: play/step for files, a status line for live sources.
	beginCard("zbar", nullptr, nullptr, ImVec2(canvasW, barH));
	ImGui::SetCursorPosY((barH - ImGui::GetFrameHeight()) * .5f);
	if (player.seekable()) {
		if (iconButton(player.playing ? ICON_PAUSE : ICON_PLAY, player.playing ? "Pause" : "Play")) player.playing = !player.playing;
		ImGui::SameLine();
		int pos = player.position();
		ImGui::SetNextItemWidth(canvasW * .45f);
		if (ImGui::SliderInt("##scrub", &pos, 0, std::max(0, player.info().frames - 1), "frame %d")) player.seek(pos);
		ImGui::SameLine();
	}
	ImGui::AlignTextToFramePadding();
	{
		// One line in a fixed-height bar: shorten with an ellipsis rather than run past the card's edge.
		const char* hint = hovered ? hovered : ICON_INFO "  Drag the box, lines and labels. Size them on a frame with a car.";
		ImVec2 at = ImGui::GetCursorScreenPos();
		float maxX = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
		float lineH = ImGui::GetFrameHeight();
		ImGui::PushStyleColor(ImGuiCol_Text, v4(hovered ? c::text : c::dim));
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, ImVec2(maxX, at.y + lineH), maxX, maxX, hint, nullptr, nullptr);
		ImGui::PopStyleColor();
		ImGui::Dummy(ImVec2(std::max(0.f, maxX - at.x), lineH));
	}
	endCard();

	// Legend: what each colour on the picture is for.
	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("legend", nullptr, nullptr, ImVec2(canvasW, 0));
	struct Key { ImU32 col; const char* name; const char* what; };
	const bool oneWay = cfg.g.LegalDirection == "L2R" || cfg.g.LegalDirection == "R2L";
	std::vector<Key> keys = {
		{ c::accent2, "Analysis box", "the only part of the picture the tracker looks at" },
		{ c::rgb(0xFFFFFF), "Speed lines", "timing runs from one to the other" },
		{ c::obstruction, "Obstruction", "the tracker coasts through it" },
		// A one-way street has one lane line; a two-way street's two lanes share the legend's second row.
		oneWay ? Key{ cfg.g.LegalDirection == "L2R" ? c::l2r : c::r2l, "Lane", "hubcap height of cars; drag an end to follow the street" }
		       : Key{ c::l2r, "→ Lane", "hubcap height of cars going right" },
	};
	if (!oneWay) keys.push_back({ c::r2l, "← Lane", "hubcap height of cars going left" });
	if (cfg.g.hasStreetEdge()) keys.push_back({ c::streetEdge, "Street edge", "along the curb: how the street recedes" });
	if (parkingLane().set()) keys.push_back({ c::parking, "Parking lane", "wheel centres of parked cars, for step 5" });
	float kw = (canvasW - px(36)) / 3;
	for (int i = 0; i < int(keys.size()); i++) {
		if (i % 3) ImGui::SameLine(px(18) + kw * (i % 3));
		ImGui::BeginGroup();
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(0, px(5)), p + ImVec2(px(18), px(9)), keys[i].col, px(2));
		ImGui::SetCursorScreenPos(p + ImVec2(px(28), 0));
		ImGui::PushFont(fonts.strong);
		ImGui::TextUnformatted(keys[i].name);
		ImGui::PopFont();
		ImGui::SetCursorScreenPos(p + ImVec2(px(28), fsz(fonts.strong) + px(4)));
		ImGui::PushFont(fonts.caption);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
		ImGui::TextUnformatted(keys[i].what);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::EndGroup();
	}
	endCard();
	ImGui::EndGroup();

	ImGui::SameLine(0, px(20));
	ImGui::BeginChild("inspector", ImVec2(inspW, availH), 0, 0);
	zoneInspector();
	ImGui::EndChild();
}

void App::zoneInspector() {
	Globals& g = cfg.g;
	if (inspectorScroll_ >= 0) { ImGui::SetScrollY(px(inspectorScroll_)); inspectorScroll_ = -1; }
	const float col = px(150);
	auto intRow = [&](const char* label, int* v, int lo, int hi, const char* help = nullptr) {
		formLabel(label, col);
		ImGui::PushID(label);
		ImGui::DragInt("##v", v, 1, lo, hi);
		ImGui::PopID();
		if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", help);
	};

	beginCard("level", ICON_ROTATE, "Level the picture");
	dimText("Camera a little crooked on its mount? Turn the picture until the curb runs straight across. Do it before you calibrate.");
	{
		float deg = float(g.levelDegrees), lim = float(Globals::kMaxLevelDegrees);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::SliderFloat("##level", &deg, -lim, lim, deg == 0 ? "level (0°)" : "%.1f°"))
			g.levelDegrees = std::round(std::clamp(double(deg), -double(lim), double(lim)) * 10) / 10;
		levelGrid_ = ImGui::IsItemActive();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Ctrl+click to type an angle. Positive turns the picture counter-clockwise.");
	}
	float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
	if (!levelTool_) {
		ImGui::BeginDisabled(!player.isOpen());
		if (ghostButton(ICON_CROSSHAIRS "  Use the curb", ImVec2(half, 0))) levelTool_ = true;
		ImGui::EndDisabled();
	} else if (primaryButton(ICON_CHECK "  Level it", ImVec2(half, 0))) {
		double turn = levelCorrection(levelDots_[0], levelDots_[1]);
		double next = std::clamp(g.levelDegrees + turn, -Globals::kMaxLevelDegrees, Globals::kMaxLevelDegrees);
		next = std::round(next * 10) / 10;
		for (ImVec2& d : levelDots_) d = levelTurn(d, next - g.levelDegrees);   // keep the dots on the curb as it turns
		g.levelDegrees = next;
		levelTool_ = false;
		if (std::fabs(turn) > Globals::kMaxLevelDegrees) toast("That's more tilt than software should fix. Straighten the camera on its mount, then level again.", c::warn, ICON_WARN);
	}
	ImGui::SameLine();
	if (levelTool_) { if (ghostButton("Cancel", ImVec2(half, 0))) levelTool_ = false; }
	else {
		ImGui::BeginDisabled(g.levelDegrees == 0);
		if (ghostButton(ICON_UNDO "  Straight", ImVec2(half, 0))) g.levelDegrees = 0;
		ImGui::EndDisabled();
	}
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("street", ICON_ROAD, "Your street");
	dimText("Which way may traffic go? On a one-way street VST calls out everyone driving the other way.");
	int legal = g.LegalDirection == "L2R" ? 1 : g.LegalDirection == "R2L" ? 2 : 0;
	static const char* const legalLabels[] = { "Two-way", "→ only", "← only" };
	if (segmented("legal", &legal, legalLabels, 3)) {
		g.LegalDirection = legal == 1 ? "L2R" : legal == 2 ? "R2L" : "Both";
		// One-way: one lane, so the wrong-way line joins the legal one (the canvas then shows and drags just that).
		if (legal == 1) { g.R2LStreetY = g.L2RStreetY; g.R2LStreetYRight = g.L2RStreetYRight; }
		if (legal == 2) { g.L2RStreetY = g.R2LStreetY; g.L2RStreetYRight = g.R2LStreetYRight; }
	}
	ImGui::Dummy(ImVec2(0, px(2)));
	static const char* compass[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	auto compassCombo = [&](const char* label, std::string& value, ImU32 col2) {
		formLabel(label, col);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(col2));
		if (ImGui::BeginCombo((std::string("##") + label).c_str(), value.c_str())) {
			for (const char* cpt : compass)
				if (ImGui::Selectable(cpt, value == cpt)) value = cpt;
			ImGui::EndCombo();
		}
		ImGui::PopStyleColor();
	};
	compassCombo("→ traffic heads", g.L2RDirection, c::l2r);
	compassCombo("← traffic heads", g.R2LDirection, c::r2l);
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("zone", ICON_STOPWATCH, "Speed zone");
	dimText("Timing starts when a front bumper crosses one white line and stops at the other. Put them as far apart as the lane stays clear; "
		"with short-view timing (step 2) even a short stretch works.");
	// On the rails (Globals::railZone()): inside the box, at least kMinZone apart.  A typed value (Ctrl+click) can go past
	// the drag limits, so the rails are applied after either field changes, moving the other line if it must.
	{
		const int sl0 = g.speedLineLeft, sr0 = g.speedLineRight;
		intRow("Left line", &g.speedLineLeft, 0, std::max(g.AnalysisBoxWidth - Globals::kMinZone, 0));
		intRow("Right line", &g.speedLineRight, std::min(Globals::kMinZone, g.AnalysisBoxWidth), g.AnalysisBoxWidth);
		if (g.speedLineLeft != sl0 || g.speedLineRight != sr0) g.railZone(g.speedLineLeft != sl0);
		dimText("The lines stay inside the box and at least %d px apart. Right on the box's edge is fine: VST times cars as they come into view.", Globals::kMinZone);
	}
	// How long a car at the limit spends between the lines, from the calibration (the legal direction's lane on a
	// one-way street).  Before calibrating, the numbers in VST.cfg belong to some other street, so it says nothing
	// yet; with short-view timing a short time is expected, not a problem.
	if (!calibrationDone()) dimText(ICON_INFO "  After you calibrate (step 5) this says how long a car spends between the lines.");
	else {
		double frames = g.LegalDirection == "R2L" ? g.CalibrationFramesR2L : g.CalibrationFramesL2R;
		double secs = frames / g.CalibrationFPS * g.CalibrationSpeed / std::max(prefs.speedLimit, 1);
		bool fine = secs >= 1 || g.subFrameTiming;
		ImGui::PushStyleColor(ImGuiCol_Text, v4(fine ? c::ok : c::warn));
		if (secs >= 1) ImGui::TextWrapped(ICON_CHECK_CIRCLE "  About %.1f s for a car at %d mph (from the calibration)", secs, prefs.speedLimit);
		else if (g.subFrameTiming) ImGui::TextWrapped(ICON_CHECK_CIRCLE "  About %.1f s for a car at %d mph: short, and fine with short-view timing on", secs, prefs.speedLimit);
		else ImGui::TextWrapped(ICON_WARN "  About %.1f s for a car at %d mph. Spread the lines wider, or turn on short-view timing in step 2", secs, prefs.speedLimit);
		ImGui::PopStyleColor();
	}
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("box", ICON_VECTOR, "Analysis box");
	dimText("Just the street: leave out treetops, parked cars and the sidewalk so they can't fool the tracker.");
	intRow("Left", &g.AnalysisBoxLeft, 0, 1279);
	intRow("Top", &g.AnalysisBoxTop, 0, 719);
	{
		const int w0 = g.AnalysisBoxWidth;
		intRow("Width", &g.AnalysisBoxWidth, Globals::kMinZone, 1280);
		if (g.AnalysisBoxWidth != w0) { g.AnalysisBoxWidth = std::max(g.AnalysisBoxWidth, Globals::kMinZone); g.railZone(false); }  // the right side moved: the lines stay inside
	}
	intRow("Height", &g.AnalysisBoxHeight, 30, 720);
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	{
		// A lane line has two ends, so it can follow a street that runs at an angle across the picture (Perspective.h);
		// setLane() keeps a level line stored the way it always was.  One-way: one line for both directions.
		const bool oneWay = g.LegalDirection == "L2R" || g.LegalDirection == "R2L", legalL2R = g.LegalDirection == "L2R";
		auto laneRows = [&](const char* leftLabel, const char* rightLabel, bool l2r, bool both) {
			perspective::Line l = g.lane(l2r);
			int left = int(l.yLeft), right = int(l.yRight);
			intRow(leftLabel, &left, 1, g.AnalysisBoxHeight);
			intRow(rightLabel, &right, 1, g.AnalysisBoxHeight, "Different from the left end when the street runs at an angle across the picture");
			setLane(g, l2r, both, left, right);
		};
		if (oneWay) {
			beginCard("lanes", ICON_ARROWS_LR, "Lane");
			dimText("Put the line at hubcap height for cars in the lane. Street at an angle? Drag either end of the line so it follows the street. "
				"Anyone driving against the arrows is logged as wrong-way.");
			laneRows("Left end", "Right end", legalL2R, true);
		} else {
			beginCard("lanes", ICON_ARROWS_LR, "Lanes");
			dimText("Put each dashed line at hubcap height for cars in that lane. Drag an end to follow a street that runs at an angle.");
			laneRows("← left end", "← right end", false, false);
			laneRows("→ left end", "→ right end", true, false);
		}

		// Advanced: how the street recedes.  A second line along the curb meets the lane line at the street's vanishing
		// point; short-view timing then measures each car along the ground rather than in pixels (Perspective.h).  Off by
		// default: on the sample's view it changes nothing, and it pays off only where the far end is clearly smaller.
		ImGui::Dummy(ImVec2(0, px(2)));
		bool recedes = g.hasStreetEdge();
		if (ImGui::Checkbox("The street gets farther away across the picture", &recedes)) {
			if (recedes) {
				// Start parallel to the lane line, a little below it (or above, near the box's bottom): no effect until the
				// user lays it on the curb.
				perspective::Line l = g.lane(oneWay ? legalL2R : true);
				int off = std::max(l.yLeft, l.yRight) + 40 <= g.AnalysisBoxHeight - 2 ? 40 : -40;
				g.streetEdge[0] = std::clamp(int(l.yLeft) + off, 1, g.AnalysisBoxHeight);
				g.streetEdge[1] = std::clamp(int(l.yRight) + off, 1, g.AnalysisBoxHeight);
			}
			else g.streetEdge[0] = g.streetEdge[1] = 0;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("Advanced, for a camera aimed partly along the street.\nLay the green dashed line along the curb; where it closes in on the lane line\n"
				"tells VST how the street recedes, and short-view timing then measures along the ground.");
		if (recedes) {
			dimText("Drag the ends of the green dashed line onto the curb, or any line that runs along the street (a lane stripe, a row of parked cars).");
			intRow("Edge left", &g.streetEdge[0], 1, g.AnalysisBoxHeight);
			intRow("Edge right", &g.streetEdge[1], 1, g.AnalysisBoxHeight);
			bool l2r = oneWay ? legalL2R : true;
			double xV = g.vanishingX(l2r);
			double ratio = perspective::depthRatio(g.speedLineLeft, g.speedLineRight, xV);
			if (std::isnan(xV)) dimText(ICON_INFO "  The two lines are parallel, so there's no receding to correct for. That's fine: the speeds are already right.");
			else if (perspective::vanishingUsable(xV, g.AnalysisBoxWidth, g.speedLineLeft, g.speedLineRight)) {
				bool rightFar = ratio > 1;
				int pct = int(std::lround((rightFar ? ratio : 1 / ratio) * 100 - 100));
				ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok));
				ImGui::TextWrapped(ICON_CHECK_CIRCLE "  The %s speed line is about %d%% farther away than the %s one; speeds are measured along the ground.",
					rightFar ? "right" : "left", pct, rightFar ? "left" : "right");
				ImGui::PopStyleColor();
			}
			if (!g.subFrameTiming)
				dimText(ICON_INFO "  Short-view timing (Detection tuning) is what uses this. Whole-frame timing already gets it right through calibration.");
		}

		// "Check it with a ghost car": the setup is the camera, not the math.  Type how the camera sits (distance and lens
		// from step 2, height and angle here) and Studio draws a typical car at each speed line (Canvas.h ghostCars).
		// Nudge the numbers until the ghost sits on a real car; then the same camera model gives the zone's length in feet
		// (Calibrate from this setup) and, for an angled camera, the street edge (Draw the street edge), so neither
		// needs a curb or a test drive.  A drive past afterwards is still the check.
		ImGui::Dummy(ImVec2(0, px(2)));
		ImGui::Checkbox("Check it with a ghost car", &ghostCar_);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("Draws a typical car (15.5 ft) at each speed line from how your camera is set up.\n"
				"Too big or too small: the distance or lens is off. Leaning wrong: the angle is.");
		if (ghostCar_) {
			dimText("Pause on a car at a speed line and nudge these until the blue ghost sits on it.");
			auto numRow = [&](const char* label, double* val, double step, const char* fmt, double lo, double hi, const char* help) {
				formLabel(label, col);
				ImGui::PushID(label);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputDouble("##v", val, step, step * 5, fmt)) *val = std::clamp(*val, lo, hi);
				ImGui::PopID();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", help);
			};
			double distFt = prefs.planDist * plan::kFt;
			double wasFt = distFt;
			numRow("To the lane (ft)", &distFt, 1, "%.0f", 4, 300, "From the camera to the middle of the lane, square to the street. The same as step 2.");
			// Through the plan, as the user's own number: a calibration later keeps it (Guide.cpp setMeasuredDistance).
			if (distFt != wasFt) setMeasuredDistance(distFt / plan::kFt, "Your distance, from the ghost car on Mark your street");
			numRow("Camera height (ft)", &prefs.calcHeightFt, 1, "%.0f", 0, 100, "Above the road. A second-floor window is about 12-15 ft.");
			numRow("Field of view (°)", &prefs.planHfov, 1, "%.0f", perspective::kMinHfovDeg, perspective::kMaxHfovDeg,
				"Horizontal angle the camera sees. Phones and webcams: 60-80°. Wide IP cameras: 90-110°. The same as step 2.");
			formLabel("Aimed along", col);
			{
				float yaw = float(prefs.calcYawDeg), lim = float(perspective::kMaxYawDeg);
				// SliderFloat prints a format with no conversion as it is, so the label carries the magnitude and which end.
				char fmt[48];
				if (std::fabs(yaw) < .5f) std::snprintf(fmt, sizeof fmt, "square (0°)");
				else std::snprintf(fmt, sizeof fmt, "%d° %s end farther", int(std::lround(std::fabs(yaw))), yaw > 0 ? "right" : "left");
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::SliderFloat("##yaw", &yaw, -lim, lim, fmt)) prefs.calcYawDeg = std::round(double(yaw));
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("How far the camera is turned from facing the street squarely.\nCtrl+click to type; negative is toward the left.");
			}
			perspective::CameraSetup cam = cameraSetup();
			double zoneFt = perspective::zoneFeet(cam, 1280, 720, g.AnalysisBoxLeft + g.speedLineLeft, g.AnalysisBoxLeft + g.speedLineRight);
			double xV = perspective::vanishingFrameX(cam, 1280) - g.AnalysisBoxLeft;
			if (std::isnan(zoneFt) || zoneFt <= 0)
				dimText(ICON_INFO "  That setup can't see both speed lines on the street. Check the distance and angle.");
			else {
				double ratio = perspective::depthRatio(g.speedLineLeft, g.speedLineRight, xV);
				std::string depthNote = std::isnan(ratio) || std::fabs(ratio - 1) < .02 ? std::string("both speed lines about the same distance away")
					: "the " + std::string(ratio > 1 ? "right" : "left") + " one about "
						+ std::to_string(int(std::lround((ratio > 1 ? ratio : 1 / ratio) * 100 - 100))) + "% farther";
				ImGui::TextWrapped(ICON_RULER "  About %.1f ft of street between the speed lines, %s.", zoneFt, depthNote.c_str());
				float half2 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
				if (ghostButton(ICON_ARROWS_LR "  Street edge", ImVec2(half2, 0))) {
					// The edge only has to meet the lane line at the vanishing point; it is drawn 40 px off it to be seen.
					perspective::EdgeResult e = perspective::edgeThrough(g.lane(ghostGoesRight()), xV, 40, g.AnalysisBoxHeight);
					if (std::isnan(xV)) { g.streetEdge[0] = g.streetEdge[1] = 0; toast("Square to the street: no street edge needed.", c::ok, ICON_CHECK_CIRCLE); }
					else if (e.ok) { g.streetEdge[0] = int(std::lround(e.yLeft)); g.streetEdge[1] = int(std::lround(e.yRight)); }
					else toast("The analysis box is too short to draw the street edge in. Make it a little taller.", c::warn, ICON_WARN);
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("Draws the green street edge from this setup, so short-view timing measures along the ground.");
				ImGui::SameLine();
				if (primaryButton(ICON_CHECK "  Calibrate from it", ImVec2(-FLT_MIN, 0))) {
					// A typed-in camera model is the roughest input (kSigmaCamera), so once anything is measured on the
					// street it counts for little; until then it gives a working start.
					char label[96];
					snprintf(label, sizeof label, "Camera setup: %.0f ft away, %.0f ft up", cam.distanceFt, cam.heightFt);
					applyDistanceCalibration(true, true, calib::framesForZone(zoneFt, g.CalibrationSpeed, g.CalibrationFPS), "the camera setup",
						"camera", calib::kSigmaCamera, label);
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("Adds the zone's length from this setup to your calibration, as a rough measurement.\n"
						"Anything measured on the street (a drive past, cones, a known car) counts for much more.");
			}
		}
		endCard();
	}

	// The parking lane: a line on the parked cars' wheel centres, kept for step 5's Known car mode (ParkingLane.h), which
	// then tells a parked car from a passing one by itself and scales a parked one to the travel lane.  The tracker never
	// uses it.  It starts parallel to the lane line, 50 px above it (the far side, the usual view from a window), or below
	// when there's no room above; the user drags it onto a parked car's wheels.
	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("parking", ICON_CAR, "Parking lane");
	{
		const bool oneWayP = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
		const bool laneL2R = oneWayP ? g.LegalDirection == "L2R" : true;
		bool hasPark = parkingLane().set();
		if (ImGui::Checkbox("Cars park along this street", &hasPark)) {
			if (hasPark) {
				// It starts 50 px beyond the farther lane line (above it), or, with no room there, 50 px nearer than the
				// nearer one, so on a two-way street it never begins between the two lanes.
				double l1 = laneRowAt(true, 0), r1 = laneRowAt(true, 1280), l2 = laneRowAt(false, 0), r2 = laneRowAt(false, 1280);
				if (oneWayP) { l2 = l1 = laneRowAt(laneL2R, 0); r2 = r1 = laneRowAt(laneL2R, 1280); }
				const bool farL1 = l1 + r1 <= l2 + r2;
				double l = farL1 ? l1 : l2, r = farL1 ? r1 : r2, off = -50;
				if (std::min(l, r) - 50 < 20) { l = farL1 ? l2 : l1; r = farL1 ? r2 : r1; off = 50; }
				prefs.parkY0 = std::clamp(std::round(l + off), 1.0, 719.0);
				prefs.parkY1 = std::clamp(std::round(r + off), 1.0, 719.0);
			}
			else prefs.parkY0 = prefs.parkY1 = 0;
			savePrefsSoon();
		}
		ImGui::PushTextWrapPos(0);
		if (!hasPark)
			dimText("Optional. Mark where parked cars' wheels sit, and step 5 can calibrate from a parked car of a known model: "
				"it tells parked from passing by itself and corrects for the parked car being farther away.");
		else {
			dimText("Drag the blue dashed line onto the middle of a parked car's wheels, the way the lane line sits on a moving "
				"car's hubcaps. Drag an end if the street runs at an angle. The tracker ignores it; it's for step 5.");
			calib::ParkingLane park = parkingLane();
			calib::CarSpot sp;
			double laneRow = laneRowAt(laneL2R, 640);
			calib::parkedBeyond(cameraSetup(), 640, laneRow, park, sp);
			if (std::isfinite(sp.parkedFt)) {
				const char* how = sp.fromPicture ? "worked out from the picture, your camera's height and step 2's distance"
					: sp.typical ? "a typical guess: VST can't read it from this camera" : "as you measured";
				ImGui::PushStyleColor(ImGuiCol_Text, v4(c::parking));
				ImGui::TextWrapped(ICON_RULER "  Parked cars' wheels are about %.1f ft %s than moving cars' (%s).",
					std::fabs(sp.parkedFt), sp.parkedFt >= 0 ? "farther" : "nearer", how);
				ImGui::PopStyleColor();
			}
			formLabel("Measured (ft)", col);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));   // room for the help marker
			if (ImGui::InputDouble("##parkft", &prefs.parkFt, .5, 1, prefs.parkFt > 0 ? "%.1f" : "auto")) {
				prefs.parkFt = std::clamp(prefs.parkFt, 0.0, 40.0);
				savePrefsSoon();
			}
			helpMarker("Optional. Across the street, from a moving car's wheels to a parked car's (for a car in the middle of\n"
				"the travel lane, about half a lane plus half a parking lane). Leave it at 0 (auto) and VST works it out from the\n"
				"two lines, using the camera height under \"Check it with a ghost car\" above.");
			int cmp = std::fabs(laneRow - park.at(640)) < 4 ? 0 : 1;
			if (!cmp) dimText(ICON_WARN "  The parking lane sits on the lane line. Drag it onto the parked cars.");
		}
		ImGui::PopTextWrapPos();
	}
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("obs", ICON_BAN, "Obstruction");
	bool has = g.obstruction[0] < g.obstruction[1];
	if (ImGui::Checkbox("A tree trunk or pole blocks part of the view", &has)) {
		if (has) { g.obstruction[0] = g.AnalysisBoxWidth / 3; g.obstruction[1] = g.AnalysisBoxWidth / 3 + 50; }
		else g.obstruction[0] = g.obstruction[1] = 0;
	}
	if (has) {
		intRow("From", &g.obstruction[0], 0, g.AnalysisBoxWidth);
		intRow("To", &g.obstruction[1], 0, g.AnalysisBoxWidth);
		intRow("Clearance", &g.obstruction_extent, 0, 200, "Pixels past the obstruction before a car shows up again as one shape");
	}
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	if (ImGui::CollapsingHeader(ICON_SLIDERS "  Detection tuning")) {
		dimText("Defaults suit most streets. Change these only if cars are missed or shadows get tracked.");
		intRow("Sensitivity", &g.SENSITIVITY_VALUE, 1, 255, "Brightness change that counts as motion. Lower = more sensitive.");
		intRow("Blur", &g.BLUR_SIZE, 1, 100, "Smooths the motion image to join a car into one shape.");
		intRow("Slop", &g.SLOP, 0, 100, "Pixels of tolerance when two vehicles overlap.");
		intRow("Vehicle height", &g.nextHeight, 10, 300, "First guess at a vehicle's height in pixels.");
		intRow("Large vehicle", &g.largeVehicleArea, 1, 1000, "Area above which a vehicle counts as a truck or bus.");
		bool shortView = g.subFrameTiming != 0;
		if (ImGui::Checkbox("Short view (about 14-25 ft of street in the picture)", &shortView)) g.subFrameTiming = shortView;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Times each car's front bumper to a fraction of a frame, for views where a car fills most of the frame.\nThe site planner in step 2, Pick your spot, can set this and the steps below for you.");
		intRow("Max → step", &g.maxL2RDistOnEntry, 5, 1280, "Fastest trackable motion, pixels per two frames. A short view needs several hundred.");
		intRow("Max ← step", &g.maxR2LDistOnEntry, 5, 1280, "Fastest trackable motion, pixels per two frames. A short view needs several hundred.");
		intRow("Entry look-back", &g.entryLookBack, 0, 1280);
		intRow("Slowest logged", &g.minLoggedSpeed, 0, 100, "Vehicles timed below this many mph (walkers, cyclists) are counted in the run manifest but not written to the csv. 0 logs every timed vehicle. Follows the speed limit (5 mph under it, at most 18) until you change it here.");
	}

	// Validation from the engine itself, then save/revert.
	auto problems = cfg.problems();
	ImGui::Dummy(ImVec2(0, px(4)));
	for (auto& p : problems) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad));
		ImGui::TextWrapped(ICON_X_CIRCLE "  %s", p.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PushTextWrapPos(0);
	if (problems.empty()) dimText(ICON_CHECK "  Changes save automatically.");
	else dimText("Changes save automatically once the problems above are fixed.");
	ImGui::PopTextWrapPos();
	ImGui::BeginDisabled(!cfg.hasBackup());
	if (ghostButton(ICON_UNDO "  Undo changes since opening", ImVec2(-FLT_MIN, 0))) {
		if (cfg.restoreBackup()) toast("Settings restored to how they were when Studio opened", c::ok, ICON_UNDO);
		else toast(cfg.error, c::bad, ICON_WARN);
	}
	ImGui::EndDisabled();
}

// ======================================================================================================
// Calibrate
// ======================================================================================================

// Speed you drove, as an exact whole number with -/+ steps (a slider can't land on 27 reliably).  True when edited.
static bool speedStepper(const char* id, int* mph) {
	ImGui::SetNextItemWidth(-FLT_MIN);
	bool changed = ImGui::InputInt(id, mph, 1, 5);
	*mph = std::clamp(*mph, calib::kTargetMin, calib::kTargetMax);
	return changed;
}

// The drive-by's target speed as chips (calib::targetPresets: 10, 15, 20, 25 and the posted limit), above the steps'
// pass list so it's chosen before the drive.  Until the user touches it the target follows the saved choice or the
// posted limit; a chip is remembered (prefs.calibTarget) for next time.  Any other speed goes in the "I drove at"
// stepper below, which is the same value: the chips are shortcuts for it.
void App::targetSpeedChips() {
	if (!calibSpeedSet_) calibSpeed_ = calib::defaultTarget(prefs.calibTarget, prefs.speedLimit);
	std::vector<int> presets = calib::targetPresets(prefs.speedLimit);
	std::vector<std::string> text;
	for (int mph : presets) text.push_back(std::to_string(mph) + (mph == prefs.speedLimit ? " (limit)" : ""));
	std::vector<const char*> labels;
	for (const std::string& t : text) labels.push_back(t.c_str());
	int sel = int(std::find(presets.begin(), presets.end(), calibSpeed_) - presets.begin());
	if (sel == int(presets.size())) sel = -1;   // a custom or dashcam-read speed: no chip lit
	formLabel("Target (mph)", px(130));
	if (segmented("target", &sel, labels.data(), int(labels.size()))) {
		calibSpeed_ = presets[size_t(sel)];
		calibSpeedSet_ = true;
		prefs.calibTarget = calibSpeed_;
		prefs.save();
		dashFor_ = 0;   // a dashcam reading no longer stands for "I drove at"
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Any steady speed calibrates equally well, so drive at whatever is comfortable on your street.\n"
			"The posted limit is a good choice. For another speed, type it under \"I drove at\".");
}

double App::drivenMph(int vehicleId) const {
	return dashFor_ == vehicleId && calibSpeed_ == int(std::lround(dashMph_)) ? dashMph_ : calibSpeed_;
}

// Under "I drove at" on the pass picker: load the drive past's speed records and read the car's speed through the
// zone for the picked pass, so the driver needn't hold a cruise-control speed.  Two kinds of record, loaded together
// or alone: a Tesla's dashcam clips (wheel speed in every frame) and the phone's VST Ride-along recordings (GPS
// speed about once a second).  Each kind is matched to the pass by GPS at the camera spot when that's set, else by
// the clocks (which must agree to a second or two), and dashcam::jointSpeed() cross-checks them when both are there.
void App::dashcamSpeed(const Vehicle* picked) {
	if (ghostButton(ICON_CAR "  Read my speed from a dashcam clip or ride-along", ImVec2(-FLT_MIN, 0))) {
		auto files = pfd::open_file("Tesla dashcam clips (-front.mp4) or VST Ride-along recordings (.csv) from the drive past", "",
			{ "Drive records", "*.mp4 *.csv", "TeslaCam clips", "*.mp4", "Ride-along recordings", "*.csv", "All files", "*" },
			pfd::opt::multiselect).result();
		std::string lastErr;
		int added = 0;
		for (const std::string& f : files) {
			if (std::any_of(dashClips_.begin(), dashClips_.end(), [&](const dashcam::Clip& c) { return c.path == f; })) continue;
			dashcam::Clip c;
			std::string err;
			if (dashcam::readAny(f, c, err)) { dashClips_.push_back(std::move(c)); added++; }
			else lastErr = err;
		}
		if (added) dashFor_ = 0;
		if (!lastErr.empty()) toast(added ? "Skipped a file: " + lastErr : lastErr, added ? c::warn : c::bad, ICON_WARN);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Drive past at any steady speed while recording, then pick the files. Load both to cross-check them.\n"
			"  Tesla: the -front.mp4 clips from the USB drive's TeslaCam folder (software 2025.44.25 or later, HW3 and newer).\n"
			"  Any phone: a recording from VST Ride-along, the GPS recorder at joekiller.github.io/VideoSpeedTracker.");
	if (dashClips_.empty()) return;

	int teslas = 0, phones = 0;
	for (const dashcam::Clip& c : dashClips_) (c.kind == dashcam::Clip::Tesla ? teslas : phones)++;
	std::string loaded;
	if (teslas) loaded += std::to_string(teslas) + " Tesla clip" + (teslas == 1 ? "" : "s");
	if (phones) loaded += std::string(teslas ? ", " : "") + std::to_string(phones) + " ride-along" + (phones == 1 ? "" : "s");
	dimText(ICON_FOLDER "  %s loaded", loaded.c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("Clear##drive")) { dashClips_.clear(); dashFor_ = 0; return; }
	if (!picked) { dimText("Pick your pass above."); return; }

	const std::string& when = !picked->live && !picked->recorded.empty() ? picked->recorded : picked->time;
	double end = dashcam::parseIso(when), crossing = picked->frames() / std::max(picked->fps, 1.0);
	bool haveSpot = prefs.siteLat != 0 || prefs.siteLon != 0;
	dashcam::Joint j = dashcam::jointSpeed(dashClips_, prefs.siteLat, prefs.siteLon, end, crossing);
	ImGui::PushTextWrapPos(0);
	// One line per kind of record, then the verdict.
	auto line = [&](const char* who, const dashcam::PassSpeed& ps, int count) {
		if (!count) return;
		if (!ps.found) { dimText(ICON_X_CIRCLE "  %s: doesn't cover %s", who, when.substr(11).c_str()); return; }
		const char* how = ps.byGps ? "GPS match" : "clock match";
		if (ps.steady()) dimText(ICON_CHECK "  %s: %.1f mph, steady (%s)", who, ps.mph, how);
		else {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
			ImGui::TextWrapped(ICON_WARN "  %s: %.1f to %.1f mph, not steady (%s)", who, ps.minMph, ps.maxMph, how);
			ImGui::PopStyleColor();
		}
		if (ps.byGps && ImGui::IsItemHovered())
			ImGui::SetTooltip("Passed your camera spot %.1f s %s this pass's time on the computer's clock.\n"
				"GPS matching doesn't depend on the clocks agreeing.", std::fabs(ps.clockOffset), ps.clockOffset >= 0 ? "after" : "before");
	};
	line("Tesla", j.tesla, teslas);
	line("Phone", j.phone, phones);
	if (!j.found) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::TextWrapped(ICON_WARN "  Nothing loaded covers %s, when this pass was measured. Pick the files from that minute%s.",
			when.substr(11).c_str(), haveSpot ? "" : ", or check the clocks");
		ImGui::PopStyleColor();
	}
	else {
		if (dashFor_ != picked->id) {
			dashFor_ = picked->id;
			dashMph_ = std::round(j.mph * 10) / 10;
			calibSpeed_ = int(std::lround(dashMph_));
			calibSpeedSet_ = true;
		}
		if (j.agree) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok));
			ImGui::TextWrapped(ICON_CHECK_CIRCLE "  They agree within %.1f mph. Using %.1f mph.", std::fabs(j.tesla.mph - j.phone.mph), dashMph_);
			ImGui::PopStyleColor();
		}
		else if (j.both) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
			ImGui::TextWrapped(ICON_WARN "  They differ by %.1f mph. Using the Tesla's %.1f mph; the phone may not have had a clear view of the sky.",
				std::fabs(j.tesla.mph - j.phone.mph), dashMph_);
			ImGui::PopStyleColor();
		}
		else if (j.steady) dimText("Using %.1f mph.", dashMph_);
		else {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
			ImGui::TextWrapped(ICON_WARN "  The car sped up or slowed down here, so %.1f mph is only roughly right. A steadier pass calibrates better.", dashMph_);
			ImGui::PopStyleColor();
		}
	}
	ImGui::PopTextWrapPos();

	// The camera spot for GPS matching: learnt from a clock-matched pass held steady through the zone (the car's or
	// phone's position mid-zone), or pasted from a map ("38.03021, -78.48362").
	if (haveSpot) {
		dimText(ICON_LOCATE "  Camera spot %.5f, %.5f", prefs.siteLat, prefs.siteLon);
		ImGui::SameLine();
		if (ImGui::SmallButton("Forget##spot")) { prefs.siteLat = prefs.siteLon = 0; prefs.save(); dashFor_ = 0; }
		return;
	}
	if (j.found && j.steady) {
		if (ghostButton(ICON_LOCATE "  Remember this spot for GPS matching", ImVec2(-FLT_MIN, 0))) {
			double lat, lon;
			if (dashcam::positionAt(dashClips_, end + .5 - crossing / 2, lat, lon)) {
				prefs.siteLat = lat;
				prefs.siteLon = lon;
				prefs.save();
				dashFor_ = 0;
				toast("Camera spot saved. Later passes are matched by GPS, whatever the clocks say.", c::ok, ICON_CHECK);
			}
			else toast("There was no GPS fix at this pass. Try another pass, or paste the spot from a map.", c::warn, ICON_WARN);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Saves where the car was halfway through the zone on this pass. From then on, passes are\n"
				"matched by when the car went by that spot, so the clocks needn't agree.");
	}
	static char spot[64] = "";
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputTextWithHint("##spot", "Or paste the spot from a map: 38.03021, -78.48362", spot, sizeof spot, ImGuiInputTextFlags_EnterReturnsTrue)) {
		double lat, lon;
		if (std::sscanf(spot, "%lf , %lf", &lat, &lon) == 2 && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && (lat != 0 || lon != 0)) {
			prefs.siteLat = lat;
			prefs.siteLon = lon;
			prefs.save();
			dashFor_ = 0;
			spot[0] = 0;
		}
		else toast("That isn't a latitude, longitude pair like 38.03021, -78.48362.", c::warn, ICON_WARN);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The street in front of the middle of your zone. In most map apps, press and hold (or right-click)\nthere and copy the coordinates. Press Enter to save.");
}

void App::pageCalibrate() {
	flowStepper();
	Globals& g = cfg.g;
	// The timed pictures for marking spots alone: each is a copy of the camera's frame at its moment.  Once both are
	// taken the canvas shows the first and the next click marks spot 1 on it.
	if (coneTimerAt_ >= 0 && player.isOpen()) {
		double t = now() - coneTimerAt_;
		for (int i = 0; i < 2; i++)
			if (coneShot_[i].empty() && t >= kConeFirstS + i * kConeGapS) coneShot_[i] = player.frame().clone();
		if (!coneShot_[1].empty()) {
			coneTimerAt_ = -1;
			coneView_ = 0;
			conePlacing_ = 0;
			coneX_[0] = coneX_[1] = coneY_[0] = coneY_[1] = -1;
			toast("Both pictures taken. Click the pole in each one to mark spots 1 and 2.", c::ok, ICON_CHECK);
		}
	}
	bool coneShots = !coneShot_[0].empty() && !coneShot_[1].empty();
	if (!coneShots || calibMode_ != 2) coneView_ = -1;
	if (calibrationStale()) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::TextWrapped(ICON_WARN "  The speed lines moved since this calibration (step 4, or short-view timing in step 2), so it no longer fits them. Calibrate once more, any way below.");
		ImGui::PopStyleColor();
	}
	float avail = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
	float sideW = px(400);
	float canvasW = avail - sideW - px(20);
	float barH = px(56);

	ImGui::BeginGroup();
	float canvasH = std::min(availH - barH - px(10), canvasW * 9 / 16);
	ImGui::BeginChild("ccanvas", ImVec2(canvasW, canvasH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	// In the cones-or-pole mode the canvas can show one of the timed pictures instead of the camera.
	if (coneView_ >= 0 && coneTexOf_ != coneView_) { coneTex_.update(coneShot_[coneView_]); coneTexOf_ = coneView_; }
	void* tex = coneView_ >= 0 ? coneTex_.imguiId() : player.isOpen() ? player.texture().imguiId() : nullptr;
	View v = drawFrame(tex, ImGui::GetContentRegionAvail(), feedHint("Open a video or connect the camera (step 3)"));
	OverlayStyle st;
	st.opacity = .75f;
	st.dimOutside = false;
	if (player.isOpen()) zoneOverlay(v, g, st);
	// Highlight the line the user should be lining the bumper up with in the manual mode.
	if (calibMode_ == 1 && player.isOpen()) {
		bool l2r = calibDir_ == 0;
		int which = calibMarks[calibDir_][0] < 0 ? 0 : 1;
		int lineX = (l2r == (which == 0)) ? g.speedLineLeft : g.speedLineRight;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 a = v.at(float(g.AnalysisBoxLeft + lineX), float(g.AnalysisBoxTop) - 30), b = v.at(float(g.AnalysisBoxLeft + lineX), float(g.AnalysisBoxTop + g.AnalysisBoxHeight) + 30);
		float pulse = .6f + .4f * std::sin(float(now()) * 5);
		dl->AddLine(a, b, alpha(c::warn, pulse), px(5));
		std::string t = which == 0 ? "Step to the frame where the front bumper touches this line" : "Now step to where it touches this line";
		ImVec2 ts = fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, t.c_str());
		ImVec2 tp(std::clamp(a.x - ts.x * .5f, v.origin.x + px(8), v.origin.x + v.size.x - ts.x - px(20)), a.y - ts.y - px(16));
		dl->AddRectFilled(tp, tp + ts + ImVec2(px(16), px(10)), c::warn, px(6));
		dl->AddText(fonts.strong, fsz(fonts.strong), tp + ImVec2(px(8), px(5)), c::bg0, t.c_str());
	}
	// Two-cone and known-car modes: the marks so far (cones' spots or the car's bumpers), and the click that places the
	// next one.  Both measure a known distance between two left-right positions; only the names and arrays differ.
	bool carMode = calibMode_ == 3;
	float* markX = carMode ? carX_ : coneX_;
	float* markY = carMode ? carY_ : coneY_;
	auto markName = [&](int i) {
		if (!carMode) return "Spot " + std::to_string(i + 1);
		return std::string(i == 0 ? "Left " : "Right ") + (carWheels_ ? "wheel" : "bumper");
	};
	if ((calibMode_ == 2 || carMode) && player.isOpen()) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		// Known car: the parking lane as drawn in step 4, so it's plain which line a car's wheels are on.
		const calib::ParkingLane park = parkingLane();
		if (carMode && park.set()) {
			double py[2] = { park.yLeft, park.yRight };
			parkingLineOverlay(v, py, false, nullptr, .8f);
		}
		float top = float(g.AnalysisBoxTop) - 30, bottom = float(g.AnalysisBoxTop + g.AnalysisBoxHeight) + 30;
		if (carMode)
			for (int i = 0; i < 2; i++)
				if (markX[i] >= 0) { top = std::min(top, markY[i] - 60); bottom = std::max(bottom, markY[i] + 30); }
		for (int i = 0; i < 2; i++) {
			if (markX[i] < 0) continue;
			ImVec2 a = v.at(markX[i], top), b = v.at(markX[i], bottom), base = v.at(markX[i], markY[i]);
			dl->AddLine(a, b, alpha(c::warn, .8f), px(2));
			if (carMode && carWheels_) {
				// A crosshair on the wheel's centre, as the card's picture shows it.
				dl->AddCircle(base, px(9), c::warn, 0, 2.5f);
				dl->AddLine(base - ImVec2(px(14), 0), base + ImVec2(px(14), 0), c::warn, 2.f);
			}
			else dl->AddTriangleFilled(base + ImVec2(0, -px(16)), base + ImVec2(-px(8), 0), base + ImVec2(px(8), 0), c::warn);
			// Each name on its line's outer side, so two marks close together (a car's wheels) don't print over each other.
			std::string t = markName(i);
			const int other = 1 - i;
			const bool outLeft = markX[other] >= 0 && markX[i] < markX[other];
			const float tw = fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, t.c_str()).x;
			float tx = outLeft ? a.x - px(6) - tw : a.x + px(6);
			tx = std::clamp(tx, v.origin.x + px(4), v.origin.x + v.size.x - tw - px(4));
			dl->AddText(fonts.strong, fsz(fonts.strong), ImVec2(tx, a.y), c::warn, t.c_str());
		}
		if (markX[0] >= 0 && markX[1] >= 0) {
			ImVec2 a = v.at(markX[0], markY[0]), b = v.at(markX[1], markY[0]);
			dl->AddLine(a + ImVec2(0, px(8)), b + ImVec2(0, px(8)), c::warn, px(2));
		}
		// Known car: where the car is, on the car itself, in the colour of the line it's on.
		if (carMode && markX[0] >= 0 && markX[1] >= 0) {
			const bool oneWayC = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
			bool l2r = oneWayC ? g.LegalDirection == "L2R" : coneDir_ != 2;
			calib::CarSpot spot = carSpot(l2r);
			char t[96];
			ImU32 col;
			if (park.set() && spot.offLines) { std::snprintf(t, sizeof t, ICON_WARN "  Not on a line: click the wheel centres"); col = c::warn; }
			else if (carParked_) { std::snprintf(t, sizeof t, ICON_CAR "  PARKED  ·  scaled x%.2f to the travel lane", carScale(l2r)); col = c::parking; }
			else { std::snprintf(t, sizeof t, ICON_CHECK_CIRCLE "  IN THE TRAVEL LANE"); col = l2r ? c::l2r : c::r2l; }
			ImVec2 ts = fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, t);
			ImVec2 mid = v.at(float(markX[0] + markX[1]) * .5f, std::min(markY[0], markY[1]));
			ImVec2 tp(std::clamp(mid.x - ts.x * .5f - px(8), v.origin.x + px(6), v.origin.x + v.size.x - ts.x - px(22)),
				std::max(mid.y - px(46) - ts.y, v.origin.y + px(44)));
			dl->AddRectFilled(tp, tp + ts + ImVec2(px(16), px(10)), col, px(6));
			dl->AddText(fonts.strong, fsz(fonts.strong), tp + ImVec2(px(8), px(5)), c::bg0, t);
		}
		if (coneTimerAt_ >= 0) {
			// Big enough to read from across the room on the way out.
			double t = now() - coneTimerAt_;
			int which = coneShot_[0].empty() ? 0 : 1;
			int left = int(std::ceil(kConeFirstS + which * kConeGapS - t));
			std::string s = "Picture of spot " + std::to_string(which + 1) + " in " + std::to_string(std::max(left, 0)) + " s";
			ImVec2 ts = fonts.title->CalcTextSizeA(fsz(fonts.title), FLT_MAX, 0, s.c_str());
			ImVec2 tp = v.origin + ImVec2((v.size.x - ts.x) * .5f, px(16));
			dl->AddRectFilled(tp - ImVec2(px(12), px(8)), tp + ts + ImVec2(px(12), px(8)), alpha(c::bg0, .85f), px(8));
			dl->AddText(fonts.title, fsz(fonts.title), tp, c::warn, s.c_str());
		}
		if (conePlacing_ >= 0) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			std::string t = carMode ? (carWheels_ ? "Click the middle of the " + std::string(conePlacing_ == 0 ? "left" : "right") + " wheel, on the side facing you"
					: "Click the " + std::string(conePlacing_ == 0 ? "left" : "right") + " bumper, where it ends on the side facing the camera")
				: "Click the cone or pole at spot " + std::to_string(conePlacing_ + 1);
			ImVec2 ts = fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, t.c_str());
			ImVec2 tp = v.origin + ImVec2((v.size.x - ts.x) * .5f, px(12));
			dl->AddRectFilled(tp - ImVec2(px(8), px(5)), tp + ts + ImVec2(px(8), px(5)), c::warn, px(6));
			dl->AddText(fonts.strong, fsz(fonts.strong), tp, c::bg0, t.c_str());
			ImVec2 m = ImGui::GetMousePos();
			bool inside = m.x >= v.origin.x && m.y >= v.origin.y && m.x < v.origin.x + v.size.x && m.y < v.origin.y + v.size.y;
			if (inside && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
				ImVec2 f = v.frame(m);
				markX[conePlacing_] = f.x;
				markY[conePlacing_] = f.y;
				conePlacing_ = markX[1 - conePlacing_] < 0 ? 1 - conePlacing_ : -1;    // straight on to the other mark
				if (coneShots && conePlacing_ >= 0 && !carMode) coneView_ = conePlacing_;   // on its own picture
			}
		}
	}
	if (coneView_ < 0) sharedFeedBadge(v);   // a timed picture on screen is not the feed
	ImGui::EndChild();

	// Frame stepper.
	beginCard("cbar", nullptr, nullptr, ImVec2(canvasW, barH));
	ImGui::SetCursorPosY((barH - ImGui::GetFrameHeight()) * .5f);
	bool canStep = player.seekable();
	ImGui::BeginDisabled(!canStep);
	int pos = std::max(player.position(), 0);
	if (iconButton(ICON_REWIND, "Back 10 frames  (Shift+←)") || (canStep && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && ImGui::GetIO().KeyShift)) player.seek(pos - 10);
	ImGui::SameLine();
	if (iconButton(ICON_STEP_BACK, "Back 1 frame  (←)") || (canStep && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !ImGui::GetIO().KeyShift)) player.seek(pos - 1);
	ImGui::SameLine();
	if (iconButton(player.playing ? ICON_PAUSE : ICON_PLAY, "Play / pause  (Space)") || (canStep && ImGui::IsKeyPressed(ImGuiKey_Space))) player.playing = !player.playing;
	ImGui::SameLine();
	if (iconButton(ICON_STEP_FWD, "Forward 1 frame  (→)") || (canStep && ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !ImGui::GetIO().KeyShift)) player.seek(pos + 1);
	ImGui::SameLine();
	if (iconButton(ICON_FAST_FWD, "Forward 10 frames  (Shift+→)") || (canStep && ImGui::IsKeyPressed(ImGuiKey_RightArrow) && ImGui::GetIO().KeyShift)) player.seek(pos + 10);
	ImGui::SameLine(0, px(16));
	ImGui::SetNextItemWidth(canvasW * .4f);
	int p2 = pos;
	if (ImGui::SliderInt("##cscrub", &p2, 0, std::max(0, player.info().frames - 1), "")) player.seek(p2);
	ImGui::EndDisabled();
	ImGui::SameLine(0, px(16));
	ImGui::AlignTextToFramePadding();
	double fps = player.info().fps > 0 ? player.info().fps : 30;
	ImGui::PushFont(fonts.strong);
	ImGui::Text("frame %d", pos);
	ImGui::PopFont();
	ImGui::SameLine();
	dimText("%.2f s", pos / fps);
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	// Under the picture: the latest check pass's result when there is one (it has the room to sit beside the
	// numbers), else every measurement the calibration is made of, else (nothing measured yet) how it is used.
	if (lastCheck_.valid) calibrationCheckCard(canvasW);
	else if (!calibrationInputs().empty()) {
		// The rest of the column, scrolling if a two-way street's two lists need more.
		beginCard("calinputs", ICON_LAYERS, "What your calibration is made of", ImVec2(canvasW, std::max(px(150), ImGui::GetContentRegionAvail().y)), true);
		calibrationInputsCard(true);
		endCard();
	}
	else {
		beginCard("how", ICON_INFO, "How VST turns frames into speed", ImVec2(canvasW, 0));
		ImGui::PushTextWrapPos(0);
		dimText("While tracking, VST doesn't measure feet. It counts the frames a front bumper takes to get from one white line to the other and "
			"compares that with your calibration pass:");
		ImGui::PopTextWrapPos();
		ImGui::PushFont(fonts.strong);
		ImGui::TextUnformatted("speed  =  calibration speed  ×  calibration frames  ÷  frames this car took");
		ImGui::PopFont();
		dimText("So a car that crosses in half the frames you did was going twice your speed. Different frame rates are scaled automatically.");
		endCard();
	}
	ImGui::EndGroup();

	ImGui::SameLine(0, px(20));
	ImGui::BeginChild("cside", ImVec2(sideW, availH));
	static const char* const modes[] = { "Drive-through", "Count frames", "Cones/pole", "Known car" };
	if (segmented("cmode", &calibMode_, modes, 4)) conePlacing_ = -1;   // a pending click belongs to the mode it was for
	if (calibMode_ >= 2) calibChecking_ = false;   // a check is always a pass, on the first two modes
	ImGui::Dummy(ImVec2(0, px(4)));

	// The check pass reuses the calibration controls (the pass list or the frame marks, and the speed you drove):
	// calibChecking_ switches what the button does, from setting the calibration to testing it.  Checking only
	// makes sense once a direction has a calibration to test.
	CalState calL2R = calibrationState(true), calR2L = calibrationState(false);
	bool anyCalibrated = calL2R.kind != CalState::None || calR2L.kind != CalState::None;
	if (!anyCalibrated) calibChecking_ = false;
	auto checkOffer = [&] {
		if (!anyCalibrated) return;
		ImGui::Dummy(ImVec2(0, px(2)));
		if (calibChecking_) {
			if (ImGui::SmallButton(ICON_ARROW_LEFT "  Back to calibrating")) calibChecking_ = false;
		}
		else if (ghostButton(ICON_BULLSEYE "  Check it with a second drive", ImVec2(-FLT_MIN, 0))) {
			calibChecking_ = true;
			calibPickId_ = 0;   // a check needs a new pass, not the one just used
			calibSpeed_ = calib::checkTarget(calibSpeed_);   // suggest another speed; the user can pick any
			calibSpeedSet_ = true;
			dashFor_ = 0;
		}
	};
	// A pass can be checked once its direction has a calibration, and never against the pass that calibration was
	// made from (it would agree with itself to the rounding).  Returns why not, or nullptr.
	auto whyNoCheck = [&](bool l2r, int vehicleId) -> const char* {
		if ((l2r ? calL2R : calR2L).kind == CalState::None) return "Calibrate this direction first; then a second pass can check it.";
		if (vehicleId && vehicleId == calibAppliedId_) return "That's the pass the calibration came from. Pick a new pass.";
		return nullptr;
	};

	// A numbered step: the number in a tinted circle, the text wrapped beside it.
	auto stepLine = [&](int n, const char* text) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		dl->AddCircleFilled(p + ImVec2(px(11), px(11)), px(11), alpha(c::accent, .2f));
		std::string s = std::to_string(n);
		ImVec2 ts = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, s.c_str());
		dl->AddText(fonts.caption, fsz(fonts.caption), p + ImVec2(px(11), px(11)) - ts * .5f, c::accent, s.c_str());
		ImGui::SetCursorScreenPos(p + ImVec2(px(32), 0));
		ImGui::PushTextWrapPos(0);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, px(2)));
	};

	if (calibMode_ == 0) {
		beginCard("drive", calibChecking_ ? ICON_BULLSEYE : ICON_CAR, calibChecking_ ? "Check it with a second drive" : "Drive through at a known speed");
		// The steps name the target speed, so they read as instructions for this street rather than an example.
		targetSpeedChips();
		char stepText[320];
		if (calibChecking_) {
			snprintf(stepText, sizeof stepText, "Drive past again at a steady %d mph. A different speed from your first pass tests more.", calibSpeed_);
			stepLine(1, stepText);
			stepLine(2, "Pick that pass and enter the speed you drove. Within ±2 mph is verified.");
		}
		else {
			stepLine(1, "Start tracking (live, or record a video). Your street should already be marked (step 4).");
			snprintf(stepText, sizeof stepText, "Drive past at a steady %d mph, or at any steady speed while a Tesla dashcam or VST Ride-along on your "
				"phone records it. %s", calibSpeed_, g.LegalDirection == "Both" ? "Once each way." : "The legal way is enough.");
			stepLine(2, stepText);
			stepLine(3, "Pick your pass below, enter the speed you drove (or read it from those recordings), and apply.");
		}
		if (calib::belowCruiseControl(calibSpeed_)) {
			ImGui::PushTextWrapPos(0);
			dimText(ICON_INFO "  Most cruise control won't hold under 25 mph. Hold it steady by eye on the speedometer, "
				"or let VST Ride-along on your phone read the speed for you afterwards.");
			ImGui::PopTextWrapPos();
		}
		ImGui::Dummy(ImVec2(0, px(2)));
		auto vs = tracker.passesSince(0);
		if (vs.empty()) dimText("No vehicles measured yet this session. Run tracking first, then come back here.");
		else {
			// Five rows while checking (the newest passes are the likely ones), so the Current calibration card with
			// the per-direction verdicts stays in view below.
			ImGui::BeginChild("picklist", ImVec2(0, calibChecking_ ? ImGui::GetTextLineHeightWithSpacing() * 5 + ImGui::GetStyle().FramePadding.y * 2 : px(240)), ImGuiChildFlags_FrameStyle);
			for (auto it = vs.rbegin(); it != vs.rend(); ++it) {
				ImGui::PushID(it->id);
				char label[200];
				// The direction as the lane labels on the picture show it: the arrow and its compass heading ("← NW").
				snprintf(label, sizeof label, "%s %s  %d frames  measured %d mph%s  %s%s",
					it->ev.leftToRight ? "→" : "←", (it->ev.leftToRight ? cfg.g.L2RDirection : cfg.g.R2LDirection).c_str(), it->frames(), it->ev.speed, it->belowMin ? " · slow" : "",
					it->time.substr(11).c_str(), it->id == calibAppliedId_ ? "  · calibrated from" : "");
				if (ImGui::Selectable(label, calibPickId_ == it->id)) calibPickId_ = it->id;
				if (it->belowMin && ImGui::IsItemHovered())
					ImGui::SetTooltip("Slower than Slowest logged (%d mph), so it isn't in the csv or Results,\n"
						"but it calibrates as well as any other pass.", g.minLoggedSpeed);
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
		formLabel("I drove at (mph)", px(130));
		if (speedStepper("##spd", &calibSpeed_)) { calibSpeedSet_ = true; dashFor_ = 0; }
		Vehicle picked;
		bool have = calibPickId_ > 0 && tracker.vehicle(calibPickId_, picked);
		dashcamSpeed(have ? &picked : nullptr);
		if (calibChecking_) {
			const char* why = have ? whyNoCheck(picked.ev.leftToRight, picked.id) : nullptr;
			ImGui::BeginDisabled(!have || why);
			if (primaryButton(ICON_BULLSEYE "  Check this pass", ImVec2(-FLT_MIN, 0))) {
				checkCalibration(picked.ev.leftToRight, drivenMph(picked.id), picked.frames(), picked.fps);
				markCalibrationPass(picked, true, drivenMph(picked.id));
			}
			ImGui::EndDisabled();
			if (why) { ImGui::PushTextWrapPos(0); dimText("%s", why); ImGui::PopTextWrapPos(); }
		}
		else {
			ImGui::BeginDisabled(!have);
			if (primaryButton(ICON_CHECK "  Use this pass", ImVec2(-FLT_MIN, 0))) {
				applyCalibration(picked.ev.leftToRight, drivenMph(picked.id), picked.frames(), picked.fps);
				markCalibrationPass(picked, false, drivenMph(picked.id));
				calibAppliedId_ = picked.id;
			}
			ImGui::EndDisabled();
		}
		checkOffer();
		endCard();
	}
	else if (calibMode_ == 2) {
		// No car needed: the zone's length from two cones a taped distance apart.  It's only as good as where the cones
		// stand (the cars' near side, at the lane's depth), so it counts as a rough start until a drive past checks it.
		beginCard("cones", ICON_RULER, "Two spots a measured distance apart");
		stepLine(1, "Chalk two spots 10 ft or more apart along the lane, where cars' near wheels run, one at each white line. "
			"Stand a cone on each, or have one person hold a pole (or a broom) upright on one spot, then the other.");
		stepLine(2, "With no car in the way, mark each spot on the picture, here or on the phone companion from the street. "
			"Any point on an upright pole will do, so it works even where the road surface is hidden.");
		stepLine(3, "Enter the distance and add it to your calibration. A drive past later cross-checks it.");
		ImGui::Dummy(ImVec2(0, px(4)));
		// On your own, or with the phone busy being the camera: two timed pictures (live cameras), or the recording.
		bool live = player.isOpen() && (player.info().kind == SourceInfo::Camera || player.info().kind == SourceInfo::Stream);
		if (live && coneTimerAt_ < 0) {
			if (ghostButton(ICON_STOPWATCH "  On your own? Take two timed pictures", ImVec2(-FLT_MIN, 0))) {
				coneShot_[0].release();
				coneShot_[1].release();
				coneTimerAt_ = now();
				coneTexOf_ = -1;
				conePlacing_ = -1;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Press it, walk out, and hold the pole upright on spot 1 for the first picture (%.0f s from now),\n"
					"then on spot 2 for the second (%.0f s after that). Come back and click the pole in each picture.", kConeFirstS, kConeGapS);
		}
		else if (live) {
			ImGui::PushTextWrapPos(0);
			dimText(ICON_STOPWATCH "  Hold the pole on spot 1 for the first picture, then on spot 2 for the second, %.0f s later.", kConeGapS);
			ImGui::PopTextWrapPos();
			if (ImGui::SmallButton("Cancel the timer")) coneTimerAt_ = -1;
		}
		else if (player.isOpen() && player.seekable()) {
			ImGui::PushTextWrapPos(0);
			dimText(ICON_FILM "  A recording works too: step to where the pole stands on spot 1 and mark it, then to spot 2.");
			ImGui::PopTextWrapPos();
		}
		if (coneShots) {
			static const char* const views[] = { "Camera", "Spot 1 picture", "Spot 2 picture" };
			int sel = coneView_ + 1;
			if (segmented("coneview", &sel, views, 3)) coneView_ = sel - 1;
		}
		float bw = (ImGui::GetContentRegionAvail().x - px(10)) * .5f;
		ImGui::BeginDisabled(!player.isOpen() && !coneShots);
		for (int i = 0; i < 2; i++) {
			if (i) ImGui::SameLine(0, px(10));
			std::string label = std::string(conePlacing_ == i ? ICON_CROSSHAIRS "  Click the picture" : ICON_FLAG "  Mark spot ")
				+ (conePlacing_ == i ? "" : std::to_string(i + 1)) + "##cone" + std::to_string(i);
			if (conePlacing_ == i ? primaryButton(label.c_str(), ImVec2(bw, 0)) : ghostButton(label.c_str(), ImVec2(bw, 0))) {
				conePlacing_ = conePlacing_ == i ? -1 : i;
				if (coneShots && conePlacing_ >= 0) coneView_ = conePlacing_;
			}
		}
		ImGui::EndDisabled();
		auto coneText = [&](int i) { return coneX_[i] < 0 ? std::string("not marked") : "x " + std::to_string(int(std::lround(coneX_[i]))); };
		dimText("Spot 1: %s    Spot 2: %s", coneText(0).c_str(), coneText(1).c_str());
		formLabel("Spots apart (ft)", px(130));
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputDouble("##coneft", &coneFt_, .5, 1, "%.1f");
		coneFt_ = std::clamp(coneFt_, 0.0, 500.0);
		static const char* const coneDirs[] = { "Both ways", "→ only", "← only" };
		segmented("conedir", &coneDir_, coneDirs, 3);
		if (coneDir_ == 0 && g.LegalDirection == "Both") {
			ImGui::PushTextWrapPos(0);
			dimText(ICON_INFO "  On a two-way street each lane is a different distance from the camera. Mark spots in one lane, "
				"apply that direction, then do the other lane.");
			ImGui::PopTextWrapPos();
		}
		double markPx = (coneX_[0] >= 0 && coneX_[1] >= 0) ? std::fabs(coneX_[1] - coneX_[0]) : 0;
		double zonePx = g.speedLineRight - g.speedLineLeft;
		double framesD = calib::framesForDistance(markPx, coneFt_, zonePx, g.CalibrationSpeed, g.CalibrationFPS);
		bool tooClose = markPx > 0 && markPx < 40;
		if (framesD > 0 && !tooClose) {
			double zoneFt = zonePx / markPx * coneFt_;
			int rounded = std::max(1, int(std::lround(framesD)));
			ImGui::PushFont(fonts.stat);
			ImGui::Text("%.1f ft zone", zoneFt);
			ImGui::PopFont();
			ImGui::PushTextWrapPos(0);
			dimText("A car at %.0f mph crosses it in %.1f frames at %.0f fps, saved as %d (%.1f%% from rounding).", g.CalibrationSpeed,
				framesD, g.CalibrationFPS, rounded, std::fabs(rounded - framesD) / framesD * 100);
			ImGui::PopTextWrapPos();
		}
		else if (tooClose) dimText(ICON_WARN "  The two spots are too close together on the picture. Space them further apart.");
		// Spots well inside the zone still work, but the zone's length is scaled up from them: a marking error grows by
		// zonePx / markPx, and with the camera off square a stretch of street doesn't take up the same pixels everywhere
		// across the frame.  Spots at the white lines measure the zone itself, so nudge toward them below about 60%.
		if (framesD > 0 && !tooClose && markPx < .6 * zonePx) {
			ImGui::PushTextWrapPos(0);
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
			ImGui::TextWrapped(ICON_INFO "  Your spots span %.0f%% of the zone. That works, but spots right at the two white lines are more "
				"accurate, especially with the camera at an angle to the street.", markPx / zonePx * 100);
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
		}
		ImGui::BeginDisabled(framesD <= 0 || tooClose);
		if (primaryButton(ICON_PLUS "  Add to calibration", ImVec2(-FLT_MIN, 0))) {
			char label[64];
			snprintf(label, sizeof label, "Cones or pole, %g ft apart", coneFt_);
			applyDistanceCalibration(coneDir_ != 2, coneDir_ != 1, framesD, "the cones", "cones", calib::sigmaCones(markPx), label);
		}
		ImGui::EndDisabled();
		if (ImGui::SmallButton("Clear spots")) { coneX_[0] = coneX_[1] = coneY_[0] = coneY_[1] = -1; conePlacing_ = -1; }
		if (anyCalibrated) {
			ImGui::Dummy(ImVec2(0, px(2)));
			if (ghostButton(ICON_BULLSEYE "  Check it with a drive past", ImVec2(-FLT_MIN, 0))) {
				calibMode_ = 0;
				calibChecking_ = true;
				calibPickId_ = 0;
			}
		}
		endCard();
	}
	else if (calibMode_ == 3) {
		// A car of known size as the measuring stick: the same arithmetic as the cones (calib::framesForDistance), with
		// two points on the car as the marks and the maker's figure as the distance.  What it adds over the cones is
		// convenience (no tape, no trip outside, and any car of a known model that shows up will do); what it costs is
		// accuracy, from three places the copy below steers around:
		//  - Where to click.  The wheel centres (the default) are two sharp points on the side facing the camera, so a
		//    car at a slight angle, rounded corners or a bike rack change nothing; bumpers are there for a car whose
		//    wheels are hidden.  See calib::kKnownCars.
		//  - Depth.  A parked car isn't where moving cars are, so its span is scaled to the travel lane (carScale): from
		//    the parking lane drawn in step 4 when there is one (ParkingLane.h; the page then says Parked or In the
		//    travel lane by itself), otherwise from "Parked" and how far beyond, through the camera model.  A passing
		//    car needs nothing.
		//  - The model's figures: the picker names each generation's years, since generations differ by inches.
		// The marks span the car, not the zone, so a camera not square to the street (the zone's ends at different
		// depths) adds error the cones placed at the white lines don't have.
		const bool oneWay = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
		const calib::ParkingLane park = parkingLane();
		const bool marked = carX_[0] >= 0 && carX_[1] >= 0;
		beginCard("car", ICON_CAR, "A car you know the model of");
		stepLine(1, "Find a frame with the car side-on, near the middle of the picture and as big as it gets. A parked car works, "
			"or step through a recording to a sharp frame of one passing by.");
		stepLine(2, carWheels_ ? "Click the centre of each wheel on the side facing you, left wheel first."
			: "Click the left bumper, then the right one, each where it ends on the side facing the camera.");
		stepLine(3, "Pick the model, check it says where the car is, and apply. Then check it with one drive past.");
		ImGui::Dummy(ImVec2(0, px(4)));

		// What to click, as a picture: the two marks and the span they measure.
		static const char* const how[] = { "Wheel centres", "Bumpers" };
		int hw = carWheels_ ? 0 : 1;
		if (segmented("carhow", &hw, how, 2) && carWheels_ != (hw == 0)) {
			carWheels_ = hw == 0;
			// The picked model gives the other figure; a typed one starts again from a typical car.
			carFt_ = carModel_ >= 0 ? (carWheels_ ? calib::kKnownCars[carModel_].wheelbaseIn : calib::kKnownCars[carModel_].inches) / 12
				: carWheels_ ? calib::kTypicalWheelbaseFt : 15.5;
			carX_[0] = carX_[1] = carY_[0] = carY_[1] = -1;
			conePlacing_ = -1;
		}
		{
			float sw = ImGui::GetContentRegionAvail().x, sh = px(84);
			ImVec2 at = ImGui::GetCursorScreenPos();
			wheelbaseSketch(ImGui::GetWindowDrawList(), at + ImVec2(sw * .08f, 0), ImVec2(sw * .84f, sh), carWheels_);
			ImGui::Dummy(ImVec2(sw, sh));
		}
		ImGui::PushTextWrapPos(0);
		dimText("%s", carWheels_
			? "The middle of each wheel is a sharp point at the lane line's height, so a car at a slight angle, rounded corners "
			  "or a bike rack don't matter. The easiest and most accurate choice."
			: "Only when the wheels are hidden. Bumpers are rounded, and at an angle the camera sees some of the front or back, "
			  "which reads as extra length.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, px(2)));

		float bw = (ImGui::GetContentRegionAvail().x - px(10)) * .5f;
		ImGui::BeginDisabled(!player.isOpen());
		for (int i = 0; i < 2; i++) {
			if (i) ImGui::SameLine(0, px(10));
			const char* name = carWheels_ ? (i == 0 ? "Left wheel" : "Right wheel") : (i == 0 ? "Left bumper" : "Right bumper");
			std::string label = std::string(conePlacing_ == i ? ICON_CROSSHAIRS "  Click the picture" : std::string(ICON_FLAG "  ") + name)
				+ "##car" + std::to_string(i);
			if (conePlacing_ == i ? primaryButton(label.c_str(), ImVec2(bw, 0)) : ghostButton(label.c_str(), ImVec2(bw, 0)))
				conePlacing_ = conePlacing_ == i ? -1 : i;
		}
		ImGui::EndDisabled();
		auto carText = [&](int i) { return carX_[i] < 0 ? std::string("not marked") : "x " + std::to_string(int(std::lround(carX_[i]))); };
		dimText("Left: %s    Right: %s", carText(0).c_str(), carText(1).c_str());

		// The model picker fills the span; typing one switches the picker to "typed in".
		formLabel("Model", px(130));
		ImGui::SetNextItemWidth(-FLT_MIN);
		const char* shownModel = carModel_ >= 0 ? calib::kKnownCars[carModel_].name : "Other (type it)";
		if (ImGui::BeginCombo("##carmodel", shownModel, ImGuiComboFlags_HeightLarge)) {
			for (int i = 0; i < int(IM_ARRAYSIZE(calib::kKnownCars)); i++) {
				char item[112];
				double in = carWheels_ ? calib::kKnownCars[i].wheelbaseIn : calib::kKnownCars[i].inches;
				snprintf(item, sizeof item, "%s   %d ft %d in", calib::kKnownCars[i].name, int(in) / 12, int(in) % 12);
				if (ImGui::Selectable(item, carModel_ == i)) { carModel_ = i; carFt_ = in / 12; }
			}
			if (ImGui::Selectable("Other (type it)", carModel_ < 0)) carModel_ = -1;
			ImGui::EndCombo();
		}
		formLabel(carWheels_ ? "Wheelbase (ft)" : "Length (ft)", px(130));
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));   // room for the help marker
		if (ImGui::InputDouble("##carft", &carFt_, .1, 1, "%.2f")) carModel_ = -1;
		carFt_ = std::clamp(carFt_, 0.0, 80.0);
		helpMarker(carWheels_
			? "Front wheel centre to rear wheel centre. Search the year and model with \"wheelbase\"; makers list it in inches\n"
			  "(divide by 12). An inch either way is about 0.9%, or 0.2 mph at 25 mph."
			: "Bumper to bumper. Search the year and model with \"overall length\"; makers list it in inches (divide by 12).\n"
			  "An inch either way is about half a percent, or 0.1 mph at 25 mph.");

		// Where the car is.  With a parking lane drawn (step 4) the marks' rows say it, and the choice follows each new
		// pair of marks; a click on the other option overrides it until the car is marked again.  Without one it is the
		// user's call, with how far beyond the moving cars a parked car sits.
		const bool primaryL2R = oneWay ? g.LegalDirection == "L2R" : coneDir_ != 2;
		calib::CarSpot spot = carSpot(primaryL2R);
		bool newMarks = !std::equal(carX_, carX_ + 2, carAutoFor_) || !std::equal(carY_, carY_ + 2, carAutoFor_ + 2);
		if (marked && park.set() && newMarks && spot.where != calib::CarSpot::Unknown) {
			carParked_ = spot.where == calib::CarSpot::Parked;
			std::copy(carX_, carX_ + 2, carAutoFor_);
			std::copy(carY_, carY_ + 2, carAutoFor_ + 2);
		}
		static const char* const where[] = { "In the travel lane", "Parked" };
		int w = carParked_ ? 1 : 0;
		if (segmented("carwhere", &w, where, 2)) carParked_ = w == 1;
		const double midX = marked ? (carX_[0] + carX_[1]) * .5 : 640;
		ImGui::PushTextWrapPos(0);
		if (park.set()) {
			if (!marked) dimText(ICON_INFO "  Mark the car and VST says whether it's parked, from the parking lane you drew in step 4.");
			else if (spot.offLines) {
				ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
				ImGui::TextWrapped(ICON_WARN "  The marks aren't on the lane line or the parking lane. Click the middle of each wheel, "
					"or move the lines in step 4 if they've drifted.");
				ImGui::PopStyleColor();
			}
			else {
				bool autoParked = spot.where == calib::CarSpot::Parked;
				ImGui::PushStyleColor(ImGuiCol_Text, v4(carParked_ ? c::parking : c::ok));
				if (carParked_ == autoParked)
					ImGui::TextWrapped(carParked_ ? ICON_CAR "  Parked: its wheels are on the parking lane." : ICON_CHECK_CIRCLE "  In the travel lane: its wheels are on the lane line.");
				else ImGui::TextWrapped(ICON_INFO "  You chose %s; its wheels look %s.", carParked_ ? "parked" : "in the travel lane", autoParked ? "parked" : "in the travel lane");
				ImGui::PopStyleColor();
			}
			if (carParked_ && marked && std::isfinite(spot.parkedFt)) {
				const char* how2 = spot.fromPicture ? "from your picture and camera" : spot.typical ? "a typical guess" : "as you measured";
				dimText("Parked cars are about %.1f ft %s than moving ones (%s), so VST scales this car ×%.2f to the travel lane.",
					std::fabs(spot.parkedFt), spot.parkedFt >= 0 ? "farther" : "nearer", how2, spot.parkedScale);
			}
		}
		else {
			if (carParked_) {
				formLabel("Farther by (ft)", px(130));
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));   // room for the help marker
				ImGui::InputDouble("##carfar", &carFartherFt_, .5, 1, "%.1f");
				carFartherFt_ = std::clamp(carFartherFt_, -30.0, 50.0);
				helpMarker("From a passing car's wheels to the parked car's, across the street. Across half a lane and a parking\n"
					"lane that's usually 6 to 9 ft. A car parked between you and traffic is nearer: type it negative.");
				if (marked) dimText("VST scales this car ×%.2f to the travel lane, from your camera's height (step 4) and distance (step 2).", carScale(primaryL2R));
			}
			dimText(ICON_INFO "  Tip: draw the parking lane in step 4 and VST tells parked cars from passing ones by itself.");
			if (ImGui::SmallButton("Draw the parking lane in step 4##car")) goFlow(3);
		}
		ImGui::PopTextWrapPos();
		static const char* const carDirs[] = { "Both ways", "→ only", "← only" };
		segmented("cardir", &coneDir_, carDirs, 3);

		// Each direction's frames: on a two-way street the two lanes are at different depths, so a parked car scales
		// differently to each.  The card shows the primary direction (the legal one, or → unless "← only").
		const bool doL2R = coneDir_ != 2, doR2L = coneDir_ != 1;
		double rawPx = marked ? std::fabs(carX_[1] - carX_[0]) : 0;
		double zonePx = g.speedLineRight - g.speedLineLeft;
		auto framesFor = [&](bool l2r) { return calib::framesForDistance(rawPx * carScale(l2r), carFt_, zonePx, g.CalibrationSpeed, g.CalibrationFPS); };
		double framesD = framesFor(primaryL2R);
		bool tooSmall = rawPx > 0 && rawPx < 60;
		if (framesD > 0 && !tooSmall) {
			double zoneFt = zonePx / (rawPx * carScale(primaryL2R)) * carFt_;
			int rounded = std::max(1, int(std::lround(framesD)));
			ImGui::PushFont(fonts.stat);
			ImGui::Text("%.1f ft zone", zoneFt);
			ImGui::PopFont();
			ImGui::PushTextWrapPos(0);
			dimText("A car at %.0f mph crosses it in %.1f frames at %.0f fps, saved as %d.", g.CalibrationSpeed, framesD, g.CalibrationFPS, rounded);
			if (!oneWay && doL2R && doR2L) dimText("← lane: %.1f frames.", framesFor(false));
			if (carParked_ && !(park.set() && spot.fromPicture)) {
				// How much a foot of error in "how far beyond" moves the result, so the user knows whether to measure it.
				double ft = park.set() ? spot.parkedFt : carFartherFt_;
				double s0 = calib::scaleBeyond(cameraSetup(), midX, ft), s1 = calib::scaleBeyond(cameraSetup(), midX, ft + 1);
				if (s0 > 0) dimText("Each foot that distance is off moves speeds about %.0f%%.", std::fabs(s1 / s0 - 1) * 100);
			}
			ImGui::PopTextWrapPos();
		}
		else if (tooSmall) dimText(ICON_WARN "  The car is too small on the picture to measure. Pick a nearer one.");
		ImGui::BeginDisabled(framesD <= 0 || tooSmall);
		if (primaryButton(ICON_PLUS "  Add to calibration", ImVec2(-FLT_MIN, 0))) {
			// One input per direction it speaks for (a parked car scales differently to each lane of a two-way street),
			// weighted by how well the marks and the parked-car depth are known (calib::sigmaCar).
			const char* from = carWheels_ ? "the car's wheelbase" : "the car's length";
			const double laneFt = prefs.planDist * plan::kFt;
			const double fartherFt = park.set() && std::isfinite(spot.parkedFt) ? spot.parkedFt : carFartherFt_;
			const double sigma = calib::sigmaCar(rawPx, carParked_, laneFt, fartherFt);
			std::string label = std::string(carModel_ >= 0 ? calib::kKnownCars[carModel_].name : carWheels_ ? "A car's wheelbase" : "A car of known length")
				+ (carParked_ ? ", parked" : "");
			if (oneWay || !(doL2R && doR2L)) applyDistanceCalibration(doL2R, doR2L, framesD, from, "car", sigma, label);
			else {
				applyDistanceCalibration(true, false, framesFor(true), from, "car", sigma, label + ", →");
				applyDistanceCalibration(false, true, framesFor(false), from, "car", sigma, label + ", ←");
			}
		}
		ImGui::EndDisabled();
		if (ImGui::SmallButton(carWheels_ ? "Clear wheels##car" : "Clear bumpers##car")) { carX_[0] = carX_[1] = carY_[0] = carY_[1] = -1; conePlacing_ = -1; }
		if (anyCalibrated) {
			ImGui::Dummy(ImVec2(0, px(2)));
			if (ghostButton(ICON_BULLSEYE "  Check it with a drive past##car", ImVec2(-FLT_MIN, 0))) {
				calibMode_ = 0;
				calibChecking_ = true;
				calibPickId_ = 0;
			}
		}
		endCard();
	}
	else {
		beginCard("manual", calibChecking_ ? ICON_BULLSEYE : ICON_STOPWATCH, calibChecking_ ? "Check it with a second pass" : "Count the frames yourself");
		ImGui::PushTextWrapPos(0);
		dimText(calibChecking_ ? "Find a second pass in the video, ideally at a different speed. Clear the marks, mark it the same way and enter the speed you drove."
			: "Step through your calibration video frame by frame. Mark when the front bumper touches the first line, then the second.");
		ImGui::PopTextWrapPos();
		static const char* const dirs[] = { "→  Left to right", "←  Right to left" };
		segmented("cdir", &calibDir_, dirs, 2);
		ImGui::Dummy(ImVec2(0, px(4)));
		int* m = calibMarks[calibDir_];
		float bw = (ImGui::GetContentRegionAvail().x - px(10)) * .5f;
		ImGui::BeginDisabled(!player.seekable());
		// The buttons always say what they do; the marks so far show underneath, so a set mark never looks like
		// a finished step.
		if (ghostButton(ICON_FLAG "  Bumper at line 1", ImVec2(bw, 0))) m[0] = player.position();
		ImGui::SameLine(0, px(10));
		if (ghostButton(ICON_FLAG "  Bumper at line 2", ImVec2(bw, 0))) m[1] = player.position();
		ImGui::EndDisabled();
		dimText("Line 1: %s    Line 2: %s", m[0] >= 0 ? ("frame " + std::to_string(m[0])).c_str() : "not marked",
			m[1] >= 0 ? ("frame " + std::to_string(m[1])).c_str() : "not marked");
		int frames = (m[0] >= 0 && m[1] > m[0]) ? m[1] - m[0] : 0;
		if (frames) {
			ImGui::PushFont(fonts.stat);
			ImGui::Text("%d frames", frames);
			ImGui::PopFont();
		}
		if (!calibSpeedSet_) calibSpeed_ = calib::defaultTarget(prefs.calibTarget, prefs.speedLimit);
		formLabel("I drove at (mph)", px(130));
		if (speedStepper("##spd2", &calibSpeed_)) calibSpeedSet_ = true;
		if (calibChecking_) {
			const char* why = whyNoCheck(calibDir_ == 0, 0);
			ImGui::BeginDisabled(!frames || why);
			if (primaryButton(ICON_BULLSEYE "  Check", ImVec2(-FLT_MIN, 0))) checkCalibration(calibDir_ == 0, calibSpeed_, frames, fps);
			ImGui::EndDisabled();
			if (why) { ImGui::PushTextWrapPos(0); dimText("%s", why); ImGui::PopTextWrapPos(); }
		}
		else {
			ImGui::BeginDisabled(!frames);
			if (primaryButton(ICON_PLUS "  Add to calibration", ImVec2(-FLT_MIN, 0))) applyCalibration(calibDir_ == 0, calibSpeed_, frames, fps);
			ImGui::EndDisabled();
		}
		if (ImGui::SmallButton("Clear marks")) m[0] = m[1] = -1;
		checkOffer();
		endCard();
	}

	ImGui::Dummy(ImVec2(0, px(4)));
	beginCard("calnow", ICON_GAUGE, "Your calibration");
	calibrationInputsCard(false);
	float half = (ImGui::GetContentRegionAvail().x - px(10)) * .5f;
	auto calTile = [&](const char* name, int frames, ImU32 col) {
		char v[32];
		snprintf(v, sizeof v, "%d frames", frames);
		char sub[64];
		snprintf(sub, sizeof sub, "at %.0f mph, %.0f fps", g.CalibrationSpeed, g.CalibrationFPS);
		statTile(ICON_CAR, name, v, sub, col, ImVec2(half, px(100)));
	};
	calTile("Left to right", g.CalibrationFramesL2R, c::l2r);
	ImGui::SameLine(0, px(10));
	calTile("Right to left", g.CalibrationFramesR2L, c::r2l);
	// Under each tile, how far that calibration has been shown to hold: the history's verdict for the direction.
	auto statePill = [&](const CalState& s) {
		char t[96];
		ImU32 col = c::dim;
		switch (s.kind) {
		case CalState::None: snprintf(t, sizeof t, ICON_WARN "  Not calibrated"); col = c::warn; break;
		case CalState::Unchecked: snprintf(t, sizeof t, ICON_CIRCLE "  Not checked"); break;
		case CalState::Verified: snprintf(t, sizeof t, ICON_CHECK_CIRCLE "  Verified (±%.1f mph)", std::fabs(s.error)); col = c::ok; break;
		case CalState::Failed: snprintf(t, sizeof t, ICON_X_CIRCLE "  Check failed (%+.1f mph)", s.error); col = c::bad; break;
		}
		pill(t, col, alpha(col, .14f), fonts.caption);
		if (ImGui::IsItemHovered()) {
			std::string tip = s.kind == CalState::None ? "No calibration for this direction yet."
				: s.kind == CalState::Unchecked ? "Calibrated" + (s.applied.empty() ? std::string(" before VST kept a history") : " " + s.applied.substr(0, 16))
					+ ", not yet checked with a second pass."
				: "Checked " + s.checked.substr(0, 16) + " with a second pass.";
			ImGui::SetTooltip("%s", tip.c_str());
		}
	};
	float rowX = ImGui::GetCursorPosX();
	statePill(calL2R);
	ImGui::SameLine(rowX + half + px(10));
	statePill(calR2L);
	// What a given crossing time reads as, so the numbers mean something.
	// Each direction has its own calibration, so each gets its own row of crossing times.
	dimText("Frames a car takes to cross the zone at each speed:");
	if (ImGui::BeginTable("speedtable", 5, ImGuiTableFlags_SizingStretchSame)) {
		const int speeds[] = { prefs.speedLimit, prefs.speedLimit + 10, prefs.speedLimit + 20, prefs.speedLimit + 30 };
		ImGui::TableNextColumn();
		for (int mph : speeds) {
			ImGui::TableNextColumn();
			ImGui::PushStyleColor(ImGuiCol_Text, v4(speedColor(mph, prefs.speedLimit, prefs.egregious)));
			ImGui::Text("%d mph", mph);
			ImGui::PopStyleColor();
		}
		for (int dir = 0; dir < 2; dir++) {
			int cal = dir == 0 ? g.CalibrationFramesL2R : g.CalibrationFramesR2L;
			ImGui::TableNextColumn();
			ImGui::PushStyleColor(ImGuiCol_Text, v4(dir == 0 ? c::l2r : c::r2l));
			ImGui::TextUnformatted(dir == 0 ? "→" : "←");
			ImGui::PopStyleColor();
			for (int mph : speeds) {
				ImGui::TableNextColumn();
				dimText("%d", int(std::lround(g.CalibrationSpeed * cal / mph)));
			}
		}
		ImGui::EndTable();
	}
	ImGui::PushTextWrapPos(0);
	dimText(calibrationDone() ? ICON_CHECK "  Calibration is saved with your settings."
		: "Not calibrated for this camera yet. Apply one pass above; it saves automatically.");
	dimText(ICON_FILE_CSV "  History: calibration-history.csv");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every calibration and check is appended to\n%s", calib::historyPath().c_str());
	ImGui::PopTextWrapPos();
	endCard();
	ImGui::EndChild();
}

// The Calibrate page's "Your calibration" list (CalEvidence.h): what every measurement added up to, in one line a
// novice can read ("3 measurements agree within 2%"), how sure that is, and each measurement with its share of the
// weight, how far it sits from the rest, and a button to take it out.  Taken-out ones stay listed with "Put back",
// and ones made on other speed lines are listed as not counted, so nothing ever vanishes without a trace.
void App::calibrationInputsCard(bool list) {
	const Globals& g = cfg.g;
	const std::vector<calib::Input>& inputs = calibrationInputs();
	calib::Combined both[2] = { calibrationCombined(true), calibrationCombined(false) };
	// One section for a one-way street whose two directions count the same inputs (the usual case: a pass there
	// measures the lane both ways); otherwise one per direction, since each lane is its own distance away.
	auto sameIds = [](const calib::Combined& a, const calib::Combined& b) {
		if (a.shares.size() != b.shares.size()) return false;
		for (size_t i = 0; i < a.shares.size(); i++) if (a.shares[i].id != b.shares[i].id) return false;
		return true;
	};
	std::vector<int> dirs;
	if (g.LegalDirection != "Both" && sameIds(both[0], both[1])) dirs.push_back(g.LegalDirection == "R2L" ? 1 : 0);
	else dirs = { 0, 1 };
	std::string toRemove, toRestore;

	for (int d : dirs) {
		const calib::Combined& cmb = both[d];
		ImGui::PushID(d);
		if (dirs.size() > 1) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(d == 0 ? c::l2r : c::r2l));
			ImGui::TextUnformatted(d == 0 ? "→  Left to right" : "←  Right to left");
			ImGui::PopStyleColor();
		}
		std::string detail;
		std::string head = calib::summary(cmb, &detail);
		bool good = cmb.valid && (cmb.n == 1 || cmb.agree());
		ImU32 col = !cmb.valid ? c::warn : good ? (cmb.n >= 2 ? c::ok : c::text) : c::warn;
		ImGui::PushFont(fonts.strong);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
		ImGui::PushTextWrapPos(0);
		ImGui::TextWrapped("%s  %s", !cmb.valid || !good ? ICON_WARN : cmb.n >= 2 ? ICON_CHECK_CIRCLE : ICON_CIRCLE, head.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::PushTextWrapPos(0);
		if (cmb.valid) {
			// The combination's uncertainty in the terms that matter: mph at the posted limit.  Whole-frame rounding
			// in the engine is on top of this, and shows on the tiles below.
			double limit = prefs.speedLimit > 0 ? prefs.speedLimit : 25;
			// The side panel's summary leaves the next step to the list under the picture.
			dimText("Good to about ±%.1f%%, so ±%.1f mph at %.0f mph. %s", cmb.sigma * 100, cmb.sigma * limit, limit, list ? detail.c_str() : "");
		}
		else if (!detail.empty() && list) dimText("%s", detail.c_str());
		ImGui::PopTextWrapPos();

		// The counted inputs, one line each: what it is, its share of the weight as a bar and a percent, how far it sits
		// from the result, and a button to take it out.
		if (list && cmb.valid && ImGui::BeginTable("calinputs", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthFixed, px(120));
			ImGui::TableSetupColumn("off", ImGuiTableColumnFlags_WidthFixed, px(48));
			ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, px(24));
			for (const calib::Share& sh : cmb.shares) {
				const calib::Input* in = nullptr;
				for (const calib::Input& e : inputs) if (e.id == sh.id) in = &e;
				if (!in) continue;
				ImGui::PushID(in->id.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, v4(sh.outlier ? c::warn : c::text));
				ImGui::Text("%s%s", sh.outlier ? ICON_WARN "  " : "", in->label.empty() ? calib::methodName(in->method) : in->label.c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s: says %.1f ft of street between the speed lines, give or take %.0f%%.\nAdded %s.%s",
						calib::methodName(in->method), in->zoneFt, in->sigma * 100, in->time.substr(0, 16).c_str(),
						sh.outlier ? "\nWell off the others: if you doubt it, take it out." : "");
				// Share of the weight: a bar and the percent beside it.
				ImGui::TableNextColumn();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float w = px(70), h = px(6), y = (ImGui::GetFrameHeight() - h) * .5f;
				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(p + ImVec2(0, y), p + ImVec2(w, y + h), c::bg3, h * .5f);
				dl->AddRectFilled(p + ImVec2(0, y), p + ImVec2(std::max(h, w * float(sh.contribution)), y + h), sh.outlier ? c::warn : c::accent, h * .5f);
				ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its share of the say in the result: surer measurements count for more.");
				ImGui::SameLine(0, px(8));
				ImGui::AlignTextToFramePadding();
				dimText("%.0f%%", sh.contribution * 100);
				ImGui::TableNextColumn();
				if (cmb.n >= 2) {
					ImGui::AlignTextToFramePadding();
					ImGui::PushStyleColor(ImGuiCol_Text, v4(sh.outlier ? c::warn : c::dim));
					ImGui::Text("%+.0f%%", sh.offPct);
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(sh.offPct >= 0 ? "Says the street is %.1f%% longer than the combined result:\nalone, it would read speeds that much higher."
						: "Says the street is %.1f%% shorter than the combined result:\nalone, it would read speeds that much lower.", std::fabs(sh.offPct));
				}
				ImGui::TableNextColumn();
				if (ImGui::SmallButton(ICON_XMARK)) toRemove = in->id;
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take this measurement out. You can put it back.");
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::PopID();
		ImGui::Dummy(ImVec2(0, px(4)));
	}

	if (!list) return;
	// Not counted: taken out by hand (with Put back), or made on speed lines that have moved since.
	std::string lines = speedLinesSig();
	int notCounted = 0;
	for (const calib::Input& in : inputs) notCounted += in.removed || (!in.lines.empty() && in.lines != lines);
	if (notCounted) {
		char h[64];
		snprintf(h, sizeof h, "Not counted (%d)###notcounted", notCounted);
		if (ImGui::TreeNode(h)) {
			for (const calib::Input& in : inputs) {
				bool moved = !in.lines.empty() && in.lines != lines;
				if (!in.removed && !moved) continue;
				ImGui::PushID(in.id.c_str());
				dimText("%s", in.label.empty() ? calib::methodName(in.method) : in.label.c_str());
				ImGui::SameLine();
				if (in.removed) { if (ImGui::SmallButton(ICON_UNDO "  Put back")) toRestore = in.id; }
				else {
					ImGui::PushFont(fonts.caption);
					dimText("(other speed lines)");
					ImGui::PopFont();
				}
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
	}
	if (!toRemove.empty()) {
		setCalibrationInputRemoved(toRemove, true);
		toast("Taken out. Under Not counted you can put it back.", c::dim, ICON_UNDO);
	}
	if (!toRestore.empty()) {
		setCalibrationInputRemoved(toRestore, false);
		toast("Put back into your calibration.", c::ok, ICON_CHECK_CIRCLE);
	}
}

// The latest check pass: what you drove beside what the calibration says, the error, and the verdict.  A failed
// check explains the usual causes and offers the two ways forward: calibrate again from scratch, or average the
// two passes (when both drives were honest, the mean is a better estimate than either).
void App::calibrationCheckCard(float width) {
	const CalCheck& k = lastCheck_;
	const Globals& g = cfg.g;
	beginCard("calcheck", ICON_BULLSEYE, k.l2r ? "Check result: left to right" : "Check result: right to left", ImVec2(width, 0));
	// Left: the three numbers side by side.  Right: the verdict in words and, for a failed check, what to do.
	float tileW = px(128), tileH = px(96), gap = px(8);
	char drove[16], said[16], err[16];
	snprintf(drove, sizeof drove, "%g", k.drivenMph);
	snprintf(said, sizeof said, "%.1f", k.measuredMph);
	snprintf(err, sizeof err, "%+.1f", k.error);
	ImU32 verdict = k.verified ? c::ok : c::bad;
	ImGui::BeginGroup();
	statTile(ICON_CAR, "You drove", drove, "mph", k.l2r ? c::l2r : c::r2l, ImVec2(tileW, tileH));
	ImGui::SameLine(0, gap);
	statTile(ICON_GAUGE, "VST says", said, "mph", c::accent, ImVec2(tileW, tileH));
	ImGui::SameLine(0, gap);
	statTile(k.verified ? ICON_CHECK_CIRCLE : ICON_WARN, "Error", err, "mph", verdict, ImVec2(tileW, tileH));
	ImGui::EndGroup();
	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	float textW = ImGui::GetContentRegionAvail().x;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW);
	ImGui::PushFont(fonts.strong);
	ImGui::PushStyleColor(ImGuiCol_Text, v4(verdict));
	if (k.verified) ImGui::Text(ICON_CHECK_CIRCLE "  Verified: within ±%.0f mph of what you drove", calib::kVerifiedMph);
	else ImGui::Text(ICON_WARN "  Off by %.1f mph, more than ±%.0f mph", std::fabs(k.error), calib::kVerifiedMph);
	ImGui::PopStyleColor();
	ImGui::PopFont();
	if (k.verified)
		dimText("A second, independent pass agrees with the calibration, and both passes are recorded in calibration-history.csv. "
			"If you can, check the other direction too.");
	else {
		dimText("Likely causes:");
		for (const char* t : {
			"Speedometer error: most read high. A GPS speed app is a better reference.",
			"The zones moved: the camera or the speed lines shifted since the first pass.",
			"A different lane: the far lane crosses more road between the lines." }) {
			// Hanging indent: a wrapped line starts under the text, not under the bullet.
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
			ImGui::TextUnformatted("•");
			ImGui::SameLine(0, px(8));
			ImGui::TextWrapped("%s", t);
			ImGui::PopStyleColor();
		}
	}
	ImGui::PopTextWrapPos();
	if (!k.verified) {
		// What adding the check pass would set: the inputs with this pass added, combined, so the button says exactly
		// what it does.  Two honest passes end up near their mean; with more inputs the check pass is one voice of many.
		std::vector<calib::Input> with = calibrationInputs();
		calib::Input pass;
		pass.method = "pass";
		pass.dirs = g.LegalDirection == "Both" ? (k.l2r ? calib::kL2R : calib::kR2L) : calib::kBothDirs;
		pass.zoneFt = calib::zoneFtFromPass(k.drivenMph, k.frames, k.fps);
		pass.sigma = calib::sigmaPass(k.drivenMph, k.frames);
		pass.lines = speedLinesSig();
		pass.time = "preview";
		calib::addInput(with, pass);
		calib::Combined after = calib::combine(with, k.l2r, speedLinesSig());
		int next = std::max(1, int(std::lround(calib::framesForZone(after.zoneFt, g.CalibrationSpeed, g.CalibrationFPS))));
		ImGui::Dummy(ImVec2(0, px(2)));
		float bw = (textW - px(10)) * .5f;
		if (ghostButton(ICON_ROTATE "  Drive again", ImVec2(bw, 0))) {
			calibChecking_ = false;
			calibPickId_ = 0;
			lastCheck_ = CalCheck();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave this pass out. Drive past again and add or check that one.\n"
			"A measurement you doubt can be removed under Your calibration.");
		ImGui::SameLine(0, px(10));
		char label[64];
		snprintf(label, sizeof label, ICON_PLUS "  Add this pass: %d frames", next);
		if (primaryButton(label, ImVec2(bw, 0))) averageCalibration(k);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Count this pass as one more measurement. VST weighs it with the %d already there\n"
				"(%s), giving %d frames. Then check it with one more pass.", after.n - 1, calib::summary(calibrationCombined(k.l2r)).c_str(), next);
	}
	ImGui::EndGroup();
	endCard();
}

// ======================================================================================================
// Live tracking
// ======================================================================================================

void App::pageTrack() {
	float avail = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
	float sideW = px(380);
	float canvasW = avail - sideW - px(20);
	bool run = tracker.running();
	auto prog = tracker.progress();
	float stripH = px(84) + px(30);
	float logClosed = px(40);

	ImGui::BeginGroup();
	float canvasH = std::min(availH - stripH - logClosed - px(20), canvasW * 9 / 16 + px(30));
	ImGui::BeginChild("tcanvas", ImVec2(canvasW, canvasH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	bool haveLive = liveTex_.valid() && (run || live_.serial > 0);
	void* tex = haveLive ? liveTex_.imguiId() : player.isOpen() ? player.texture().imguiId() : nullptr;
	ImVec2 area = ImGui::GetContentRegionAvail();
	area.y -= px(30);
	bool noSource = source.empty() && batch.empty();
	View v = drawFrame(tex, area, noSource ? "Connect a camera or open a video in step 3" : "Press Start tracking to begin");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// Motion the tracker sees, tinted over the analysis box, under the zone and box overlays.
	if (prefs.showMotion && run && motionTex_.valid()) {
		const cv::Rect& ab = live_.analysisBox;
		dl->AddImage((ImTextureID)(intptr_t)motionTex_.imguiId(), v.at(float(ab.x), float(ab.y)), v.at(float(ab.x + ab.width), float(ab.y + ab.height)));
	}
	OverlayStyle st;
	st.opacity = run ? .3f : .5f;   // while tracking, the zones step back so the vehicles and their speeds read first
	st.dimOutside = true;
	if (tex) zoneOverlay(v, cfg.g, st);
	if (haveLive) trackOverlay(v, cfg.g, live_.analysisBox, live_.boxes, prefs.speedLimit, prefs.egregious);

	// HUD: source badge, frame counter and how fast processing is going relative to real time.
	{
		ImVec2 p = v.origin + ImVec2(px(14), v.size.y - px(14) - fsz(fonts.strong) - px(10));
		const char* badge = !run ? "READY" : prog.reconnecting ? "RECONNECTING…" : prog.live ? "● LIVE" : tracker.paused() ? "PAUSED" : "● PLAYING";
		ImU32 bc = !run ? c::faint : prog.reconnecting ? c::warn : prog.live ? c::bad : tracker.paused() ? c::warn : c::ok;
		ImVec2 bs = fonts.strong->CalcTextSizeA(fsz(fonts.strong), FLT_MAX, 0, badge);
		dl->AddRectFilled(p, p + bs + ImVec2(px(18), px(10)), alpha(c::bg0, .78f), px(7));
		dl->AddText(fonts.strong, fsz(fonts.strong), p + ImVec2(px(9), px(5)), bc, badge);
		if (run) {
			char info[256];
			double factor = prog.videoFps > 0 ? prog.procFps / prog.videoFps : 0;
			// Files also show how far through they are (and which video of a batch); live sources have no end.
			std::string done = prog.totalFrames > 0 ? "   ·   " + std::string(prog.jobs > 1 ? "video " + std::to_string(prog.job + 1) + " of "
				+ std::to_string(prog.jobs) + ", " : "") + std::to_string(int(std::clamp(prog.frame / float(prog.totalFrames), 0.f, 1.f) * 100)) + "%" : "";
			if (prog.reconnecting)   // the engine's line, e.g. "Reconnecting to rtsp://cam/stream: attempt 4 in 8 s"
				snprintf(info, sizeof info, "%s", prog.sourceStatus.c_str());
			else
				snprintf(info, sizeof info, "%s   ·   frame %d   ·   %.0f fps   ·   %.1f× real time%s", prog.label.c_str(), prog.frame, prog.procFps, factor, done.c_str());
			ImVec2 ip = p + ImVec2(bs.x + px(26), 0);
			ImVec2 is = fonts.body->CalcTextSizeA(fsz(fonts.body), FLT_MAX, 0, info);
			dl->AddRectFilled(ip, ip + is + ImVec2(px(18), px(10)), alpha(c::bg0, .78f), px(7));
			dl->AddText(fonts.body, fsz(fonts.body), ip + ImVec2(px(9), px(5)), c::text, info);
		}
	}
	// Big call to action when idle.
	if (!run) {
		ImVec2 mid = v.origin + v.size * .5f;
		ImVec2 bsz(px(220), px(52));
		ImGui::SetCursorScreenPos(mid - bsz * .5f + ImVec2(0, px(40)));
		if (noSource ? primaryButton(ICON_VIDEO "  Choose a source", bsz) : primaryButton(ICON_PLAY "  Start tracking", bsz)) {
			if (noSource) page = Page::Source;
			else startTracking();
		}
	}
	// Progress for files: a bar along the bottom edge of the picture, with the percentage in the HUD above
	// (the canvas child has no room below the picture, which used to clip the percentage).
	if (run && prog.totalFrames > 0) {
		float frac = std::clamp(prog.frame / float(prog.totalFrames), 0.f, 1.f);
		ImVec2 p(v.origin.x, v.origin.y + v.size.y - px(5));
		dl->AddRectFilled(p, p + ImVec2(v.size.x, px(5)), alpha(c::bg0, .7f));
		dl->AddRectFilled(p, p + ImVec2(v.size.x * frac, px(5)), c::accent);
	}
	ImGui::EndChild();

	// Filmstrip of the latest vehicles, newest first.
	sectionLabel("JUST MEASURED");
	ImGui::BeginChild("strip", ImVec2(canvasW, px(84) + ImGui::GetStyle().ScrollbarSize), 0, ImGuiWindowFlags_HorizontalScrollbar);
	{
		auto recent = tracker.vehiclesSince(0, 24);
		if (recent.empty()) {
			ImGui::SetCursorPos(ImVec2(px(14), px(12)));
			dimText("Vehicles appear here the moment they leave the speed zone. Click one for details.");
		}
		// As many whole cards as fit across the picture, the rest a scroll away, so none is cut off at the edge.
		int fit = std::max(1, int((canvasW + px(10)) / px(290)));
		float cardW = (canvasW - px(10) * (fit - 1)) / fit;
		bool first = true;
		for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
			if (!first) ImGui::SameLine(0, px(10));
			first = false;
			ImGui::BeginGroup();
			vehicleCard(*it, cardW);
			ImGui::EndGroup();
		}
	}
	ImGui::EndChild();

	// Engine log drawer, taking whatever height is left.
	float logH = logOpen_ ? std::max(logClosed, ImGui::GetContentRegionAvail().y) : logClosed;
	beginCard("log", nullptr, nullptr, ImVec2(canvasW, logH), logOpen_);
	if (ImGui::Selectable(logOpen_ ? ICON_TERMINAL "  Tracker log   " ICON_CHEVRON_DOWN : ICON_TERMINAL "  Tracker log   " ICON_CHEVRON_RIGHT, false)) logOpen_ = !logOpen_;
	if (logOpen_) {
		ImGui::PushFont(fonts.caption);
		for (auto& l : tracker.logTail(200)) {
			bool warn = l.find("WRONG") != std::string::npos || l.find("Can't") != std::string::npos || l.find("Warning") != std::string::npos;
			ImGui::PushStyleColor(ImGuiCol_Text, v4(warn ? c::warn : c::dim));
			ImGui::TextUnformatted(l.c_str());
			ImGui::PopStyleColor();
		}
		if (run) ImGui::SetScrollHereY(1);
		ImGui::PopFont();
	}
	endCard();
	ImGui::EndGroup();

	// Right side: counts, run options, live feed.
	ImGui::SameLine(0, px(20));
	ImGui::BeginChild("tside", ImVec2(sideW, availH), 0, 0);
	Counts cn = tracker.counts();
	// From the room left by the scrollbar, which appears once the cards below outgrow the window.
	float tw = (ImGui::GetContentRegionAvail().x - px(12)) * .5f, th = px(104);
	char sub[64];
	// Vehicles counts the cars that got a speed; "of N tried" says how many crossed into the zone, so a view where cars
	// are seen but not timed reads as that rather than as a tracker counting nothing.
	if (cn.vehicles && cn.tries > cn.vehicles) snprintf(sub, sizeof sub, "of %d tried · avg %.0f mph", cn.tries, cn.avgSpeed);
	else if (cn.vehicles) snprintf(sub, sizeof sub, "avg %.0f mph", cn.avgSpeed);
	else if (cn.tries) snprintf(sub, sizeof sub, "%d tried, none timed yet", cn.tries);
	else snprintf(sub, sizeof sub, "none yet");
	statTile(ICON_CAR, "Vehicles", std::to_string(cn.vehicles), sub, c::accent, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "%d%% of traffic", cn.vehicles ? cn.speeders * 100 / cn.vehicles : 0);
	statTile(ICON_GAUGE, "Speeding", std::to_string(cn.speeders), sub, c::warn, ImVec2(tw, th));
	snprintf(sub, sizeof sub, "%d+ mph", prefs.egregious);
	statTile(ICON_BOLT, "Way over", std::to_string(cn.egregious), sub, c::bad, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	if (cfg.g.LegalDirection != "Both") statTile(ICON_WARN, "Wrong way", std::to_string(cn.wrongWay), "on a one-way street", c::wrong, ImVec2(tw, th));
	else statTile(ICON_FLAG, "Top speed", cn.topSpeed ? std::to_string(cn.topSpeed) + " mph" : "—", "this session", c::accent2, ImVec2(tw, th));
	// Where the tries went, once some didn't get a speed: the first thing to look at when the counts seem low.
	if (cn.tries > cn.vehicles) {
		ImGui::Dummy(ImVec2(0, px(2)));
		ImGui::PushTextWrapPos(0);
		std::string why = std::to_string(cn.tries) + " tried: " + std::to_string(cn.vehicles) + " timed";
		if (cn.slow) why += ", " + std::to_string(cn.slow) + " under " + std::to_string(cfg.g.minLoggedSpeed) + " mph (walkers, bikes; not listed)";
		if (cn.untimed) why += ", " + std::to_string(cn.untimed) + " lost in the zone before a speed";
		dimText("%s", (ICON_INFO "  " + why + ".").c_str());
		if (cn.untimed * 3 > cn.tries && cn.tries >= 5)
			dimText("Many lost? Check the lane line sits at hubcap height along the whole street, and the box leaves out parked cars and shadows.");
		ImGui::PopTextWrapPos();
	}

	ImGui::Dummy(ImVec2(0, px(4)));
	runOptions();
	ImGui::Dummy(ImVec2(0, px(4)));
	healthCard();

	ImGui::Dummy(ImVec2(0, px(4)));
	// The fastest this session, the list people ask about first.
	beginCard("fastest", ICON_BOLT, "Fastest this session");
	{
		auto all = tracker.vehiclesSince(0);
		std::sort(all.begin(), all.end(), [](const Vehicle& a, const Vehicle& b) { return a.ev.speed > b.ev.speed; });
		if (all.empty()) dimText("Nothing measured yet.");
		for (size_t i = 0; i < all.size() && i < 5; i++) {
			const Vehicle& v = all[i];
			ImGui::PushID(v.id);
			ImU32 col = v.ev.wrongWay ? c::wrong : speedColor(v.ev.speed, prefs.speedLimit, prefs.egregious);
			linePill((std::to_string(v.ev.speed) + " mph").c_str(), c::bg0, col, fonts.strong);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, v4(v.ev.leftToRight ? c::l2r : c::r2l));
			ImGui::Text("%s %s", v.ev.leftToRight ? "→" : "←", v.ev.compass.c_str());
			ImGui::PopStyleColor();
			if (v.ev.wrongWay) { ImGui::SameLine(); linePill(ICON_WARN " wrong way", c::rgb(0xFFFFFF), c::wrong); }
			if (v.large) { ImGui::SameLine(); ImGui::TextUnformatted(ICON_TRUCK); }
			ImGui::PopID();
		}
	}
	endCard();
	ImGui::EndChild();
}

void App::runOptions() {
	bool run = tracker.running();
	beginCard("runopts", ICON_SLIDERS, "Run settings");
	formLabel("Speed limit", px(130));
	ImGui::BeginDisabled(run);
	int limitBefore = prefs.speedLimit;
	if (ImGui::InputInt("##limit", &prefs.speedLimit, 5, 5)) {
		prefs.speedLimit = std::clamp(prefs.speedLimit, 5, 80);
		prefs.egregious = std::max(prefs.egregious, prefs.speedLimit);
		followSpeedLimit(limitBefore);
	}
	formLabel("Way over from", px(130));
	if (ImGui::InputInt("##egr", &prefs.egregious, 5, 5)) prefs.egregious = std::max(prefs.egregious, prefs.speedLimit);
	ImGui::EndDisabled();
	// How fast a video file is fed to the tracker.  Real time sleeps until each frame's moment (Tracker::onFrame), so a
	// file plays like a camera; Fast reads frames as quickly as this computer can analyse them.  Speeds come from
	// frame counts, not the clock, so both give the same results; Fast just finishes sooner.  A live camera or stream
	// delivers frames as they happen, so the choice does nothing there: the control says so instead of offering it.
	const std::string& next = batch.empty() ? source : batch.front();
	bool live = run ? tracker.progress().live : (!next.empty() && classifySource(next) != SourceInfo::File);
	formLabel("Recordings play", px(130));
	int rt = prefs.realtime ? 0 : 1;
	static const char* const pace[] = { "Normal speed", "Full speed" };
	ImGui::BeginDisabled(live);
	if (segmented("pace", &rt, pace, 2)) { prefs.realtime = rt == 0; tracker.setRealtime(prefs.realtime); }
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Only for recorded videos. Both give the same speeds, because VST counts frames rather than timing them.\n"
			"Normal speed plays the video like watching it; full speed finishes a long recording sooner.");
	if (live) {
		ImGui::PushTextWrapPos(0);
		dimText(ICON_INFO "  A live camera always runs at its own pace, so this only matters for recorded videos.");
		ImGui::PopTextWrapPos();
	}
	ImGui::BeginDisabled(run);
	ImGui::Checkbox("Save a highlights video of speeders", &prefs.highlights);
	if (prefs.highlights) {
		formLabel("  from / to mph", px(130));
		ImGui::PushItemWidth((ImGui::GetContentRegionAvail().x - px(8)) * .5f);
		ImGui::InputInt("##hlmin", &prefs.hlMin, 0);
		ImGui::SameLine(0, px(8));
		ImGui::InputInt("##hlmax", &prefs.hlMax, 0);
		ImGui::PopItemWidth();
	}
	ImGui::EndDisabled();
	ImGui::Checkbox("Tint the motion the tracker sees", &prefs.showMotion);
	endCard();
}

void App::vehicleCard(const Vehicle& v, float width) {
	ImGui::PushID(v.id);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = px(84);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImU32 col = v.ev.wrongWay ? c::wrong : speedColor(v.ev.speed, prefs.speedLimit, prefs.egregious);
	dl->AddRectFilled(p, p + ImVec2(width, h), c::bg2, px(10));
	dl->AddRectFilled(p, p + ImVec2(px(4), h), col, px(10), ImDrawFlags_RoundCornersLeft);
	// Snapshot.
	float tw = px(150), th = h - px(16);
	ImVec2 tp = p + ImVec2(px(12), px(8));
	if (Texture* t = snapshotTexture(v.id)) {
		float s = std::max(tw / t->width(), th / t->height());
		ImVec2 uv = ImVec2(tw / (t->width() * s), th / (t->height() * s));
		ImVec2 uv0 = (ImVec2(1, 1) - uv) * .5f;
		dl->AddImageRounded((ImTextureID)(intptr_t)t->imguiId(), tp, tp + ImVec2(tw, th), uv0, uv0 + uv, IM_COL32_WHITE, px(6));
	}
	else dl->AddRectFilled(tp, tp + ImVec2(tw, th), c::bg3, px(6));
	// Speed and details.
	ImVec2 sp = tp + ImVec2(tw + px(14), -px(2));
	std::string speed = std::to_string(v.ev.speed);
	dl->AddText(fonts.stat, fsz(fonts.stat), sp, col, speed.c_str());
	ImVec2 ss = fonts.stat->CalcTextSizeA(fsz(fonts.stat), FLT_MAX, 0, speed.c_str());
	dl->AddText(fonts.caption, fsz(fonts.caption), sp + ImVec2(ss.x + px(4), ss.y - fsz(fonts.caption) - px(4)), c::dim, "mph");
	std::string dir = std::string(v.ev.leftToRight ? "→ " : "← ") + v.ev.compass + (v.large ? "   " ICON_TRUCK : "");
	dl->AddText(fonts.body, fsz(fonts.body), sp + ImVec2(0, ss.y + px(2)), v.ev.leftToRight ? c::l2r : c::r2l, dir.c_str());
	const std::string& at = v.passed();
	std::string when = at.size() > 11 ? at.substr(11) : at;
	ImVec2 ws = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, when.c_str());
	dl->AddText(fonts.caption, fsz(fonts.caption), p + ImVec2(width - ws.x - px(12), px(10)), c::faint, when.c_str());
	if (v.ev.wrongWay) {
		const char* ww = ICON_WARN " WRONG WAY";
		ImVec2 s = pillSize(ww);
		ImVec2 at = p + ImVec2(width - s.x - px(10), h - s.y - px(10));
		dl->AddRectFilled(at, at + s, c::wrong, s.y * .5f);
		dl->AddText(fonts.caption, fsz(fonts.caption), at + ImVec2(px(8), px(3)), c::rgb(0xFFFFFF), ww);
	}
	ImGui::Dummy(ImVec2(width, h));
	if (ImGui::IsItemClicked()) {
		showSessionResults();
		for (size_t i = 0; i < rows.size(); i++)
			if (rows[i].vehicleId == v.id) selectedRow_ = int(i);
		page = Page::Results;
	}
	ImGui::Dummy(ImVec2(0, px(2)));
	ImGui::PopID();
}

// ======================================================================================================
// Results
// ======================================================================================================

void App::pageResults() {
	float avail = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
	// Which rows to show on arrival.  This session's vehicles when there are any; otherwise, the first time Results is
	// opened in a launch, every saved run, so a reopened Studio shows the results it has rather than "0 of 0".
	if (rowsScope == ResultsScope::Session && rows.empty() && tracker.lastVehicleId() > 0) showSessionResults();
	if (!resultsOpened_ && !screenshotMode && rowsScope == ResultsScope::Session && rows.empty() && !tracker.running()) showSavedRuns();
	resultsOpened_ = true;

	// Toolbar: filter, where the rows come from, file actions.
	static const char* const filters[] = { "All", "Speeding", "Way over", "Wrong way" };
	// The file buttons keep their full labels; the filter gives up width first, so the row fits a 1366-1480 px laptop
	// window instead of pushing "Make report" off the edge.
	float actionsW = px(16) * 2 + px(180);
	for (const char* l : { ICON_FOLDER_OPEN " Open", ICON_ARCHIVE " Archive", ICON_FILM " Highlights", ICON_FOLDER " Folder", ICON_REPORT "  Make report" })
		actionsW += buttonWidth(l) + ImGui::GetStyle().ItemSpacing.x;
	segmented("rfilter", &resultsFilter_, filters, 4, std::clamp(avail - actionsW, px(300), px(440)));
	ImGui::SameLine(0, px(16));
	ImGui::SetNextItemWidth(px(180));
	ImGui::InputTextWithHint("##search", ICON_SEARCH "  min speed or source", searchBuf_, sizeof searchBuf_);
	ImGui::SameLine(0, px(16));
	if (ghostButton(ICON_FOLDER_OPEN " Open")) { archives_ = archive::list(dataDir()); publishedList_ = dataset::list(dataDir()); ImGui::OpenPopup("openmenu"); }
	openMenu();
	ImGui::SameLine();
	// Archive what's on screen.  Not an archive (it is one already), not nothing, and not while a run is still writing
	// the csv it would move.
	const char* whyNot = reviewing() ? "This is an archive already." : rows.empty() ? "Nothing to archive yet."
		: tracker.running() ? "Stop tracking first: the run is still writing its results." : nullptr;
	ImGui::BeginDisabled(whyNot != nullptr);
	if (ghostButton(ICON_ARCHIVE " Archive") || (openArchiveDialog_ && !whyNot)) {
		// A name that's ready to go: the site and the days the vehicles passed ("Locust Ave, 2016-01-30"), so a novice
		// can just press Archive.  Rows without a usable date fall back to today.
		archiveForm_ = ArchiveForm();
		std::string first, last;
		for (const ResultRow& r : rows) {
			if (r.time.size() < 10 || r.time[4] != '-') continue;
			std::string d = r.time.substr(0, 10);
			if (first.empty() || d < first) first = d;
			if (last.empty() || d > last) last = d;
		}
		std::string when = first.empty() ? calib::nowIso().substr(0, 10) : first == last ? first : first + " to " + last;
		std::string site = prefs.siteName == report::Settings().siteName ? std::string() : prefs.siteName;
		snprintf(archiveForm_.name, sizeof archiveForm_.name, "%s", (site.empty() ? "Results " + when : site + ", " + when).c_str());
		ImGui::OpenPopup("Archive these results");
	}
	openArchiveDialog_ = false;
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", whyNot ? whyNot : "Put these results away in a folder of their own, with snapshots, clips and the calibration, to look at again later.");
	archiveDialog();
	ImGui::SameLine();
	// While reviewing, Highlights and Folder show the archive's own.
	fs::path base = reviewing() ? fs::path(archive_.dir) : fs::path(dataDir());
	if (ghostButton(ICON_FILM " Highlights")) openWithSystem((base / "HiLites").string());
	ImGui::SameLine();
	if (ghostButton(ICON_FOLDER " Folder")) openWithSystem(reviewing() ? base.string() : (base / "Stats").string());
	ImGui::SameLine();
	if (primaryButton(ICON_REPORT "  Make report")) { reportForm_.saved.clear(); reportForm_.error.clear(); ImGui::OpenPopup("Street report"); }
	reportDialog();
	if (reviewing()) reviewBanner(avail);
	if (!dataset_.dir.empty()) datasetCover(avail);
	const int limit = resultsLimit(), wayOver = resultsWayOver();

	// Filtered view.
	int minSpeed = 0;
	std::string search = searchBuf_;
	try { if (!search.empty()) minSpeed = std::stoi(search); } catch (...) {}
	std::vector<const ResultRow*> shown;
	for (const ResultRow& r : rows) {
		if (resultsFilter_ == 1 && r.speed <= limit) continue;
		if (resultsFilter_ == 2 && r.speed < wayOver) continue;
		if (resultsFilter_ == 3 && !r.wrongWay) continue;
		if (minSpeed > 0 && r.speed < minSpeed) continue;
		if (!search.empty() && minSpeed == 0 && r.source.find(search) == std::string::npos) continue;
		if (setFilter_ && r.setId != setFilter_) continue;   // the set bar's "Only this set"
		shown.push_back(&r);
	}
	int ownDrives = int(std::count_if(rows.begin(), rows.end(), [](const ResultRow& r) { return r.calKind != 0; }));
	// The menu beside it names the source, so the line just counts.
	std::string from = reviewing() ? std::string(dataset_.dir.empty() ? "Archived" : "Published") : rowsScope == ResultsScope::Saved ? std::to_string(rowsRuns) + (rowsRuns == 1 ? " run" : " runs")
		: rowsScope == ResultsScope::File ? std::string("1 run") : std::string("Measured since Studio opened");
	// Where the rows come from, on the line under the toolbar (the toolbar has no room for it at 1366-1480 px with
	// Open and Archive): this session, all saved runs, or one saved run (the newest dozen, by name and time).  The menu
	// is read when it opens, so a run that just ended is in it.
	std::string scopeText = reviewing() ? std::string(ICON_ARCHIVE "  ") + archive_.name : rowsScope == ResultsScope::Saved ? ICON_CLOCK "  All saved runs"
		: rowsScope == ResultsScope::File ? std::string(ICON_FILE_CSV "  ") + fs::path(rowsFrom).filename().string() : ICON_BOLT "  This session";
	ImGui::SetNextItemWidth(px(240));
	if (ImGui::BeginCombo("##rscope", scopeText.c_str(), ImGuiComboFlags_HeightLarge)) {
		int session = tracker.lastVehicleId() > 0 ? int(tracker.vehiclesSince(0).size()) : 0;
		std::string label = "This session  (" + std::to_string(session) + " vehicles)";
		if (ImGui::Selectable(label.c_str(), rowsScope == ResultsScope::Session)) showSessionResults();
		std::vector<resume::SavedRun> runs = screenshotMode ? std::vector<resume::SavedRun>() : resume::savedRuns(dataDir());
		label = "All saved runs  (" + std::to_string(runs.size()) + ")";
		if (ImGui::Selectable(label.c_str(), rowsScope == ResultsScope::Saved)) showSavedRuns();
		if (!runs.empty()) ImGui::SeparatorText("One run");
		for (size_t i = 0; i < runs.size() && i < 12; i++) {
			ImGui::PushID(int(i));
			if (ImGui::Selectable(resume::runLabel(runs[i].name).c_str(), rowsScope == ResultsScope::File && rowsFrom == runs[i].csv))
				loadResultsCsv(runs[i].csv);
			ImGui::PopID();
		}
		if (runs.size() > 12) ImGui::TextDisabled("Older runs: Open > A stats csv");
		ImGui::EndCombo();
	}
	ImGui::SameLine(0, px(12));
	ImGui::AlignTextToFramePadding();
	dimText("%s  ·  %zu of %zu vehicles%s", from.c_str(), shown.size(), rows.size(),
		!ownDrives ? "" : ownDrives == 1 ? "  ·  1 is your own calibration drive, left out of the totals" : ("  ·  " + std::to_string(ownDrives)
			+ " are your own calibration drives, left out of the totals").c_str());
	// Calibration sets: which runs share a calibration, each set's state, and what can be done with it (Sets.cpp).
	setsBar(avail);
	publishDialog();
	removeDialog();
	recheckDialog();

	if (rows.empty() && reviewing()) {
		ImGui::Dummy(ImVec2(0, px(20)));
		dimText("This archive holds no vehicles: its runs logged none. Its files are still in the folder.");
		return;
	}
	if (rows.empty()) {
		ImGui::Dummy(ImVec2(0, px(40)));
		// This session is empty but earlier runs are saved: say so first, so it never reads as lost data.
		size_t saved = rowsScope == ResultsScope::Saved || screenshotMode ? 0 : resume::savedRuns(dataDir()).size();
		beginCard("noresults", ICON_CHART, saved ? "Nothing measured this session yet" : "Nothing here yet", ImVec2(avail, 0));
		if (saved) {
			dimText("Your earlier results are safe: %zu saved %s in the data folder. Every run is kept from one launch to the next.",
				saved, saved == 1 ? "run" : "runs");
			std::string show = std::string(ICON_CLOCK "  Show all saved runs (") + std::to_string(saved) + ")";
			if (primaryButton(show.c_str())) showSavedRuns();
			ImGui::SameLine();
		}
		else
			dimText(rowsScope == ResultsScope::Saved
				? "No saved runs yet. Run tracking on a video or camera and every measured vehicle lands here, with its speed, direction "
				  "and a snapshot, and stays here the next time you open VST Studio."
				: "Run tracking on a video or camera and every measured vehicle lands here, with its speed, direction and a snapshot. "
				  "Or use Open to look at an archive or the results of an earlier run.");
		if (source.empty() && batch.empty()) { if ((saved ? ghostButton(ICON_VIDEO "  Choose a source") : primaryButton(ICON_VIDEO "  Choose a source"))) page = Page::Source; }
		else if (saved ? ghostButton(ICON_PLAY "  Start tracking") : primaryButton(ICON_PLAY "  Start tracking")) startTracking();
		endCard();
		return;
	}

	// Summary tiles across the top.
	int over = 0, way = 0, wrong = 0, top = 0;
	double sum = 0;
	std::vector<int> speeds;
	int traffic = 0;   // every row but the user's own calibration and check drives
	for (const ResultRow& r : rows) {
		if (r.calKind) continue;
		traffic++;
		over += r.speed > limit;
		way += r.speed >= wayOver;
		wrong += r.wrongWay;
		top = std::max(top, r.speed);
		sum += r.speed;
		speeds.push_back(r.speed);
	}
	std::sort(speeds.begin(), speeds.end());
	int p85 = report::percentile(speeds, 85);   // nearest rank, the same definition as the street report and the phone
	float tw = (avail - px(48)) / 5, th = px(100);
	char sub[64];
	const int perc = std::max(traffic, 1);
	statTile(ICON_CAR, "Vehicles", std::to_string(traffic), reviewing() ? "in this archive" : rowsScope == ResultsScope::Session ? "this session" : rowsScope == ResultsScope::Saved ? "in all saved runs" : "in this file", c::accent, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "%d%% over %d mph", int(over * 100 / perc), limit);
	statTile(ICON_GAUGE, "Speeding", std::to_string(over), sub, c::warn, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "%d mph or more", wayOver);
	statTile(ICON_BOLT, "Way over", std::to_string(way), sub, c::bad, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	const std::string& legal = reviewing() ? archive_.cal.legal : cfg.g.LegalDirection;
	statTile(ICON_WARN, "Wrong way", std::to_string(wrong), legal != "Both" ? "against the one-way" : reviewing() ? "two-way when archived" : "one-way? set it in step 4",
		c::wrong, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "avg %.0f · top %d mph", sum / perc, top);
	statTile(ICON_CHART, "85th percentile", std::to_string(p85) + " mph", sub, c::accent2, ImVec2(tw, th));

	ImGui::Dummy(ImVec2(0, px(2)));
	float detailW = px(340);
	float leftW = avail - detailW - px(20);
	float chartsH = px(210);
	ImGui::BeginGroup();
	std::vector<const ResultRow*> charted;   // the charts are the street's traffic: not the user's own drives
	for (const ResultRow* r : shown)
		if (!r->calKind) charted.push_back(r);
	resultsCharts(charted, leftW);
	ImGui::Dummy(ImVec2(0, px(2)));

	float tableH = ImGui::GetContentRegionAvail().y;
	if (ImGui::BeginTable("rows", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable
		| ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX, ImVec2(leftW, tableH))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, px(46));
		ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, px(110));
		ImGui::TableSetupColumn("Direction", 0, 1.5f);
		// Entry -> exit speed with the trend's icon; sorts by acceleration (fastest speeding up first, unknown last).
		ImGui::TableSetupColumn("In " ICON_ARROW_RIGHT " out", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, px(118));
		ImGui::TableSetupColumn("When", 0, 1.2f);
		ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, px(70));
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, px(80));
		ImGui::TableSetupColumn("Source", 0, 1.4f);
		ImGui::TableHeadersRow();
		// Sorting: by speed (default, fastest first) or by the order they were logged.
		std::vector<int> order(shown.size());
		for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
		if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
			if (ss->SpecsCount > 0) {
				int colIdx = ss->Specs[0].ColumnIndex;
				bool asc = ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
				std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
					const ResultRow *x = shown[a], *y = shown[b];
					if (colIdx == 3) {
						// Unknown trends sort after every known one, whichever way round.
						bool kxn = !x->trend.empty(), kyn = !y->trend.empty();
						if (kxn != kyn) return kxn;
						return asc ? x->accel < y->accel : x->accel > y->accel;
					}
					int kx = colIdx == 1 ? x->speed : colIdx == 5 ? x->frames : colIdx == 6 ? x->area : a;
					int ky = colIdx == 1 ? y->speed : colIdx == 5 ? y->frames : colIdx == 6 ? y->area : b;
					return asc ? kx < ky : kx > ky;
				});
			}
		// Every cell is one frame-height line: the row number centred in it (the selectable spans the row), pills
		// centred on it and plain text aligned to frame padding, so a row reads along one baseline.
		float lineH = ImGui::GetFrameHeight();
		ImGuiListClipper clip;
		clip.Begin(int(order.size()));
		while (clip.Step())
			for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
				const ResultRow& r = *shown[order[k]];
				int idx = int(&r - rows.data());
				ImGui::TableNextRow(0, px(30));
				ImGui::TableNextColumn();
				setStripe(r, px(30));
				ImGui::PushID(idx);
				char num[16];
				snprintf(num, sizeof num, "%d", idx + 1);
				ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, .5f));
				if (ImGui::Selectable(num, selectedRow_ == idx, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, lineH))) {
					selectedRow_ = idx;
					if (r.setId) selectedSet_ = r.setId;   // the set bar follows the picked car
				}
				ImGui::PopStyleVar();
				ImGui::PopID();
				ImGui::TableNextColumn();
				std::string s = std::to_string(r.speed) + " mph";
				ImU32 col = speedColor(r.speed, limit, wayOver);
				linePill(s.c_str(), c::bg0, col, fonts.strong);
				if (r.adjusted) {
					// Re-timed to its set's newer calibration: a small mark, the measured value on hover.
					// Drawn beside the pill, centred on it, without adding a layout item: any item here makes the row taller
					// than its neighbours and breaks the set stripe down the left edge.
					ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();
					ImVec2 sz = ImGui::CalcTextSize(ICON_RULER);
					ImVec2 at(pmax.x + px(4), (pmin.y + pmax.y - sz.y) / 2);
					ImGui::GetWindowDrawList()->AddText(at, c::dim, ICON_RULER);
					if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(at, at + sz))
						ImGui::SetTooltip("Adjusted to your newer calibration: measured %d mph with the one it was recorded with", r.measured.speed);
				}
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, v4(r.l2r ? c::l2r : c::r2l));
				ImGui::Text("%s %s", r.l2r ? "→" : "←", r.compass.c_str());
				ImGui::PopStyleColor();
				if (r.calKind) {
					ImGui::SameLine();
					linePill(r.calKind == 1 ? ICON_RULER " calibration" : ICON_BULLSEYE " check", c::bg0, c::accent2);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Your own %s drive at %g mph: left out of the totals, the charts and the street report",
							r.calKind == 1 ? "calibration" : "check", std::round(r.drivenMph * 10) / 10);
				}
				else if (r.wrongWay) { ImGui::SameLine(); linePill(ICON_WARN " wrong way", c::rgb(0xFFFFFF), c::wrong); }
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				{
					TrendLook tl = trendLook(r.trend);
					ImGui::PushStyleColor(ImGuiCol_Text, v4(tl.color));
					if (r.trend.empty()) ImGui::TextUnformatted(tl.icon);
					else ImGui::Text("%s  %d " ICON_ARROW_RIGHT " %d", tl.icon, r.entryMph, r.exitMph);
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered()) {
						if (r.trend.empty()) ImGui::SetTooltip("Not seen often or cleanly enough to tell whether it was speeding up or slowing down");
						else ImGui::SetTooltip("%s: %d mph at the first speed line, %d at the second (%+.1f mph/s).\nAbout %d mph 20 ft further on.",
							tl.label, r.entryMph, r.exitMph, r.accel, r.mphIn20ft);
					}
				}
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				if (!r.time.empty()) ImGui::TextUnformatted(r.time.size() > 11 ? r.time.substr(11).c_str() : r.time.c_str());
				else ImGui::Text("%d:%02d in video", int(r.frame / r.fps) / 60, int(r.frame / r.fps) % 60);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%d", r.frames);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				// Feet long when the calibration gives the scale; the engine's area units otherwise.
				if (r.lengthFt > 0) ImGui::Text(r.large ? ICON_TRUCK " %.0f ft" : "%.0f ft", r.lengthFt);
				else if (r.large) ImGui::Text(ICON_TRUCK " %d", r.area);
				else ImGui::Text("%d", r.area);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
				ImGui::TextUnformatted(r.source.c_str());
				ImGui::PopStyleColor();
				if (r.flagged) { ImGui::SameLine(); ImGui::TextColored(v4(c::warn), ICON_WARN); }
			}
		ImGui::EndTable();
	}
	ImGui::EndGroup();
	(void)chartsH;

	// Detail of the selected vehicle.
	ImGui::SameLine(0, px(20));
	ImGui::BeginChild("detail", ImVec2(detailW, 0));
	if (selectedRow_ >= 0 && selectedRow_ < int(rows.size())) {
		const ResultRow& r = rows[selectedRow_];
		beginCard("det", nullptr, nullptr, ImVec2(detailW, 0));
		float w = ImGui::GetContentRegionAvail().x;
		Texture* t = r.vehicleId ? snapshotTexture(r.vehicleId) : !r.snapshotPath.empty() ? fileTexture(r.snapshotPath) : nullptr;
		if (t) ImGui::Image((ImTextureID)(intptr_t)t->imguiId(), ImVec2(w, w * t->height() / std::max(1, t->width())));
		else {
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, w * .4f), c::bg3, px(8));
			// Rows read from a csv have no picture; say why, centred in the placeholder.
			const char* why = reviewing() ? "No snapshot was archived\nfor this vehicle" : "Snapshots are kept only for\nvehicles tracked this session";
			ImVec2 ts = ImGui::CalcTextSize(why);
			ImGui::GetWindowDrawList()->AddText(p + (ImVec2(w, w * .4f) - ts) * .5f, c::faint, why);
			ImGui::Dummy(ImVec2(w, w * .4f));
		}
		ImU32 col = r.wrongWay ? c::wrong : speedColor(r.speed, limit, wayOver);
		ImGui::PushFont(fonts.hero);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
		ImGui::Text("%d", r.speed);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::SameLine();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(26));
		dimText("mph");
		if (r.calKind) {
			char what[96];
			snprintf(what, sizeof what, "%s  YOU, %s AT %g MPH", r.calKind == 1 ? ICON_RULER : ICON_BULLSEYE, r.calKind == 1 ? "CALIBRATING" : "CHECKING",
				std::round(r.drivenMph * 10) / 10);
			pill(what, c::bg0, c::accent2, fonts.strong);
		}
		else if (r.wrongWay) pill(ICON_WARN "  WRONG WAY", c::rgb(0xFFFFFF), c::wrong, fonts.strong);
		else if (r.speed >= wayOver) pill(ICON_BOLT "  WAY OVER THE LIMIT", c::bg0, c::bad, fonts.strong);
		else if (r.speed > limit) pill(ICON_GAUGE "  SPEEDING", c::bg0, c::warn, fonts.strong);
		else pill(ICON_CHECK "  WITHIN THE LIMIT", c::bg0, c::ok, fonts.strong);
		ImGui::Dummy(ImVec2(0, px(4)));
		auto kv = [&](const char* k, const std::string& v) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
			ImGui::TextUnformatted(k);
			ImGui::PopStyleColor();
			ImGui::SameLine(px(120));
			ImGui::TextUnformatted(v.c_str());
		};
		kv("Direction", std::string(r.l2r ? "Left to right, " : "Right to left, ") + r.compass);
		{
			// Speed change: the trend in words and colour, entry -> exit, and where it was headed 20 ft on.
			TrendLook tl = trendLook(r.trend);
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
			ImGui::TextUnformatted("Speed change");
			ImGui::PopStyleColor();
			ImGui::SameLine(px(120));
			ImGui::PushStyleColor(ImGuiCol_Text, v4(tl.color));
			ImGui::Text("%s  %s", tl.icon, tl.label);
			ImGui::PopStyleColor();
			if (!r.trend.empty()) {
				char buf[96];
				snprintf(buf, sizeof buf, "%d " ICON_ARROW_RIGHT " %d mph  (%+.1f mph/s)", r.entryMph, r.exitMph, r.accel);
				kv("In, out", buf);
				kv("20 ft on", r.mphIn20ft == 0 ? std::string("stopped, at this rate") : "about " + std::to_string(r.mphIn20ft) + " mph");
			}
		}
		kv("In the zone", std::to_string(r.frames) + " frames (" + std::to_string(r.frames / r.fps).substr(0, 4) + " s)");
		{
			char sz[96];
			if (r.lengthFt > 0) snprintf(sz, sizeof sz, "about %.0f ft long  (%s)", r.lengthFt, r.large ? "truck or bus" : r.lengthFt >= 19 ? "pickup or van" : "car");
			else snprintf(sz, sizeof sz, "%d%s", r.area, r.large ? "  (truck or bus)" : "");
			kv("Size", sz);
		}
		kv("Frame", std::to_string(r.frame));
		if (!r.time.empty()) { std::string t = r.time; if (t.size() > 10 && t[10] == 'T') t[10] = ' '; kv("Passed at", t); }
		kv("Source", r.source);
		if (!reviewing() && r.lengthFt <= 0 && r.area > 0 && pxPerFtAtLane() <= 0) { ImGui::PushTextWrapPos(0); dimText("Calibrate in step 5 and sizes show in feet."); ImGui::PopTextWrapPos(); }
		if (r.calKind) {
			ImGui::PushTextWrapPos(0);
			dimText("Your own %s drive: left out of the totals, the charts and the street report.", r.calKind == 1 ? "calibration" : "check");
			ImGui::PopTextWrapPos();
		}
		if (const calsets::Set* set = setOfRow(r)) {
			// Which calibration this speed is at, in words; the stripe's colour ties it to the set bar above the table.
			ImGui::PushTextWrapPos(0);
			if (!set->known) dimText("From before VST kept each run's calibration, so this speed is shown as it was measured.");
			else if (r.adjusted)
				dimText("Measured %d mph with the calibration it was recorded with; %d mph with %s.", r.measured.speed, r.speed,
					set->locked() ? "the calibration its set is locked to" : "your latest calibration");
			else dimText("Set %d%s.", set->id, set->locked() ? ", locked to the calibration it was measured with" : ", at your latest calibration");
			ImGui::PopTextWrapPos();
		}
		ImGui::Dummy(ImVec2(0, px(4)));
		std::string clip = r.clipPath;
		Vehicle v;
		if (r.vehicleId && tracker.vehicle(r.vehicleId, v)) clip = v.clipPath;
		if (!clip.empty() && ghostButton(ICON_FILM "  Play clip", ImVec2(-FLT_MIN, 0))) openWithSystem(clip);
		if (r.vehicleId && ghostButton(ICON_RULER "  This was me: calibrate from it", ImVec2(-FLT_MIN, 0))) {
			calibPickId_ = r.vehicleId;
			calibMode_ = 0;
			calibChecking_ = false;   // "calibrate from it", not check it
			page = Page::Calibrate;
		}
		endCard();
	}
	else {
		beginCard("det0", ICON_EYE, "Pick a vehicle", ImVec2(detailW, 0));
		dimText("Select a row to see its snapshot and details.");
		endCard();
	}
	ImGui::EndChild();
}

// Histogram of speeds (2 mph bins, coloured by band, limit marked) next to a timeline of every vehicle.
void App::resultsCharts(const std::vector<const ResultRow*>& shown, float width) {
	const int limit = resultsLimit(), wayOver = resultsWayOver();
	float h = px(200);
	float half = (width - px(12)) * .5f;
	auto frameCard = [&](const char* id, const char* icon, const char* title) { beginCard(id, icon, title, ImVec2(half, h)); };

	frameCard("hist", ICON_CHART, "Speeds");
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 sz = ImGui::GetContentRegionAvail();
		sz.y -= px(16);
		int lo = 10, hi = 60;
		for (const ResultRow* r : shown) { lo = std::min(lo, r->speed / 2 * 2); hi = std::max(hi, r->speed / 2 * 2 + 2); }
		int bins = (hi - lo) / 2;
		std::vector<int> counts(bins, 0);
		for (const ResultRow* r : shown) counts[std::clamp((r->speed - lo) / 2, 0, bins - 1)]++;
		int maxC = std::max(1, *std::max_element(counts.begin(), counts.end()));
		float bw = sz.x / bins;
		int hoverBin = -1;
		// Only when nothing covers the card: IsMouseHoveringRect alone also fires under an open dialog.
		const bool canHover = ImGui::IsWindowHovered();
		for (int i = 0; i < bins; i++) {
			int mph = lo + i * 2;
			float bh = (sz.y - px(4)) * counts[i] / maxC;
			ImVec2 a(p.x + i * bw + 1, p.y + sz.y - bh), b(p.x + (i + 1) * bw - 1, p.y + sz.y);
			bool hov = canHover && ImGui::IsMouseHoveringRect(ImVec2(a.x, p.y), b);
			if (hov) hoverBin = i;
			if (counts[i]) dl->AddRectFilled(a, b, alpha(speedColor(mph, limit, wayOver), hov ? 1.f : .8f), px(3), ImDrawFlags_RoundCornersTop);
		}
		float lx = p.x + (limit + 1 - lo) / 2.f * bw;
		for (float y = p.y; y < p.y + sz.y; y += px(8)) dl->AddLine(ImVec2(lx, y), ImVec2(lx, y + px(4)), c::text, 1.5f);
		// The label sits on a chip of the card colour so the bars behind it can't swallow it.
		std::string lim = std::to_string(limit) + " mph limit";
		ImVec2 ls = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, lim.c_str());
		ImVec2 lp(std::min(lx + px(6), p.x + sz.x - ls.x - px(4)), p.y);
		dl->AddRectFilled(lp - ImVec2(px(4), px(1)), lp + ls + ImVec2(px(4), px(1)), alpha(c::bg2, .9f), px(4));
		dl->AddText(fonts.caption, fsz(fonts.caption), lp, c::text, lim.c_str());
		dl->AddLine(ImVec2(p.x, p.y + sz.y), ImVec2(p.x + sz.x, p.y + sz.y), c::border);
		for (int mph = (lo + 9) / 10 * 10; mph <= hi; mph += 10) {
			float x = p.x + (mph - lo) / 2.f * bw;
			std::string t = std::to_string(mph);
			dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(x - px(6), p.y + sz.y + px(2)), c::faint, t.c_str());
		}
		ImGui::Dummy(ImVec2(sz.x, sz.y + px(14)));
		if (hoverBin >= 0) ImGui::SetTooltip("%d-%d mph: %d vehicles", lo + hoverBin * 2, lo + hoverBin * 2 + 1, counts[hoverBin]);
	}
	endCard();
	ImGui::SameLine(0, px(12));
	frameCard("timeline", ICON_CLOCK, "Over time");
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 sz = ImGui::GetContentRegionAvail();
		sz.y -= px(4);
		int top = 50;
		for (const ResultRow* r : shown) top = std::max(top, r->speed + 5);
		float ly = p.y + sz.y - sz.y * limit / top;
		dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + sz.x, ly), alpha(c::bad, .04f));
		for (float x = p.x; x < p.x + sz.x; x += px(8)) dl->AddLine(ImVec2(x, ly), ImVec2(x + px(4), ly), alpha(c::text, .6f), 1.f);
		size_t n = shown.size();
		const ResultRow* hov = nullptr;
		for (size_t i = 0; i < n; i++) {
			const ResultRow* r = shown[i];
			float x = p.x + (n > 1 ? sz.x * i / (n - 1) : sz.x * .5f);
			float y = p.y + sz.y - sz.y * r->speed / top;
			ImU32 col = speedColor(r->speed, limit, wayOver);
			if (r->wrongWay) dl->AddCircle(ImVec2(x, y), px(6), c::wrong, 0, 2.f);
			dl->AddCircleFilled(ImVec2(x, y), px(3.2f), col);
			ImVec2 m = ImGui::GetIO().MousePos;
			if (std::fabs(m.x - x) < px(4) && std::fabs(m.y - y) < px(6)) hov = r;
		}
		ImGui::Dummy(sz);
		if (hov) ImGui::SetTooltip("%d mph %s%s", hov->speed, hov->l2r ? "→" : "←", hov->wrongWay ? "  (wrong way)" : "");
	}
	endCard();
}

// ---- street report ----------------------------------------------------------------------------------------

report::Input App::reportInput(bool snapshots, const std::string& from, const std::string& to) {
	const Globals& g = cfg.g;
	report::Input in;
	in.dataDir = dataDir();
	in.configPath = cfg.path();
	// calibration-history.csv lives in the config folder: next to this VST.cfg, or the per-user config folder.
	if (!cfg.path().empty()) in.historyFiles.push_back((fs::path(cfg.path()).parent_path() / "calibration-history.csv").string());
	in.historyFiles.push_back((userpaths::configDir() / "calibration-history.csv").string());
	// Runs recorded before manifests existed are read with the current settings.
	in.current.l2rCompass = g.L2RDirection;
	in.current.r2lCompass = g.R2LDirection;
	in.current.legal = g.LegalDirection;
	in.current.framesL2R = g.CalibrationFramesL2R;
	in.current.framesR2L = g.CalibrationFramesR2L;
	in.current.calSpeed = g.CalibrationSpeed;
	in.current.calFps = g.CalibrationFPS;
	in.current.minLoggedSpeed = g.minLoggedSpeed;
	calibrationPass("", 0);   // load calibration-passes.csv
	for (const auto& kv : calPasses_) in.calibrationPasses.insert(kv.first);
	// Each run at its calibration set's zone feet, the same speeds Results shows (CalSets.h).
	syncSets(true);
	for (const calsets::Set& s : sets_.sets) {
		calsets::Scale ft = calsets::shownScale(s, currentView(), currentScale());
		if (s.known && ft.valid()) for (const std::string& r : s.runs) in.zoneFt[r] = { ft.l2r, ft.r2l };
	}
	if (!snapshots) return in;
	// Example frames exist only for this session's vehicles (kept in memory).  Take the six fastest in the date range,
	// each made safe by blurForReport before it ever reaches the report.
	std::vector<Vehicle> vs = tracker.vehiclesSince(0);
	std::sort(vs.begin(), vs.end(), [](const Vehicle& a, const Vehicle& b) { return a.ev.speed > b.ev.speed; });
	for (const Vehicle& v : vs) {
		if (in.snapshots.size() >= 6) break;
		std::string when = v.passed();
		std::string day = when.substr(0, 10);
		if (calibrationPass(v.statsPath, v.ev.frame)) continue;   // the user's own drive isn't an example of the street's traffic
		if (v.snapshot.empty() || (!from.empty() && (day.empty() || day < from)) || (!to.empty() && (day.empty() || day > to))) continue;
		std::string jpeg = report::blurForReport(v.snapshot);
		if (!jpeg.empty()) in.snapshots.push_back({ jpeg, v.ev.speed, v.ev.leftToRight, when });
	}
	return in;
}

// The same for an open archive: its folder is laid out like the data folder, so the report reads its Stats/ as is, with
// the history and config it was archived with and its own (unblurred until blurForReport) snapshots.
report::Input App::archiveReportInput(bool snapshots, const std::string& from, const std::string& to) {
	const archive::Info& a = archive_;
	report::Input in;
	in.dataDir = a.dir;
	in.configPath = a.configPath;   // history rows are keyed on the original path
	in.historyFiles.push_back((fs::path(a.dir) / "calibration-history.csv").string());
	// The user's own drives are left out as they were in the data folder: from the archive's own record, or, for an
	// archive made before it kept one, from this computer's (runs are matched by csv name, which archiving keeps).
	if (a.hasPasses) for (const CalPass& p : a.passes) in.calibrationPasses.insert(report::passKey(p.stats, p.frame));
	else {
		calibrationPass("", 0);
		for (const auto& kv : calPasses_) in.calibrationPasses.insert(kv.first);
	}
	// The calibration sets it was archived with (Stats/calibration-sets.json), so the report re-times its runs the way
	// Results showed them when they were archived.
	for (const calsets::Set& s : calsets::load((fs::path(a.dir) / "Stats").string()).sets)
		if (s.known && s.scale.valid()) for (const std::string& r : s.runs) in.zoneFt[r] = { s.scale.l2r, s.scale.r2l };
	in.current.l2rCompass = a.cal.l2rCompass;
	in.current.r2lCompass = a.cal.r2lCompass;
	in.current.legal = a.cal.legal;
	in.current.framesL2R = a.cal.framesL2R;
	in.current.framesR2L = a.cal.framesR2L;
	in.current.calSpeed = a.cal.calSpeed;
	in.current.calFps = a.cal.calFps;
	in.current.minLoggedSpeed = a.cal.minLoggedSpeed;
	if (!snapshots) return in;
	std::vector<const archive::Vehicle*> vs;
	for (const archive::Vehicle& v : a.vehicles) if (!v.snapshot.empty()) vs.push_back(&v);
	std::sort(vs.begin(), vs.end(), [](const archive::Vehicle* x, const archive::Vehicle* y) { return x->speed > y->speed; });
	for (const archive::Vehicle* v : vs) {
		if (in.snapshots.size() >= 6) break;
		std::string day = v->passed.substr(0, 10);
		if ((!from.empty() && (day.empty() || day < from)) || (!to.empty() && (day.empty() || day > to))) continue;
		std::ifstream f(fs::path(a.dir) / fs::path(v->snapshot), std::ios::binary);
		std::vector<uchar> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		cv::Mat img = bytes.empty() ? cv::Mat() : cv::imdecode(bytes, cv::IMREAD_COLOR);
		std::string jpeg = img.empty() ? std::string() : report::blurForReport(img);
		if (!jpeg.empty()) in.snapshots.push_back({ jpeg, v->speed, v->l2r, v->passed });
	}
	return in;
}

report::Settings App::reportSettings() {
	report::Settings s;
	s.siteName = prefs.siteName;
	s.speedLimit = prefs.speedLimit;
	s.wayOver = prefs.egregious;
	s.version = VST_VERSION;
	return s;
}

// The "Make report" dialog: a few choices, then one HTML file in <data>/Reports that prints cleanly to PDF.
void App::reportDialog() {
	ImGui::SetNextWindowSize(ImVec2(px(560), 0), ImGuiCond_Always);   // 0 = fit the height every frame, as options open and close
	if (!ImGui::BeginPopupModal("Street report", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
	ReportForm& f = reportForm_;
	float col = px(150);
	dimText(reviewing() ? "A printable study of the speeds in this archive, for a city council or a traffic engineer. "
		"It lists coverage, speeds per direction, the calibration record and the error band."
		: "A printable study of the speeds in your data folder, for a city council or a traffic engineer. "
		"It lists coverage, speeds per direction, the calibration record and the error band.");
	ImGui::Dummy(ImVec2(0, px(6)));
	formLabel("Site name", col);
	char site[96];
	snprintf(site, sizeof site, "%s", prefs.siteName.c_str());
	if (ImGui::InputTextWithHint("##rsite", "Street and block, e.g. Locust Ave, 600 block", site, sizeof site)) prefs.siteName = site;
	ImGui::SetCursorPosX(col);
	dimText("Street and block only, never a house number.");
	float half = (ImGui::GetContentRegionAvail().x - col - px(8)) / 2;
	formLabel("Dates", col);
	ImGui::SetNextItemWidth(half);
	ImGui::InputTextWithHint("##rfrom", "from YYYY-MM-DD", f.from, sizeof f.from);
	ImGui::SameLine(0, px(8));
	ImGui::SetNextItemWidth(half);
	ImGui::InputTextWithHint("##rto", "to YYYY-MM-DD", f.to, sizeof f.to);
	ImGui::SetCursorPosX(col);
	dimText("Leave blank for all the data.");
	ImGui::SetCursorPosX(col);
	ImGui::Checkbox("Compare with another period (before/after)", &f.compare);
	if (f.compare) {
		ImGui::SetCursorPosX(col);
		ImGui::SetNextItemWidth(half);
		ImGui::InputTextWithHint("##rcf", "from YYYY-MM-DD", f.cmpFrom, sizeof f.cmpFrom);
		ImGui::SameLine(0, px(8));
		ImGui::SetNextItemWidth(half);
		ImGui::InputTextWithHint("##rct", "to YYYY-MM-DD", f.cmpTo, sizeof f.cmpTo);
	}
	ImGui::SetCursorPosX(col);
	ImGui::Checkbox("Include example frames (blurred)", &f.snapshots);
	if (f.snapshots) {
		ImGui::SetCursorPosX(col);
		dimText(reviewing() ? "From this archive's snapshots. Plates are blurred and nothing identifying is added."
			: "Only this session's vehicles have snapshots. Plates are blurred and nothing identifying is added.");
	}
	formLabel("Note", col);
	ImGui::InputTextMultiline("##rnote", f.note, sizeof f.note, ImVec2(-FLT_MIN, px(70)));
	ImGui::SetCursorPosX(col);
	if (reviewing()) dimText("Limit %d mph, way over %d mph, as when it was archived.", resultsLimit(), resultsWayOver());
	else dimText("Limit %d mph, way over %d mph (set on the Live page).", prefs.speedLimit, prefs.egregious);

	if (!f.error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad)); ImGui::TextWrapped("%s", f.error.c_str()); ImGui::PopStyleColor(); }
	if (!f.saved.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok)); ImGui::TextWrapped(ICON_CHECK_CIRCLE "  Saved %s", f.saved.c_str()); ImGui::PopStyleColor(); }
	ImGui::Dummy(ImVec2(0, px(6)));
	if (primaryButton(ICON_REPORT "  Make report")) {
		f.error.clear();
		f.saved.clear();
		// Dates must be blank or YYYY-MM-DD; the report reads anything else as "all data", so say so here instead.
		int64_t d;
		for (const char* v : { f.from, f.to, f.cmpFrom, f.cmpTo })
			if (*v && !report::parseDay(v, d)) f.error = std::string("\"") + v + "\" isn't a date like 2026-11-02.";
		if (f.compare && (!*f.cmpFrom || !*f.cmpTo)) f.error = "Give both dates of the period to compare with.";
		if (f.error.empty()) {
			report::Settings s = reportSettings();
			s.speedLimit = resultsLimit();   // an archive is reported at the limits it was archived with
			s.wayOver = resultsWayOver();
			s.from = f.from;
			s.to = f.to;
			s.note = f.note;
			s.includeSnapshots = f.snapshots;
			if (f.compare) { s.compareFrom = f.cmpFrom; s.compareTo = f.cmpTo; }
			std::string path, html;
			report::Input in = reviewing() ? archiveReportInput(f.snapshots, s.from, s.to) : reportInput(f.snapshots, s.from, s.to);
			if (report::writeReport(in, s, path, html, f.error)) {
				f.saved = path;
				toast("Report saved: " + fs::path(path).filename().string(), c::ok, ICON_REPORT);
			}
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(f.saved.empty());
	if (ghostButton(ICON_ARROW_UP_RIGHT "  Open")) openWithSystem(f.saved);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ghostButton("Close")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// ======================================================================================================
// Companion
// ======================================================================================================

void App::pageCompanion() {
	float avail = ImGui::GetContentRegionAvail().x;
	float leftW = std::min(px(520), avail * .45f);
	bool on = remoteOn();

	beginCard("remote", ICON_BROADCAST, "Remote companion", ImVec2(leftW, 0));
	dimText("Open VST on your phone to check the camera's aim, help with calibration from the street, and see recent speeders, "
		"while this computer keeps tracking. Your phone needs to be on the same Wi-Fi network. Is your phone the camera? Pair a second "
		"phone or a tablet, or skip this: every step works from this computer, and Calibrate can take timed pictures while you're outside.");
	ImGui::Dummy(ImVec2(0, px(4)));
	bool want = on;
	// setRemote() starts or destroys the server right here, so read it again: drawing the pairing card below from the
	// value before the click dereferenced a destroyed server, and turning the companion off crashed the app.
	if (ImGui::Checkbox("Allow the companion app to connect", &want)) {
		setRemote(want);
		on = remoteOn();
	}
	ImGui::BeginDisabled(on);
	formLabel("Port", px(110));
	ImGui::InputInt("##port", &prefs.remotePort, 0);
	prefs.remotePort = std::clamp(prefs.remotePort, 1024, 65535);
	ImGui::EndDisabled();
	if (on && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Turn the companion off to change the port.");
	formLabel("Site name", px(110));
	char site[96];
	snprintf(site, sizeof site, "%s", prefs.siteName.c_str());
	if (ImGui::InputText("##site", site, sizeof site)) prefs.siteName = site;
	ImGui::Dummy(ImVec2(0, px(6)));
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextUnformatted(ICON_INFO);
	ImGui::SameLine();   // the paragraph wraps under itself, not under the icon
	ImGui::TextWrapped("Pairing uses a secret in the QR code. Anyone with it can see the camera and change settings, so only "
		"share it with people you trust. Making a new code signs out every paired phone.");
	ImGui::PopStyleColor();
	if (ghostButton(ICON_ROTATE "  New pairing code")) {
		prefs.remoteToken = AppPrefs::newToken();
		prefs.save();
		if (server) server->setToken(prefs.remoteToken);   // also ends the live views opened with the old code
		toast("New pairing code. Paired phones need to scan it again.", c::accent2, ICON_ROTATE);
	}
	endCard();

	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	float rw = avail - leftW - px(20);
	beginCard("pair", ICON_LINK, "Pair your phone", ImVec2(rw, 0));
	if (!on) dimText("Tick \"Allow the companion app to connect\" to get a code to scan with your phone.");
	else {
		std::string url = server->pairingUrl();
		float qs = std::min(px(260), rw - px(40));
		server->drawQr(url, qs);
		ImGui::SameLine(0, px(24));
		ImGui::BeginGroup();
		ImGui::PushFont(fonts.strong);
		ImGui::TextUnformatted("Scan with your phone's camera");
		ImGui::PopFont();
		dimText("Or type this address into the phone's browser:");
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::accent2));
		ImGui::TextWrapped("%s", server->baseUrl().c_str());
		ImGui::PopStyleColor();
		// Typing the address opens the phone's "Enter it by hand" form, which asks for the pairing code too, so show it
		// in groups of four (the phone ignores the spaces).
		std::string grouped;
		for (size_t i = 0; i < prefs.remoteToken.size(); i++) grouped += (i && i % 4 == 0 ? " " : "") + std::string(1, prefs.remoteToken[i]);
		dimText("with the pairing code:");
		ImGui::PushFont(fonts.strong);
		ImGui::TextUnformatted(grouped.c_str());
		ImGui::PopFont();
		if (ghostButton(ICON_LINK "  Copy pairing link")) { ImGui::SetClipboardText(url.c_str()); toast("Pairing link copied", c::ok, ICON_CHECK); }
		// Only shown when there's a choice: a computer on Wi-Fi and Ethernet, or with a VPN or virtual adapters, has
		// several addresses and the automatic pick can be one the phone can't reach.  The interfaces are re-read
		// every 5 s rather than every frame (each read opens a socket), so a cable plugged in shows up on its own.
		if (ImGui::GetTime() - lanReadAt_ > 5) {
			lanChoices_ = lanAddresses();
			lanGuess_ = lanAddress();
			lanReadAt_ = ImGui::GetTime();
		}
		if (lanChoices_.size() > 1) {
			ImGui::Dummy(ImVec2(0, px(4)));
			dimText("Phone can't connect? Try the address of the network the phone is on:");
			std::string automatic = "Automatic (" + lanGuess_ + ")";
			ImGui::SetNextItemWidth(px(240));
			if (ImGui::BeginCombo("##lanaddr", prefs.remoteAddress.empty() ? automatic.c_str() : prefs.remoteAddress.c_str())) {
				auto pick = [&](const std::string& label, const std::string& value) {
					if (ImGui::Selectable(label.c_str(), prefs.remoteAddress == value)) {
						prefs.remoteAddress = value;
						prefs.save();
						server->setAddress(value);
					}
				};
				pick(automatic, "");
				for (const std::string& ip : lanChoices_) pick(ip, ip);
				ImGui::EndCombo();
			}
		}
		if (int n = server->openStreams()) {
			ImGui::Dummy(ImVec2(0, px(4)));
			dimText("%d live view%s open (up to %d at once)", n, n == 1 ? "" : "s", Server::kMaxStreams);
		}
		ImGui::Dummy(ImVec2(0, px(8)));
		dimText("The phone can:");
		for (const char* t : { ICON_EYE "  see the camera live, with the zones drawn on", ICON_RULER "  mark calibration passes from the curb",
			ICON_GAUGE "  start, stop and follow tracking", ICON_CAR "  browse recent speeders with snapshots and clips",
			ICON_EXPORT "  share a vehicle as a picture or clip" })
			ImGui::TextUnformatted(t);
		ImGui::EndGroup();
	}
	endCard();
	ImGui::EndGroup();
}
