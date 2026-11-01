// VST Studio's side of unattended-run health (Health.h has the checks themselves and the file formats).
//
// healthTick() runs every frame on the GUI thread and is cheap: it adds the frame's time to today's tracked or
// outage hours, counts outages and vehicles, rolls the day over into <data>/Stats/health.csv, and every so often
// starts the slower work:
//   * every 10 minutes (and once at startup): prune Clips/ + HiLites/ to the budget on a worker thread
//     (std::async), since listing thousands of clips shouldn't stall a frame;
//   * every 2 minutes of a live run with pictures: compare the newest frame with the reference view.  The
//     reference is taken from the first checked frame whenever there is none for the current zones, so redrawing
//     the zones (the usual fix after a bump) also resets it.  Three "moved" results in a row raise the warning;
//     "unclear" (night, fog) neither raises nor clears it.
#include "App.h"
#include "Icons.h"
#include "Theme.h"
#include "UserPaths.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <opencv2/imgcodecs.hpp>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;
using namespace theme;

namespace {

double now() { return glfwGetTime(); }

std::tm localNow() {
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	return tm;
}

std::string localStamp(const char* fmt) {
	std::tm tm = localNow();
	std::ostringstream o;
	o << std::put_time(&tm, fmt);
	return o.str();
}

// Severity order for the day's camera column: one "moved" in a day is what matters.
int cameraRank(const std::string& s) { return s == "moved" ? 3 : s == "unclear" ? 2 : s == "same" ? 1 : 0; }

std::string gb(std::uintmax_t bytes) {
	char b[32];
	snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
	return b;
}

}  // namespace

std::string App::referenceViewPath() { return (userpaths::configDir() / "view-reference.jpg").string(); }

// The zone geometry the reference view belongs to; a different signature means the zones were redrawn.
std::string App::zonesSignature() {
	const Globals& g = cfg.g;
	std::ostringstream o;
	o << g.AnalysisBoxLeft << ',' << g.AnalysisBoxTop << ',' << g.AnalysisBoxWidth << ',' << g.AnalysisBoxHeight << ','
	  << g.speedLineLeft << ',' << g.speedLineRight << ',' << g.R2LStreetY << ',' << g.L2RStreetY << ',' << source;
	// Tilted lane lines extend it; a level setup keeps the signature it always had, so existing references stay valid.
	if (g.R2LStreetYRight || g.L2RStreetYRight) o << ',' << g.R2LStreetYRight << ',' << g.L2RStreetYRight;
	return o.str();
}

void App::useCurrentViewAsReference() {
	cv::Mat f = tracker.latestFrame();
	if (f.empty()) { toast("No picture yet to use as the reference", c::warn, ICON_WARN); return; }
	std::error_code ec;
	fs::create_directories(userpaths::configDir(), ec);
	if (!cv::imwrite(referenceViewPath(), f)) { toast("Can't save the reference view in " + userpaths::configDir().string(), c::bad, ICON_WARN); return; }
	std::ofstream(referenceViewPath() + ".zones") << zonesSignature();
	health.movedStreak = 0;
	health.cameraWarning = false;
	health.view = health::ViewCheck();
	health.view.result = health::ViewCheck::Same;
	health.view.confidence = 1;
	health.viewCheckedAt = localStamp("%Y-%m-%dT%H:%M:%S");
}

void App::flushHealthDay() {
	if (screenshotMode || health.today.empty()) return;
	fs::path log = fs::path(dataDir()) / "Stats" / "health.csv";
	if (!health::appendDay(log, health.today)) toast("Can't write " + log.string(), c::bad, ICON_WARN);
	std::string date = health.today.date;
	health.today = health::Day();
	health.today.date = date;
}

