// portable-file-dialogs pulls in the Windows headers; they must be parsed before Globals.h's
// "using namespace std", or rpcndr.h's byte collides with std::byte.
#include <portable-file-dialogs.h>
#include "App.h"
#include "Canvas.h"
#include "Icons.h"
#include "Server.h"
#include "Theme.h"
#include "UserPaths.h"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>

namespace fs = std::filesystem;
using namespace theme;

// Font Awesome "file-lines" for the street report, kept here rather than in Icons.h while only the report uses it.
#define ICON_REPORT "\xef\x85\x9c"
#ifndef VST_VERSION
#define VST_VERSION "dev"
#endif

static ImTextureID tid(Texture& t) { return (ImTextureID)(intptr_t)t.imguiId(); }

static const char* pageTitle(Page p) {
	switch (p) {
	case Page::Guide: return "Get started";
	case Page::Source: return "Video source";
	case Page::Zones: return "Zones";
	case Page::Calibrate: return "Calibrate";
	case Page::Track: return "Live tracking";
	case Page::Results: return "Results";
	case Page::Companion: return "Companion app";
	default: return "";
	}
}

static const char* pageIcon(Page p) {
	switch (p) {
	case Page::Guide: return ICON_FLAG;
	case Page::Source: return ICON_VIDEO;
	case Page::Zones: return ICON_VECTOR;
	case Page::Calibrate: return ICON_RULER;
	case Page::Track: return ICON_GAUGE;
	case Page::Results: return ICON_CHART;
	case Page::Companion: return ICON_BROADCAST;
	default: return "";
	}
}

static const char* pageSubtitle(Page p) {
	switch (p) {
	case Page::Guide: return "Everything you need to measure traffic on your street, one step at a time";
	case Page::Source: return "Recorded video, a USB camera or phone, or a network camera";
	case Page::Zones: return "Drag the lines on the picture; the tracker only looks inside the box";
	case Page::Calibrate: return "Teach VST how long a car at a known speed takes to cross the zone";
	case Page::Track: return "Watch vehicles get measured as they pass";
	case Page::Results: return "Speeders and wrong-way drivers, with snapshots and statistics";
	case Page::Companion: return "Check on the camera and see recent speeders from your phone";
	default: return "";
	}
}

static double now() { return glfwGetTime(); }

// ======================================================================================================
// Lifecycle
// ======================================================================================================

App::App() = default;
App::~App() { shutdown(); }

void App::init(bool fresh) {
	// fresh: default preferences and a fixed pairing code, for reproducible documentation screenshots.
	if (fresh) prefs.remoteToken = "demo-pairing-code";
	else prefs.load();
	std::string cfgPath = prefs.configPath;
	if (cfgPath.empty() || !fs::exists(cfgPath)) cfgPath = userpaths::resolveConfig("VST.cfg");
	if (!cfgPath.empty() && !loadConfig(cfgPath)) toast(cfg.error, c::bad, ICON_WARN);
	page = prefs.guideDone ? Page::Source : Page::Guide;
	if (!prefs.lastSource.empty() && classifySource(prefs.lastSource) != SourceInfo::Camera
		&& (classifySource(prefs.lastSource) == SourceInfo::Stream || fs::exists(prefs.lastSource)))
		openSource(prefs.lastSource);
	if (prefs.remoteEnabled) setRemote(true);
}

void App::shutdown() {
	tracker.stop();
	if (persist) flushHealthDay();   // today so far; a restart later today adds a second row for the date
	server.reset();
	if (persist) prefs.save();
	persist = false;   // once is enough; the destructor calls this again
}

bool App::wantsContinuousFrames() {
	return tracker.running() || player.playing || (player.isOpen() && player.info().kind != SourceInfo::File && player.info().kind != SourceInfo::Image)
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
	return true;
}

bool App::saveConfig() {
	auto problems = cfg.problems();
	if (!problems.empty()) { toast("Fix the highlighted settings first: " + problems[0], c::bad, ICON_WARN); return false; }
	if (!cfg.save()) { toast(cfg.error, c::bad, ICON_WARN); return false; }
	toast("Saved " + fs::path(cfg.path()).filename().string(), c::ok, ICON_CHECK_CIRCLE);
	return true;
}

bool App::zonesDone() {
	return !source.empty() && prefs.zonesFor == source && cfg.problems().empty();
}

bool App::calibrationDone() {
	// The bundled sample's VST.cfg comes calibrated for that video, so the sample never nags.
	if (sampleCache_.empty()) sampleCache_ = findSample();
	if (!source.empty() && source == sampleCache_) return true;
	return !cfg.path().empty() && prefs.calibratedFor == cfg.path() && cfg.problems().empty();
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

bool App::openSource(const std::string& path) {
	if (tracker.running()) { toast("Stop tracking before changing the source", c::warn, ICON_WARN); return false; }
	if (!player.open(path)) { toast(player.error, c::bad, ICON_WARN); return false; }
	source = path;
	prefs.lastSource = path;
	prefs.addRecent(path);
	return true;
}

void App::addToBatch(const std::vector<std::string>& paths) {
	for (const std::string& p : paths)
		if (std::find(batch.begin(), batch.end(), p) == batch.end()) batch.push_back(p);
	if (source.empty() && !batch.empty()) openSource(batch.front());
}

void App::startTracking() {
	if (tracker.running()) return;
	std::vector<std::string> inputs = batch.empty() ? std::vector<std::string>{ source } : batch;
	if (inputs.empty() || inputs[0].empty()) { toast("Choose a video or camera first", c::warn, ICON_VIDEO); page = Page::Source; return; }
	if (classifySource(inputs[0]) == SourceInfo::Image) { toast("A photo is fine for planning zones, but tracking needs video", c::warn, ICON_IMAGE); return; }
	auto problems = cfg.problems();
	if (!problems.empty()) { toast("Fix the zone settings first: " + problems[0], c::bad, ICON_WARN); page = Page::Zones; return; }
	// Run with the settings on screen (saved or not) via a scratch copy, so experiments don't touch VST.cfg.
	std::string runCfg = cfg.dirty() ? cfg.writeRunCopy(userpaths::configDir().string()) : cfg.path();
	if (runCfg.empty()) { toast("Can't write the run settings to " + userpaths::configDir().string(), c::bad, ICON_WARN); return; }
	player.close();   // a camera can only be opened by one reader at a time
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
	tracker.start(jobs, o);
	rowsFrom.clear();
	page = Page::Track;
	wasRunning_ = true;
}

void App::stopTracking() {
	tracker.stop();
}

void App::applyCalibration(bool l2r, double mph, int frames, double fps) {
	Globals& g = cfg.g;
	if (mph <= 0 || frames <= 0) return;
	if (fps <= 0) fps = g.CalibrationFPS;
	// speed = CalibrationSpeed × CalibrationFrames × (fps / CalibrationFPS) / frames, so a pass at `mph` taking
	// `frames` at `fps` corresponds to CalibrationFrames = frames × mph / CalibrationSpeed × CalibrationFPS / fps.
	int calFrames = std::max(1, int(std::lround(calib::impliedFrames(mph, frames, fps, g.CalibrationSpeed, g.CalibrationFPS))));
	(l2r ? g.CalibrationFramesL2R : g.CalibrationFramesR2L) = calFrames;
	prefs.calibratedFor = cfg.path();   // autosave writes it to VST.cfg on the next frame
	// The record of it: which pass, at what speed, and the value it set.  A new apply also makes any earlier check
	// of this direction moot, so the page's last check result goes too.
	CalEvent e;
	e.l2r = l2r;
	e.drivenMph = mph;
	e.frames = frames;
	e.fps = fps;
	e.calFrames = calFrames;
	recordCalibration(e);
	if (lastCheck_.l2r == l2r) lastCheck_ = CalCheck();
	toast(std::string("Calibrated ") + (l2r ? "left → right" : "right → left") + ": " + std::to_string(calFrames) + " frames at "
		+ std::to_string(int(g.CalibrationSpeed)) + " mph", c::ok, ICON_CHECK_CIRCLE);
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
	Globals& g = cfg.g;
	if (!check.valid) return;
	// Each pass implies a CalibrationFrames value.  The first pass's is the one the check was made against; the
	// check pass's comes from its own frames and speed, the same way an apply works it out.  The new calibration
	// is their mean, rounded, so neither pass's speedometer reading or lane position wins outright.
	double second = calib::impliedFrames(check.drivenMph, check.frames, check.fps, g.CalibrationSpeed, g.CalibrationFPS);
	int calFrames = std::max(1, int(std::lround((check.calFrames + second) / 2)));
	(check.l2r ? g.CalibrationFramesL2R : g.CalibrationFramesR2L) = calFrames;
	prefs.calibratedFor = cfg.path();
	// Recorded as an apply of the check pass (its speed, frames and fps) with the averaged value in force.  That
	// starts a fresh calibration, so the direction reads "not checked" until a third pass checks the average.
	CalEvent e;
	e.l2r = check.l2r;
	e.drivenMph = check.drivenMph;
	e.frames = check.frames;
	e.fps = check.fps;
	e.calFrames = calFrames;
	recordCalibration(e);
	lastCheck_ = CalCheck();
	toast(std::string("Averaged ") + (check.l2r ? "left → right" : "right → left") + ": " + std::to_string(calFrames) + " frames at "
		+ std::to_string(int(g.CalibrationSpeed)) + " mph. Check it once more.", c::ok, ICON_CHECK_CIRCLE);
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
	return calib::state(calibrationHistory(), l2r, l2r ? cfg.g.CalibrationFramesL2R : cfg.g.CalibrationFramesR2L, calibrationDone());
}

void App::toast(const std::string& text, unsigned color, const char* icon) {
	toasts_.push_back({ text, color, icon, now() + 4.5 });
	if (toasts_.size() > 3) toasts_.pop_front();
}

void App::setRemote(bool on) {
	prefs.remoteEnabled = on;
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

void App::loadResultsCsv(const std::string& path) {
	std::vector<StatsRow> stats;
	std::string err;
	int skipped = 0;
	if (!readStatsCsv(path, stats, err, &skipped)) { toast(err, c::bad, ICON_WARN); return; }
	rows.clear();
	const Globals& g = cfg.g;
	for (const StatsRow& s : stats) {
		ResultRow r;
		r.source = fs::path(path).filename().string();
		r.frame = s.frame;
		r.compass = s.compass;
		// The csv names directions by compass label; map them back with the current config's labels.
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
		rows.push_back(r);
	}
	rowsFrom = path;
	selectedRow_ = -1;
	if (skipped) toast(std::to_string(skipped) + " rows in that file weren't in VST's format and were skipped", c::warn, ICON_WARN);
}

void App::showSessionResults() {
	rows.clear();
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
		r.time = v.live ? v.time : v.recorded;   // empty for recordings without a start time: shown as time into the video
		rows.push_back(r);
	}
	rowsFrom.clear();
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

// Pulls new frames and vehicles from the tracker each GUI frame: updates textures, raises toasts for the
// vehicles worth calling out, and refreshes the session results when a run ends.
void App::pumpTracker() {
	if (tracker.latest(live_, prefs.showMotion)) {
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
	for (const Vehicle& v : tracker.vehiclesSince(seenVehicleId_)) {
		seenVehicleId_ = v.id;
		if (v.ev.wrongWay) toast("Wrong-way driver, " + std::to_string(v.ev.speed) + " mph", c::wrong, ICON_WARN);
		else if (v.egregious) toast(std::to_string(v.ev.speed) + " mph " + (v.ev.leftToRight ? "→" : "←") + "  well over the limit", c::bad, ICON_BOLT);
		if (rowsFrom.empty()) showSessionResults();
	}
	if (wasRunning_ && !tracker.running()) {
		wasRunning_ = false;
		auto jobs = tracker.jobs();
		int failed = int(std::count_if(jobs.begin(), jobs.end(), [](const Job& j) { return j.state == Job::Failed; }));
		if (failed) toast(std::to_string(failed) + " video(s) failed: " + jobs.back().message, c::bad, ICON_WARN);
		else toast("Tracking finished: " + std::to_string(tracker.counts().vehicles) + " vehicles this session", c::ok, ICON_FLAG);
		showSessionResults();
		if (!source.empty() && classifySource(source) == SourceInfo::File) player.open(source);
	}
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
	pumpTracker();
	healthTick();
	if (!tracker.running()) player.tick(now());
	if (page == Page::Zones && !source.empty() && cfg.problems().empty()) prefs.zonesFor = source;
	autosave();

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
	ImGui::BeginChild("page", ImVec2(w - railW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, page == Page::Guide || page == Page::Source || page == Page::Companion ? 0 : ImGuiWindowFlags_NoScrollbar);
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
	int step = 0;
	auto item = [&](Page p, bool numbered, bool done) {
		bool sel = page == p;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float iw = ImGui::GetContentRegionAvail().x, ih = px(40);
		ImGui::PushID(int(p));
		if (ImGui::InvisibleButton("nav", ImVec2(iw, ih))) page = p;
		bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		if (sel) {
			dl->AddRectFilled(pos, pos + ImVec2(iw, ih), alpha(c::accent, .16f), px(8));
			dl->AddRectFilled(pos + ImVec2(0, px(9)), pos + ImVec2(px(3), ih - px(9)), c::accent, px(2));
		}
		else if (hov) dl->AddRectFilled(pos, pos + ImVec2(iw, ih), alpha(c::text, .05f), px(8));
		ImFont* f = sel ? fonts.strong : fonts.body;
		ImU32 col = sel ? c::text : (hov ? c::text : c::dim);
		dl->AddText(f, fsz(f), pos + ImVec2(px(14), (ih - fsz(f)) * .5f), sel ? c::accent : col, pageIcon(p));
		dl->AddText(f, fsz(f), pos + ImVec2(px(44), (ih - fsz(f)) * .5f), col, pageTitle(p));
		if (numbered) {
			step++;
			ImVec2 cc = pos + ImVec2(iw - px(18), ih * .5f);
			if (done) {
				dl->AddCircleFilled(cc, px(9), alpha(c::ok, .2f));
				ImFont* sf = fonts.caption;
				ImVec2 ts = sf->CalcTextSizeA(fsz(sf), FLT_MAX, 0, ICON_CHECK);
				dl->AddText(sf, fsz(sf), cc - ts * .5f, c::ok, ICON_CHECK);
			}
			else {
				dl->AddCircle(cc, px(9), c::faint, 0, 1.5f);
				std::string n = std::to_string(step);
				ImFont* sf = fonts.caption;
				ImVec2 ts = sf->CalcTextSizeA(fsz(sf), FLT_MAX, 0, n.c_str());
				dl->AddText(sf, fsz(sf), cc - ts * .5f, c::faint, n.c_str());
			}
		}
	};
	group("SET UP");
	item(Page::Guide, false, false);
	item(Page::Source, true, !source.empty());
	item(Page::Zones, true, zonesDone());
	item(Page::Calibrate, true, calibrationDone());
	group("RUN");
	item(Page::Track, false, false);
	item(Page::Results, false, false);
	group("SHARE");
	item(Page::Companion, false, false);

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
	std::string l2 = std::string(ICON_BROADCAST "  companion ") + (server && server->running() ? "on" : "off");
	dl->AddText(fonts.caption, fsz(fonts.caption), p + ImVec2(px(12), px(60)), server && server->running() ? c::accent2 : c::faint, l2.c_str());
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
	ImGui::TextUnformatted(pageTitle(page));
	ImGui::PopFont();
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextUnformatted(pageSubtitle(page));
	ImGui::PopStyleColor();
	ImGui::EndGroup();

	// Right-aligned controls, laid out from the right edge.
	bool run = tracker.running();
	// Until setup can produce a measurement, the main button leads to the next setup step instead of a run that
	// would only fail.  A missing calibration doesn't block (results are still useful relative to each other);
	// it shows as an amber chip that opens the Calibrate page.
	Page nextStep = Page::Count;
	const char* runLabel = run ? ICON_STOP "  Stop" : ICON_PLAY "  Start tracking";
	if (!run && source.empty() && batch.empty()) { nextStep = Page::Source; runLabel = "Next: choose a video  " ICON_ARROW_RIGHT; }
	else if (!run && !cfg.problems().empty()) { nextStep = Page::Zones; runLabel = "Next: fix the zones  " ICON_ARROW_RIGHT; }
	bool calNag = !run && nextStep == Page::Count && !calibrationDone();
	const char* calText = ICON_GAUGE "  Not calibrated";
	ImVec2 calSize = calNag ? pillSize(calText, fonts.body) : ImVec2(0, 0);
	float bw = ImGui::CalcTextSize(runLabel).x + px(30);
	float pauseW = run ? ImGui::GetFrameHeight() + px(10) : 0;
	std::string chip = source.empty() ? std::string("No source") : displayName(source);
	if (batch.size() > 1) chip = std::to_string(batch.size()) + " videos queued";
	const char* chipIcon = source.empty() ? ICON_VIDEO : classifySource(source) == SourceInfo::Camera ? ICON_CAMERA
		: classifySource(source) == SourceInfo::Stream ? ICON_BROADCAST : ICON_FILE_VIDEO;
	std::string chipText = std::string(chipIcon) + "  " + chip;
	ImVec2 cs = pillSize(chipText.c_str(), fonts.body);
	float right = w - px(24);
	float y = (hh - px(40)) * .5f;
	ImGui::SetCursorPos(ImVec2(right - bw, y));
	if (primaryButton(runLabel, ImVec2(bw, px(40)), run ? c::bad : c::accent)) {
		if (run) stopTracking();
		else if (nextStep != Page::Count) page = nextStep;
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
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speeds use the calibration in %s, which wasn't made for this camera.\nClick to calibrate with one drive past.",
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
		std::string text = std::string(it->icon ? it->icon : ICON_INFO) + "   " + it->text;
		ImFont* f = fonts.strong;
		ImVec2 ts = f->CalcTextSizeA(fsz(f), px(520), px(520), text.c_str());
		ImVec2 size = ts + ImVec2(px(36), px(24));
		float fade = float(std::min(1.0, (it->until - t) / .4));
		ImVec2 p(w - size.x - px(24), y);
		dl->AddRectFilled(p + ImVec2(0, px(4)), p + size + ImVec2(0, px(4)), IM_COL32(0, 0, 0, int(90 * fade)), px(10));
		dl->AddRectFilled(p, p + size, alpha(c::bg3, .98f * fade), px(10));
		dl->AddRectFilled(p, p + ImVec2(px(4), size.y), alpha(it->color, fade), px(10), ImDrawFlags_RoundCornersLeft);
		dl->AddText(f, fsz(f), p + ImVec2(px(20), px(12)), alpha(c::text, fade), text.c_str(), nullptr, px(520));
		y += size.y + px(10);
	}
}

// ======================================================================================================
// Source
// ======================================================================================================

void App::pageSource() {
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
		sourceCard("srccam", ICON_CAMERA, c::ok, "USB camera or phone", "A webcam, or a phone in USB webcam mode. Tracks live, in real time.", [&] {
			ImGui::SetNextItemWidth(px(110));
			char camLabel[48];
			snprintf(camLabel, sizeof camLabel, ICON_CAMERA "  %d", cameraIndex_);
			if (ImGui::BeginCombo("##cam", camLabel)) {
				for (int i = 0; i < 6; i++) {
					snprintf(camLabel, sizeof camLabel, "Camera %d%s", i, i == 0 ? " (usually built-in)" : "");
					if (ImGui::Selectable(camLabel, cameraIndex_ == i)) cameraIndex_ = i;
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine(0, px(8));
			if (primaryButton(ICON_LINK " Connect", ImVec2(-FLT_MIN, 0), c::ok)) { batch.clear(); openSource(std::to_string(cameraIndex_)); }
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
		ImGui::SameLine(leftW - px(36) - ImGui::CalcTextSize(ICON_PLAY " Use the sample").x - px(28));
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
			ImGui::Text(ICON_FILE_VIDEO "  %s", displayName(batch[i]).c_str());
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
				+ "   " + displayName(r);
			if (ImGui::Selectable(label.c_str(), r == source, 0, ImVec2(0, px(26)))) openSource(r);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", r.c_str());
			ImGui::PopID();
		}
	endCard();
	ImGui::EndGroup();

	// Right column: preview, facts about the source, config and output folders.
	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	beginCard("preview", ICON_EYE, "Preview", ImVec2(rightW, 0));
	float pw = ImGui::GetContentRegionAvail().x;
	drawFrame(player.isOpen() ? player.texture().imguiId() : nullptr, ImVec2(pw, pw * 9 / 16), "Pick a source");
	if (player.isOpen()) {
		const SourceInfo& in = player.info();
		auto fact = [&](const char* k, const std::string& v) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
			ImGui::TextUnformatted(k);
			ImGui::PopStyleColor();
			ImGui::SameLine(px(130));
			ImGui::TextUnformatted(v.c_str());
		};
		fact("Source", in.label);
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
		check(in.height >= 720, "HD or better: plenty of detail", "Below 720p: bumpers get blurry; 1280×720 or better is best");
		if (in.kind == SourceInfo::Image) check(false, "", "A still photo: use it to plan zones; tracking needs video");
		else check(in.fps == 0 || in.fps >= 24, "Frame rate is good for timing", "Under 24 fps: fast cars jump too far between frames");
	}
	else if (!player.error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad));
		ImGui::TextWrapped(ICON_WARN "  %s", player.error.c_str());
		ImGui::PopStyleColor();
	}
	if (player.isOpen() && ghostButton(ICON_VECTOR "  Set up zones  " ICON_ARROW_RIGHT, ImVec2(-FLT_MIN, 0))) page = Page::Zones;
	endCard();

	ImGui::Dummy(ImVec2(0, px(4)));
	beginCard("files", ICON_FOLDER, "Settings and output", ImVec2(rightW, 0));
	formLabel("Config", px(96));
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
	formLabel("Results go to", px(96));
	ImGui::TextUnformatted(dataDir().c_str());
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
	float avail = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
	float inspW = px(372);
	float canvasW = avail - inspW - px(20);
	float barH = px(48);
	ImGui::BeginGroup();
	float canvasH = std::min(availH - barH - px(10), canvasW * 9 / 16);
	ImGui::BeginChild("zcanvas", ImVec2(canvasW, canvasH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	View v = drawFrame(player.isOpen() ? player.texture().imguiId() : nullptr, ImGui::GetContentRegionAvail(),
		"Open a video, camera or photo of your street to draw on");
	OverlayStyle st;
	st.editable = true;
	const char* hovered = nullptr;
	zoneOverlay(v, cfg.g, st, &hovered);
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
	const Key keys[] = {
		{ c::accent2, "Analysis box", "the only part of the picture the tracker looks at" },
		{ c::rgb(0xFFFFFF), "Speed lines", "timing runs from one to the other" },
		{ c::l2r, "→ lane", "hubcap height of cars going right" },
		{ c::r2l, "← lane", "hubcap height of cars going left" },
		{ c::obstruction, "Obstruction", "the tracker coasts through it" },
	};
	float kw = (canvasW - px(36)) / 3;
	for (int i = 0; i < 5; i++) {
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
	const float col = px(150);
	auto intRow = [&](const char* label, int* v, int lo, int hi, const char* help = nullptr) {
		formLabel(label, col);
		ImGui::PushID(label);
		ImGui::DragInt("##v", v, 1, lo, hi);
		ImGui::PopID();
		if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", help);
	};

	beginCard("street", ICON_ROAD, "Your street");
	dimText("Which way may traffic go? On a one-way street VST calls out everyone driving the other way.");
	int legal = g.LegalDirection == "L2R" ? 1 : g.LegalDirection == "R2L" ? 2 : 0;
	static const char* const legalLabels[] = { "Two-way", "→ only", "← only" };
	if (segmented("legal", &legal, legalLabels, 3)) g.LegalDirection = legal == 1 ? "L2R" : legal == 2 ? "R2L" : "Both";
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
	compassCombo("→ heads", g.L2RDirection, c::l2r);
	compassCombo("← heads", g.R2LDirection, c::r2l);
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("zone", ICON_STOPWATCH, "Speed zone");
	dimText("Timing starts when a front bumper crosses one white line and stops at the other. Wider is more accurate: aim for at least a second at the speed limit.");
	intRow("Left line", &g.speedLineLeft, 0, g.AnalysisBoxWidth);
	intRow("Right line", &g.speedLineRight, 0, g.AnalysisBoxWidth);
	double secs = g.CalibrationFramesL2R / g.CalibrationFPS * g.CalibrationSpeed / std::max(prefs.speedLimit, 1);
	ImGui::PushStyleColor(ImGuiCol_Text, v4(secs >= 1 ? c::ok : c::warn));
	ImGui::TextWrapped("%s  About %.1f s for a car at %d mph (from the calibration)", secs >= 1 ? ICON_CHECK_CIRCLE : ICON_WARN, secs, prefs.speedLimit);
	ImGui::PopStyleColor();
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("box", ICON_VECTOR, "Analysis box");
	dimText("Just the street: leave out treetops, parked cars and the sidewalk so they can't fool the tracker.");
	intRow("Left", &g.AnalysisBoxLeft, 0, 1279);
	intRow("Top", &g.AnalysisBoxTop, 0, 719);
	intRow("Width", &g.AnalysisBoxWidth, 40, 1280);
	intRow("Height", &g.AnalysisBoxHeight, 30, 720);
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("lanes", ICON_ARROWS_LR, "Lanes");
	dimText("Put each dashed line at hubcap height for cars in that lane.");
	intRow("← lane", &g.R2LStreetY, 1, g.AnalysisBoxHeight);
	intRow("→ lane", &g.L2RStreetY, 1, g.AnalysisBoxHeight);
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
		intRow("Max → step", &g.maxL2RDistOnEntry, 5, 400, "Fastest trackable motion, pixels per two frames.");
		intRow("Max ← step", &g.maxR2LDistOnEntry, 5, 400, "Fastest trackable motion, pixels per two frames.");
		intRow("Entry look-back", &g.entryLookBack, 0, 1280);
		intRow("Slowest logged", &g.minLoggedSpeed, 0, 100, "Vehicles timed below this many mph (walkers, cyclists) are counted in the run manifest but not written to the csv. 0 logs every timed vehicle.");
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

// Speed you drove, as an exact whole number with -/+ steps (a slider can't land on 27 reliably).
static void speedStepper(const char* id, int* mph) {
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputInt(id, mph, 1, 5);
	*mph = std::clamp(*mph, 5, 80);
}

void App::pageCalibrate() {
	Globals& g = cfg.g;
	float avail = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
	float sideW = px(400);
	float canvasW = avail - sideW - px(20);
	float barH = px(56);

	ImGui::BeginGroup();
	float canvasH = std::min(availH - barH - px(10), canvasW * 9 / 16);
	ImGui::BeginChild("ccanvas", ImVec2(canvasW, canvasH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	View v = drawFrame(player.isOpen() ? player.texture().imguiId() : nullptr, ImGui::GetContentRegionAvail(), "Open the video of your calibration drive");
	OverlayStyle st;
	st.opacity = .75f;
	st.dimOutside = false;
	zoneOverlay(v, g, st);
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
	// numbers), otherwise how the calibration is used.
	if (lastCheck_.valid) calibrationCheckCard(canvasW);
	else {
		beginCard("how", ICON_INFO, "How VST turns frames into speed", ImVec2(canvasW, 0));
		ImGui::PushTextWrapPos(0);
		dimText("VST never measures feet. It counts the frames a front bumper takes to get from one white line to the other and "
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
	static const char* const modes[] = { "Drive-through", "Count frames" };
	segmented("cmode", &calibMode_, modes, 2);
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
		}
	};
	// A pass can be checked once its direction has a calibration, and never against the pass that calibration was
	// made from (it would agree with itself to the rounding).  Returns why not, or nullptr.
	auto whyNoCheck = [&](bool l2r, int vehicleId) -> const char* {
		if ((l2r ? calL2R : calR2L).kind == CalState::None) return "Calibrate this direction first; then a second pass can check it.";
		if (vehicleId && vehicleId == calibAppliedId_) return "That's the pass the calibration came from. Pick a new pass.";
		return nullptr;
	};

	if (calibMode_ == 0) {
		beginCard("drive", calibChecking_ ? ICON_BULLSEYE : ICON_CAR, calibChecking_ ? "Check it with a second drive" : "Drive through at a known speed");
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
		if (calibChecking_) {
			stepLine(1, "Drive past again on cruise control, ideally at a different speed from your first pass.");
			stepLine(2, "Pick that pass and enter the speed you drove. Within ±2 mph is verified.");
		}
		else {
			stepLine(1, "Start tracking (live, or record a video). The zones should already be set.");
			stepLine(2, "Drive past on cruise control at a speed you know, e.g. 25 mph. Once each way on a two-way street.");
			stepLine(3, "Pick your pass below, enter the speed you drove, and apply.");
		}
		ImGui::Dummy(ImVec2(0, px(2)));
		auto vs = tracker.vehiclesSince(0);
		if (vs.empty()) dimText("No vehicles measured yet this session. Run tracking first, then come back here.");
		else {
			// Five rows while checking (the newest passes are the likely ones), so the Current calibration card with
			// the per-direction verdicts stays in view below.
			ImGui::BeginChild("picklist", ImVec2(0, calibChecking_ ? ImGui::GetTextLineHeightWithSpacing() * 5 + ImGui::GetStyle().FramePadding.y * 2 : px(240)), ImGuiChildFlags_FrameStyle);
			for (auto it = vs.rbegin(); it != vs.rend(); ++it) {
				ImGui::PushID(it->id);
				char label[180];
				snprintf(label, sizeof label, "%s  %s   %d frames   (now reads %d mph)   %s%s", it->ev.leftToRight ? "→" : "←",
					it->ev.leftToRight ? "L→R" : "R→L", it->frames(), it->ev.speed, it->time.substr(11).c_str(),
					it->id == calibAppliedId_ ? "   calibrated from" : "");
				if (ImGui::Selectable(label, calibPickId_ == it->id)) calibPickId_ = it->id;
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
		formLabel("I drove at (mph)", px(130));
		speedStepper("##spd", &calibSpeed_);
		Vehicle picked;
		bool have = calibPickId_ > 0 && tracker.vehicle(calibPickId_, picked);
		if (calibChecking_) {
			const char* why = have ? whyNoCheck(picked.ev.leftToRight, picked.id) : nullptr;
			ImGui::BeginDisabled(!have || why);
			if (primaryButton(ICON_BULLSEYE "  Check this pass", ImVec2(-FLT_MIN, 0))) checkCalibration(picked.ev.leftToRight, calibSpeed_, picked.frames(), picked.fps);
			ImGui::EndDisabled();
			if (why) { ImGui::PushTextWrapPos(0); dimText("%s", why); ImGui::PopTextWrapPos(); }
		}
		else {
			ImGui::BeginDisabled(!have);
			if (primaryButton(ICON_CHECK "  Use this pass", ImVec2(-FLT_MIN, 0))) {
				applyCalibration(picked.ev.leftToRight, calibSpeed_, picked.frames(), picked.fps);
				calibAppliedId_ = picked.id;
			}
			ImGui::EndDisabled();
		}
		checkOffer();
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
		formLabel("I drove at (mph)", px(130));
		speedStepper("##spd2", &calibSpeed_);
		if (calibChecking_) {
			const char* why = whyNoCheck(calibDir_ == 0, 0);
			ImGui::BeginDisabled(!frames || why);
			if (primaryButton(ICON_BULLSEYE "  Check", ImVec2(-FLT_MIN, 0))) checkCalibration(calibDir_ == 0, calibSpeed_, frames, fps);
			ImGui::EndDisabled();
			if (why) { ImGui::PushTextWrapPos(0); dimText("%s", why); ImGui::PopTextWrapPos(); }
		}
		else {
			ImGui::BeginDisabled(!frames);
			if (primaryButton(ICON_CHECK "  Apply", ImVec2(-FLT_MIN, 0))) applyCalibration(calibDir_ == 0, calibSpeed_, frames, fps);
			ImGui::EndDisabled();
		}
		if (ImGui::SmallButton("Clear marks")) m[0] = m[1] = -1;
		checkOffer();
		endCard();
	}

	ImGui::Dummy(ImVec2(0, px(4)));
	beginCard("calnow", ICON_GAUGE, "Current calibration");
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
			"A different lane: the far lane crosses more road between the lines." })
			dimText("•  %s", t);
	}
	ImGui::PopTextWrapPos();
	if (!k.verified) {
		// What averaging would set, so the button says exactly what it does.
		double second = calib::impliedFrames(k.drivenMph, k.frames, k.fps, g.CalibrationSpeed, g.CalibrationFPS);
		int avg = std::max(1, int(std::lround((k.calFrames + second) / 2)));
		ImGui::Dummy(ImVec2(0, px(2)));
		float bw = (textW - px(10)) * .5f;
		if (ghostButton(ICON_ROTATE "  Redo calibration", ImVec2(bw, 0))) {
			calibChecking_ = false;
			calibPickId_ = 0;
			lastCheck_ = CalCheck();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drive past again and calibrate from that pass.");
		ImGui::SameLine(0, px(10));
		char label[64];
		snprintf(label, sizeof label, ICON_ARROWS_LR "  Average: %d frames", avg);
		if (primaryButton(label, ImVec2(bw, 0))) averageCalibration(k);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Average the two passes: %d frames from the first, %.1f from this one.\nThen check the average with one more pass.", k.calFrames, second);
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
	View v = drawFrame(tex, area, "Press Start tracking to begin");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// Motion the tracker sees, tinted over the analysis box, under the zone and box overlays.
	if (prefs.showMotion && run && motionTex_.valid()) {
		const cv::Rect& ab = live_.analysisBox;
		dl->AddImage((ImTextureID)(intptr_t)motionTex_.imguiId(), v.at(float(ab.x), float(ab.y)), v.at(float(ab.x + ab.width), float(ab.y + ab.height)));
	}
	OverlayStyle st;
	st.opacity = run ? .3f : .5f;   // while tracking, the zones step back so the vehicles and their speeds read first
	st.dimOutside = true;
	zoneOverlay(v, cfg.g, st);
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
		if (primaryButton(ICON_PLAY "  Start tracking", bsz)) startTracking();
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
		if (recent.empty()) dimText("Vehicles appear here the moment they leave the speed zone. Click one for details.");
		bool first = true;
		for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
			if (!first) ImGui::SameLine(0, px(10));
			first = false;
			ImGui::BeginGroup();
			vehicleCard(*it, px(300));
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
	float tw = (sideW - px(12)) * .5f, th = px(104);
	char sub[64];
	snprintf(sub, sizeof sub, "avg %.0f mph", cn.avgSpeed);
	statTile(ICON_CAR, "Vehicles", std::to_string(cn.vehicles), sub, c::accent, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "%d%% of traffic", cn.vehicles ? cn.speeders * 100 / cn.vehicles : 0);
	statTile(ICON_GAUGE, "Speeding", std::to_string(cn.speeders), sub, c::warn, ImVec2(tw, th));
	snprintf(sub, sizeof sub, "%d+ mph", prefs.egregious);
	statTile(ICON_BOLT, "Way over", std::to_string(cn.egregious), sub, c::bad, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	if (cfg.g.LegalDirection != "Both") statTile(ICON_WARN, "Wrong way", std::to_string(cn.wrongWay), "on a one-way street", c::wrong, ImVec2(tw, th));
	else statTile(ICON_FLAG, "Top speed", cn.topSpeed ? std::to_string(cn.topSpeed) + " mph" : "—", "this session", c::accent2, ImVec2(tw, th));

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
			pill((std::to_string(v.ev.speed) + " mph").c_str(), c::bg0, col, fonts.strong);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, v4(v.ev.leftToRight ? c::l2r : c::r2l));
			ImGui::Text("%s %s", v.ev.leftToRight ? "→" : "←", v.ev.compass.c_str());
			ImGui::PopStyleColor();
			if (v.ev.wrongWay) { ImGui::SameLine(); pill(ICON_WARN " wrong way", c::rgb(0xFFFFFF), c::wrong); }
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
	if (ImGui::InputInt("##limit", &prefs.speedLimit, 5, 5)) {
		prefs.speedLimit = std::clamp(prefs.speedLimit, 5, 80);
		prefs.egregious = std::max(prefs.egregious, prefs.speedLimit);
	}
	formLabel("Way over from", px(130));
	if (ImGui::InputInt("##egr", &prefs.egregious, 5, 5)) prefs.egregious = std::max(prefs.egregious, prefs.speedLimit);
	ImGui::EndDisabled();
	formLabel("Playback", px(130));
	int rt = prefs.realtime ? 0 : 1;
	static const char* const pace[] = { "Real time", "Fast" };
	if (segmented("pace", &rt, pace, 2)) { prefs.realtime = rt == 0; tracker.setRealtime(prefs.realtime); }
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
	std::string when = v.time.size() > 11 ? v.time.substr(11) : v.time;
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
	if (rows.empty() && rowsFrom.empty() && tracker.lastVehicleId() > 0) showSessionResults();

	// Toolbar: filter, where the rows come from, file actions.
	static const char* const filters[] = { "All", "Speeding", "Way over", "Wrong way" };
	segmented("rfilter", &resultsFilter_, filters, 4, px(440));
	ImGui::SameLine(0, px(16));
	ImGui::SetNextItemWidth(px(180));
	ImGui::InputTextWithHint("##search", ICON_SEARCH "  min speed or source", searchBuf_, sizeof searchBuf_);
	ImGui::SameLine(0, px(16));
	if (ghostButton(ICON_CLOCK " This session")) showSessionResults();
	ImGui::SameLine();
	if (ghostButton(ICON_FILE_CSV " Open csv")) {
		auto r = pfd::open_file("Open stats csv", (fs::path(dataDir()) / "Stats").string(), { "Stats csv", "*.csv", "All files", "*" }).result();
		if (!r.empty()) loadResultsCsv(r[0]);
	}
	ImGui::SameLine();
	if (ghostButton(ICON_FILM " Highlights")) openWithSystem((fs::path(dataDir()) / "HiLites").string());
	ImGui::SameLine();
	if (ghostButton(ICON_FOLDER_OPEN " Folder")) openWithSystem((fs::path(dataDir()) / "Stats").string());
	ImGui::SameLine();
	if (primaryButton(ICON_REPORT "  Make report")) { reportForm_.saved.clear(); reportForm_.error.clear(); ImGui::OpenPopup("Street report"); }
	reportDialog();

	// Filtered view.
	int minSpeed = 0;
	std::string search = searchBuf_;
	try { if (!search.empty()) minSpeed = std::stoi(search); } catch (...) {}
	std::vector<const ResultRow*> shown;
	for (const ResultRow& r : rows) {
		if (resultsFilter_ == 1 && r.speed <= prefs.speedLimit) continue;
		if (resultsFilter_ == 2 && r.speed < prefs.egregious) continue;
		if (resultsFilter_ == 3 && !r.wrongWay) continue;
		if (minSpeed > 0 && r.speed < minSpeed) continue;
		if (!search.empty() && minSpeed == 0 && r.source.find(search) == std::string::npos) continue;
		shown.push_back(&r);
	}
	dimText("%s  ·  %zu of %zu vehicles", rowsFrom.empty() ? "This session" : fs::path(rowsFrom).filename().string().c_str(), shown.size(), rows.size());

	if (rows.empty()) {
		ImGui::Dummy(ImVec2(0, px(40)));
		beginCard("noresults", ICON_CHART, "Nothing here yet", ImVec2(avail, 0));
		dimText("Run tracking on a video or camera and every measured vehicle lands here, with its speed, direction and a snapshot. "
			"Or open a stats csv from an earlier run.");
		if (primaryButton(ICON_PLAY "  Start tracking")) startTracking();
		endCard();
		return;
	}

	// Summary tiles across the top.
	int over = 0, way = 0, wrong = 0, top = 0;
	double sum = 0;
	std::vector<int> speeds;
	for (const ResultRow& r : rows) {
		over += r.speed > prefs.speedLimit;
		way += r.speed >= prefs.egregious;
		wrong += r.wrongWay;
		top = std::max(top, r.speed);
		sum += r.speed;
		speeds.push_back(r.speed);
	}
	std::sort(speeds.begin(), speeds.end());
	int p85 = report::percentile(speeds, 85);   // nearest rank, the same definition as the street report and the phone
	float tw = (avail - px(48)) / 5, th = px(100);
	char sub[64];
	statTile(ICON_CAR, "Vehicles", std::to_string(rows.size()), rowsFrom.empty() ? "this session" : "in this file", c::accent, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "%d%% over %d mph", int(over * 100 / rows.size()), prefs.speedLimit);
	statTile(ICON_GAUGE, "Speeding", std::to_string(over), sub, c::warn, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "%d mph or more", prefs.egregious);
	statTile(ICON_BOLT, "Way over", std::to_string(way), sub, c::bad, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	statTile(ICON_WARN, "Wrong way", std::to_string(wrong), cfg.g.LegalDirection == "Both" ? "set a one-way direction in Zones" : "against the one-way", c::wrong, ImVec2(tw, th));
	ImGui::SameLine(0, px(12));
	snprintf(sub, sizeof sub, "avg %.0f · top %d mph", sum / rows.size(), top);
	statTile(ICON_CHART, "85th percentile", std::to_string(p85) + " mph", sub, c::accent2, ImVec2(tw, th));

	ImGui::Dummy(ImVec2(0, px(2)));
	float detailW = px(340);
	float leftW = avail - detailW - px(20);
	float chartsH = px(210);
	ImGui::BeginGroup();
	resultsCharts(shown, leftW);
	ImGui::Dummy(ImVec2(0, px(2)));

	float tableH = ImGui::GetContentRegionAvail().y;
	if (ImGui::BeginTable("rows", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable
		| ImGuiTableFlags_SizingStretchProp, ImVec2(leftW, tableH))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, px(46));
		ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, px(110));
		ImGui::TableSetupColumn("Direction", 0, 1.3f);
		ImGui::TableSetupColumn("When", 0, 1.2f);
		ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, px(70));
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, px(70));
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
					int kx = colIdx == 1 ? x->speed : colIdx == 4 ? x->frames : colIdx == 5 ? x->area : a;
					int ky = colIdx == 1 ? y->speed : colIdx == 4 ? y->frames : colIdx == 5 ? y->area : b;
					return asc ? kx < ky : kx > ky;
				});
			}
		ImGuiListClipper clip;
		clip.Begin(int(order.size()));
		while (clip.Step())
			for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
				const ResultRow& r = *shown[order[k]];
				int idx = int(&r - rows.data());
				ImGui::TableNextRow(0, px(30));
				ImGui::TableNextColumn();
				ImGui::PushID(idx);
				char num[16];
				snprintf(num, sizeof num, "%d", idx + 1);
				if (ImGui::Selectable(num, selectedRow_ == idx, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, px(24)))) selectedRow_ = idx;
				ImGui::PopID();
				ImGui::TableNextColumn();
				std::string s = std::to_string(r.speed) + " mph";
				ImU32 col = speedColor(r.speed, prefs.speedLimit, prefs.egregious);
				pill(s.c_str(), c::bg0, col, fonts.strong);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, v4(r.l2r ? c::l2r : c::r2l));
				ImGui::Text("%s %s", r.l2r ? "→" : "←", r.compass.c_str());
				ImGui::PopStyleColor();
				if (r.wrongWay) { ImGui::SameLine(); pill(ICON_WARN " wrong way", c::rgb(0xFFFFFF), c::wrong); }
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				if (!r.time.empty()) ImGui::TextUnformatted(r.time.size() > 11 ? r.time.substr(11).c_str() : r.time.c_str());
				else ImGui::Text("%d:%02d in video", int(r.frame / r.fps) / 60, int(r.frame / r.fps) % 60);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%d", r.frames);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				if (r.large) ImGui::Text(ICON_TRUCK " %d", r.area);
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
		Texture* t = r.vehicleId ? snapshotTexture(r.vehicleId) : nullptr;
		if (t) ImGui::Image((ImTextureID)(intptr_t)t->imguiId(), ImVec2(w, w * t->height() / std::max(1, t->width())));
		else {
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, w * .4f), c::bg3, px(8));
			ImGui::GetWindowDrawList()->AddText(p + ImVec2(px(12), px(12)), c::faint, "Snapshots are kept for this session's vehicles");
			ImGui::Dummy(ImVec2(w, w * .4f));
		}
		ImU32 col = r.wrongWay ? c::wrong : speedColor(r.speed, prefs.speedLimit, prefs.egregious);
		ImGui::PushFont(fonts.hero);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
		ImGui::Text("%d", r.speed);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::SameLine();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(26));
		dimText("mph");
		if (r.wrongWay) pill(ICON_WARN "  WRONG WAY", c::rgb(0xFFFFFF), c::wrong, fonts.strong);
		else if (r.speed >= prefs.egregious) pill(ICON_BOLT "  WAY OVER THE LIMIT", c::bg0, c::bad, fonts.strong);
		else if (r.speed > prefs.speedLimit) pill(ICON_GAUGE "  SPEEDING", c::bg0, c::warn, fonts.strong);
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
		kv("In the zone", std::to_string(r.frames) + " frames (" + std::to_string(r.frames / r.fps).substr(0, 4) + " s)");
		kv("Size", std::to_string(r.area) + (r.large ? "  (truck or bus)" : ""));
		kv("Frame", std::to_string(r.frame));
		if (!r.time.empty()) kv("Passed at", r.time);
		kv("Source", r.source);
		ImGui::Dummy(ImVec2(0, px(4)));
		Vehicle v;
		if (r.vehicleId && tracker.vehicle(r.vehicleId, v) && !v.clipPath.empty())
			if (ghostButton(ICON_FILM "  Play clip", ImVec2(-FLT_MIN, 0))) openWithSystem(v.clipPath);
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
		for (int i = 0; i < bins; i++) {
			int mph = lo + i * 2;
			float bh = (sz.y - px(4)) * counts[i] / maxC;
			ImVec2 a(p.x + i * bw + 1, p.y + sz.y - bh), b(p.x + (i + 1) * bw - 1, p.y + sz.y);
			bool hov = ImGui::IsMouseHoveringRect(ImVec2(a.x, p.y), b);
			if (hov) hoverBin = i;
			if (counts[i]) dl->AddRectFilled(a, b, alpha(speedColor(mph, prefs.speedLimit, prefs.egregious), hov ? 1.f : .8f), px(3), ImDrawFlags_RoundCornersTop);
		}
		float lx = p.x + (prefs.speedLimit + 1 - lo) / 2.f * bw;
		for (float y = p.y; y < p.y + sz.y; y += px(8)) dl->AddLine(ImVec2(lx, y), ImVec2(lx, y + px(4)), c::text, 1.5f);
		std::string lim = std::to_string(prefs.speedLimit) + " mph limit";
		dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(lx + px(6), p.y), c::text, lim.c_str());
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
		float ly = p.y + sz.y - sz.y * prefs.speedLimit / top;
		dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + sz.x, ly), alpha(c::bad, .04f));
		for (float x = p.x; x < p.x + sz.x; x += px(8)) dl->AddLine(ImVec2(x, ly), ImVec2(x + px(4), ly), alpha(c::text, .6f), 1.f);
		size_t n = shown.size();
		const ResultRow* hov = nullptr;
		for (size_t i = 0; i < n; i++) {
			const ResultRow* r = shown[i];
			float x = p.x + (n > 1 ? sz.x * i / (n - 1) : sz.x * .5f);
			float y = p.y + sz.y - sz.y * r->speed / top;
			ImU32 col = speedColor(r->speed, prefs.speedLimit, prefs.egregious);
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
	if (!snapshots) return in;
	// Example frames exist only for this session's vehicles (kept in memory).  Take the six fastest in the date range,
	// each made safe by blurForReport before it ever reaches the report.
	std::vector<Vehicle> vs = tracker.vehiclesSince(0);
	std::sort(vs.begin(), vs.end(), [](const Vehicle& a, const Vehicle& b) { return a.ev.speed > b.ev.speed; });
	for (const Vehicle& v : vs) {
		if (in.snapshots.size() >= 6) break;
		std::string when = v.live ? v.time : v.recorded;
		std::string day = when.substr(0, 10);
		if (v.snapshot.empty() || (!from.empty() && (day.empty() || day < from)) || (!to.empty() && (day.empty() || day > to))) continue;
		std::string jpeg = report::blurForReport(v.snapshot);
		if (!jpeg.empty()) in.snapshots.push_back({ jpeg, v.ev.speed, v.ev.leftToRight, when });
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
	ImGui::SetNextWindowSize(ImVec2(px(560), 0), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Street report", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
	ReportForm& f = reportForm_;
	float col = px(150);
	dimText("A printable study of the speeds in your data folder, for a city council or a traffic engineer. "
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
	if (f.snapshots) { ImGui::SetCursorPosX(col); dimText("Only this session's vehicles have snapshots. Plates are blurred and nothing identifying is added."); }
	formLabel("Note", col);
	ImGui::InputTextMultiline("##rnote", f.note, sizeof f.note, ImVec2(-FLT_MIN, px(70)));
	ImGui::SetCursorPosX(col);
	dimText("Limit %d mph, way over %d mph (set on the Live page).", prefs.speedLimit, prefs.egregious);

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
			s.from = f.from;
			s.to = f.to;
			s.note = f.note;
			s.includeSnapshots = f.snapshots;
			if (f.compare) { s.compareFrom = f.cmpFrom; s.compareTo = f.cmpTo; }
			std::string path, html;
			if (report::writeReport(reportInput(f.snapshots, s.from, s.to), s, path, html, f.error)) {
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
	bool on = server && server->running();

	beginCard("remote", ICON_BROADCAST, "Remote companion", ImVec2(leftW, 0));
	dimText("Open VST on your phone to check the camera's aim, help with calibration from the street, and see recent speeders, "
		"while this computer keeps tracking. Your phone needs to be on the same Wi-Fi network.");
	ImGui::Dummy(ImVec2(0, px(4)));
	bool want = on;
	if (ImGui::Checkbox("Allow the companion app to connect", &want)) setRemote(want);
	ImGui::BeginDisabled(on);
	formLabel("Port", px(110));
	ImGui::InputInt("##port", &prefs.remotePort, 0);
	prefs.remotePort = std::clamp(prefs.remotePort, 1024, 65535);
	ImGui::EndDisabled();
	formLabel("Site name", px(110));
	char site[96];
	snprintf(site, sizeof site, "%s", prefs.siteName.c_str());
	if (ImGui::InputText("##site", site, sizeof site)) prefs.siteName = site;
	ImGui::Dummy(ImVec2(0, px(6)));
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextWrapped(ICON_INFO "  Pairing uses a secret in the QR code. Anyone with it can see the camera and change settings, so only "
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
	if (!on) dimText("Turn on the companion to get a pairing code.");
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
		if (ghostButton(ICON_LINK "  Copy pairing link")) { ImGui::SetClipboardText(url.c_str()); toast("Copied", c::ok, ICON_CHECK); }
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