void App::healthTick() {
	if (screenshotMode) return;
	double t = now();
	// Clamped so a laptop waking from sleep doesn't book the whole night as tracked.
	double dt = healthAt_ > 0 ? std::clamp(t - healthAt_, 0.0, 5.0) : 0;
	healthAt_ = t;

	std::string date = localStamp("%Y-%m-%d");
	if (health.today.date != date) {
		flushHealthDay();
		health.today = health::Day();
		health.today.date = date;
	}

	// Time and outages: only live sources count, since a recorded video says nothing about the camera today.
	Tracker::Progress prog = tracker.progress();
	bool liveRun = tracker.running() && prog.live;
	bool outage = liveRun && prog.reconnecting;
	if (liveRun) (outage ? health.today.outageHours : health.today.trackedHours) += dt / 3600;
	if (outage && !wasOutage_) health.today.outages++;
	wasOutage_ = outage;
	int n = tracker.counts().vehicles;
	if (liveRun && n > healthVehicles_) health.today.vehicles += n - healthVehicles_;
	healthVehicles_ = n;   // also follows clearVehicles() back down

	// Disk: collect a finished prune, then start the next one when due.
	if (pruning_.valid() && pruning_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		PruneJob j = pruning_.get();
		health.video = j.pruned.left;
		health.freeBytes = j.freeBytes;
		health.lowDisk = j.freeBytes > 0 && j.freeBytes < kMinFreeBytes;
		health.today.clipsRemoved += int(j.pruned.removed.size());
		if (j.freeBytes > 0) {
			double free = j.freeBytes / 1073741824.0;
			health.today.freeGB = health.today.freeGB < 0 ? free : std::min(health.today.freeGB, free);
		}
		if (health.lowDisk) toast("The disk is nearly full (" + gb(j.freeBytes) + " free). Old clips are being removed to make room.", c::warn, ICON_WARN);
	}
	health.budgetBytes = std::uintmax_t(std::max(prefs.keepVideoGB, 0)) << 30;
	if (!pruning_.valid() && t >= nextPrune_) {
		nextPrune_ = t + 600;
		fs::path data = dataDir();
		std::uintmax_t budget = health.budgetBytes;
		pruning_ = std::async(std::launch::async, [data, budget] {
			std::vector<fs::path> dirs = { data / "Clips", data / "HiLites" };
			std::uintmax_t free = health::freeBytes(data);
			PruneJob j;
			j.pruned = health::pruneTo(dirs, health::targetBytes(health::usage(dirs), budget, free, kMinFreeBytes));
			j.freeBytes = health::freeBytes(data);
			return j;
		});
	}

	// Camera check.
	if (!liveRun || outage) { nextViewCheck_ = std::max(nextViewCheck_, t + 10); return; }   // 10 s of pictures first
	if (t < nextViewCheck_) return;
	nextViewCheck_ = t + 120;
	cv::Mat f = tracker.latestFrame();
	if (f.empty()) return;
	std::string sig;
	{ std::ifstream in(referenceViewPath() + ".zones"); std::getline(in, sig); }
	cv::Mat ref = sig == zonesSignature() ? cv::imread(referenceViewPath()) : cv::Mat();
	if (ref.empty()) { useCurrentViewAsReference(); return; }
	health.view = health::compareView(ref, f);
	health.viewCheckedAt = localStamp("%Y-%m-%dT%H:%M:%S");
	if (health.view.result == health::ViewCheck::Moved) health.movedStreak++;
	else if (health.view.result == health::ViewCheck::Same) health.movedStreak = 0;
	if (cameraRank(health.view.name()) > cameraRank(health.today.camera)) health.today.camera = health.view.name();
	bool warn = health.movedStreak >= kMovedChecks;
	if (warn && !health.cameraWarning)
		toast("The camera seems to have moved. Check the zones still sit on the street.", c::bad, ICON_WARN);
	health.cameraWarning = warn;
}

void App::healthCard() {
	beginCard("health", ICON_CHECK_CIRCLE, "Unattended health");
	const health::Day& d = health.today;
	// Camera.
	if (health.cameraWarning) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad));
		ImGui::TextWrapped(ICON_WARN "  The camera seems to have moved by %.0f px since the zones were set. Speeds and directions may be wrong until it's re-aimed or the zones are redrawn.",
			std::hypot(health.view.dx, health.view.dy));
		ImGui::PopStyleColor();
		if (ghostButton(ICON_VECTOR "  Redraw zones")) setPage(Page::Zones);
		ImGui::SameLine();
		if (ghostButton(ICON_CHECK "  It's fine, use this view")) useCurrentViewAsReference();
	}
	else if (health.viewCheckedAt.empty()) dimText(ICON_CAMERA "  Camera position: checked every 2 min on a live camera");
	else if (health.view.result == health::ViewCheck::Unclear) dimText(ICON_CAMERA "  Camera position: too dark to tell (%s)", health.viewCheckedAt.substr(11, 5).c_str());
	else dimText(ICON_CAMERA "  Camera position: unchanged (%s)", health.viewCheckedAt.substr(11, 5).c_str());
	// Today.
	if (!d.empty()) {
		int mins = int(d.outageHours * 60 + .5);
		dimText(ICON_CLOCK "  Today: %.1f h live, %d outage%s%s", d.trackedHours, d.outages, d.outages == 1 ? "" : "s",
			mins ? (" (" + std::to_string(mins) + " min)").c_str() : "");
	}
	// Disk.
	// The budget from the setting itself: health.budgetBytes is only filled in at the first prune.
	if (prefs.keepVideoGB > 0) dimText(ICON_FOLDER "  Clips and highlights: %s of %s", gb(health.video.bytes).c_str(), gb(std::uintmax_t(prefs.keepVideoGB) << 30).c_str());
	else dimText(ICON_FOLDER "  Clips and highlights: %s, no limit", gb(health.video.bytes).c_str());
	ImGui::PushStyleColor(ImGuiCol_Text, v4(health.lowDisk ? c::warn : c::dim));
	ImGui::TextUnformatted(health.freeBytes ? (ICON_SAVE "  Disk: " + gb(health.freeBytes) + " free" + (health.lowDisk ? ", nearly full" : "")).c_str()
		: ICON_SAVE "  Disk: free space not known yet");
	ImGui::PopStyleColor();
	formLabel("Keep up to (GB)", px(150));
	if (ImGui::InputInt("##keepgb", &prefs.keepVideoGB, 5, 10)) {
		prefs.keepVideoGB = std::clamp(prefs.keepVideoGB, 0, 100000);
		prefs.save();
		nextPrune_ = 0;   // apply a lower budget now rather than in up to 10 minutes
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("When clips and highlight videos pass this size, the oldest are removed.\n0 keeps everything. Stats, reports and calibration records are never removed.");
	endCard();
}
