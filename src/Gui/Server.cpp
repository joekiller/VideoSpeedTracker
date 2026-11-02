#include <httplib.h>   // first: it pulls in winsock2.h, which must precede windows.h
#include "Server.h"
#include "App.h"
#include "Settings.h"
#include "Resume.h"
#include "Icons.h"
#include "Theme.h"
#include "UserPaths.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <qrcodegen.hpp>
#include <algorithm>
#include <filesystem>
#include <map>
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifndef VST_VERSION
#define VST_VERSION "dev"
#endif

std::string lanAddress() {
	// "Connect" a UDP socket to a public address: no packet is sent, but the OS picks the outbound interface,
	// whose address getsockname() then reports.
	std::string ip = "127.0.0.1";
	auto s = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
	if (s == INVALID_SOCKET) return ip;
#else
	if (s < 0) return ip;
#endif
	sockaddr_in dst{};
	dst.sin_family = AF_INET;
	dst.sin_port = htons(53);
	inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
	if (connect(s, (sockaddr*)&dst, sizeof dst) == 0) {
		sockaddr_in me{};
		socklen_t len = sizeof me;
		char buf[INET_ADDRSTRLEN] = {};
		if (getsockname(s, (sockaddr*)&me, &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) ip = buf;
	}
#ifdef _WIN32
	closesocket(s);
#else
	close(s);
#endif
	return ip;
}

std::vector<std::string> lanAddresses() {
	std::vector<std::string> out;
	auto add = [&out](const in_addr& a) {
		char buf[INET_ADDRSTRLEN] = {};
		if (!inet_ntop(AF_INET, &a, buf, sizeof buf)) return;
		std::string ip = buf;
		// 127/8 is this machine only and 169.254/16 is what an interface gets when DHCP failed: no phone reaches either.
		if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) return;
		if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
	};
#ifdef _WIN32
	// Windows: the host name resolves to every address of every adapter that's up, without linking iphlpapi.
	char host[256] = {};
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	if (gethostname(host, sizeof host) == 0 && getaddrinfo(host, nullptr, &hints, &res) == 0) {
		for (addrinfo* p = res; p; p = p->ai_next) add(((sockaddr_in*)p->ai_addr)->sin_addr);
		freeaddrinfo(res);
	}
#else
	// POSIX: the host name often resolves to 127.0.1.1 only, so walk the interfaces instead.
	ifaddrs* ifs = nullptr;
	if (getifaddrs(&ifs) == 0) {
		for (ifaddrs* p = ifs; p; p = p->ifa_next)
			if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && (p->ifa_flags & IFF_UP)) add(((sockaddr_in*)p->ifa_addr)->sin_addr);
		freeifaddrs(ifs);
	}
#endif
	// The outbound interface first: it's the automatic choice, and the one most likely to share the phone's Wi-Fi.
	std::string guess = lanAddress();
	auto it = std::find(out.begin(), out.end(), guess);
	if (it != out.end()) std::rotate(out.begin(), it, it + 1);
	return out;
}

namespace {

std::string lowerDir(const std::string& s) { return s == "L2R" ? "L2R" : s == "R2L" ? "R2L" : "both"; }

// A pass's speed trend (SpeedTrend.h), rounded as the csv rounds it, or null when the tracker couldn't tell.
json trendJson(const vst::SpeedTrend& t) {
	if (!t.known()) return nullptr;
	return json{ { "kind", vst::trendName(t.trend) }, { "entry", int(t.entryMph + 0.5) }, { "exit", int(t.exitMph + 0.5) },
		{ "accel", std::round(t.accelMphPerSec * 10) / 10 }, { "in20ft", int(t.projectedMph + 0.5) } };
}

json vehicleJson(const Vehicle& v) {
	return json{
		{ "id", v.id }, { "time", v.live || v.recorded.empty() ? v.time : v.recorded }, { "video", v.video }, { "frame", v.ev.frame },
		{ "dir", v.ev.leftToRight ? "L2R" : "R2L" }, { "compass", v.ev.compass }, { "wrongWay", v.ev.wrongWay },
		{ "speed", v.ev.speed }, { "speeding", v.speeding }, { "egregious", v.egregious },
		{ "area", v.ev.area }, { "large", v.large }, { "frames", v.frames() },
		{ "startFrame", v.ev.startFrame }, { "endFrame", v.ev.endFrame }, { "fps", v.fps },
		{ "snapshot", v.belowMin ? json(nullptr) : json("/api/vehicles/" + std::to_string(v.id) + "/snapshot.jpg") },
		{ "clip", v.clipPath.empty() ? json(nullptr) : json("/api/vehicles/" + std::to_string(v.id) + "/clip.mp4") },
		{ "belowMin", v.belowMin },
		{ "trend", trendJson(v.ev.trend) },
	};
}

// "yyyymmdd hhmmss" (a csv from before the Timestamp column: Date and Time, which for a Foscam recording is when it
// started) as ISO local time, "yyyy-mm-ddThh:mm:ss"; "" when it isn't that shape.
std::string isoFromDateTime(const std::string& date, const std::string& time) {
	auto digits = [](const std::string& x, size_t n) { return x.size() == n && std::all_of(x.begin(), x.end(), [](char ch) { return ch >= '0' && ch <= '9'; }); };
	if (!digits(date, 8)) return "";
	std::string t = digits(time, 6) ? time : "000000";
	return date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) + "T" + t.substr(0, 2) + ":" + t.substr(2, 2) + ":" + t.substr(4, 2);
}

// Every saved run's passes for the phone (GET /api/vehicles?scope=saved): what Studio's Results shows as "All saved
// runs", so a phone opened after Studio restarts still has the street's history.
//
// Flow, keeping file work off the GUI thread:
//   1. GUI thread (post): the data folder.
//   2. Here: resume::savedRuns() lists Stats/stats_*.csv newest first; each csv and its manifest (calsets::readRun) is
//      read, and each row is matched to this session's vehicle for the same csv and frame, if any (that one has a
//      snapshot, maybe a clip, and an id the rest of the API knows).
//   3. GUI thread (post): App::rowsFromStats() builds the rows exactly as Results does (directions from today's compass
//      labels, the user's calibration drives marked), then syncSets() + retimeRows() file each run into its
//      calibration set and re-time its speed to that set's calibration.  The session id goes in first, so the run being
//      written now (no set yet) wears the open set's stripe, as on the desktop.
//   4. Here: filtered by run/set/from/to, sorted newest first by time (undated passes last), cut at limit, as JSON.
//
// Returns false with `error` when the GUI thread was too busy to answer.
bool savedPasses(App& app, const std::string& onlyRun, int onlySet, const std::string& from, const std::string& to, size_t limit, json& out,
	std::string& error) {
	struct Run { resume::SavedRun saved; calsets::RunInfo info; std::vector<StatsRow> stats; std::vector<int> ids; };
	struct Job { std::string dataDir; std::vector<Run> runs; std::vector<ResultRow> rows; int limit = 0, egregious = 0; };
	auto job = std::make_shared<Job>();   // shared with posted tasks that may still run after post() gives up
	std::string ok = app.post([&app, job] { job->dataDir = app.dataDir(); return std::string("1"); });
	if (ok.empty()) { error = "VST Studio is busy; try again"; return false; }

	std::map<std::string, Vehicle> session;   // "<csv file name>#<frame>" -> this session's vehicle
	for (Vehicle& v : app.tracker.vehiclesSince(0))
		if (!v.statsPath.empty()) session[fs::path(v.statsPath).filename().string() + "#" + std::to_string(v.ev.frame)] = std::move(v);
	for (resume::SavedRun& sr : resume::savedRuns(job->dataDir)) {
		std::string name = fs::path(sr.csv).filename().string();
		if (!onlyRun.empty() && name != onlyRun) continue;
		Run r;
		std::string err;
		if (!readStatsCsv(sr.csv, r.stats, err)) continue;   // gone or unreadable since it was listed: Results skips it too
		r.info = calsets::readRun(sr.csv);
		for (const StatsRow& row : r.stats) {
			auto it = session.find(name + "#" + std::to_string(row.frame));
			r.ids.push_back(it == session.end() ? 0 : it->second.id);
		}
		r.saved = std::move(sr);
		job->runs.push_back(std::move(r));
	}

	ok = app.post([&app, job] {
		job->limit = app.prefs.speedLimit;
		job->egregious = app.prefs.egregious;
		for (const Run& r : job->runs) {
			size_t first = job->rows.size();
			app.rowsFromStats(r.stats, r.info, r.saved.csv, resume::runLabel(r.saved.name), job->rows);
			for (size_t i = 0; i < r.ids.size(); i++) job->rows[first + i].vehicleId = r.ids[i];
		}
		app.syncSets();
		app.retimeRows(job->rows);
		return std::string("1");
	});
	if (ok.empty()) { error = "VST Studio is busy; try again"; return false; }

	// Newest first by when each car passed, so runs that overlap or were written out of order (an old video tracked
	// today) still read as one timeline; passes with no recorded time go last, newest run first.
	struct Pick { size_t run, k, row; std::string time; };   // run index, its csv row, the row in job->rows
	std::vector<Pick> picks;
	for (size_t ri = 0, at = 0; ri < job->runs.size(); at += job->runs[ri].stats.size(), ri++) {
		const Run& r = job->runs[ri];
		for (size_t k = r.stats.size(); k-- > 0;) {
			const StatsRow& s = r.stats[k];
			if (onlySet && job->rows[at + k].setId != onlySet) continue;
			std::string time = !s.timestamp.empty() ? s.timestamp : isoFromDateTime(s.date, s.time);
			// Same rule as session vehicles: ISO local times compare correctly as strings.  A pass with no known time
			// can't be said to fall in a period, so a from/to filter leaves it out.
			if ((!from.empty() || !to.empty()) && time.empty()) continue;
			if (!from.empty() && time < from) continue;
			if (!to.empty() && time > to) continue;
			picks.push_back({ ri, k, at + k, time });
		}
	}
	std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
		if (a.time.empty() != b.time.empty()) return b.time.empty();
		return a.time > b.time;
	});
	if (picks.size() > limit) picks.resize(limit);

	out = json::array();
	for (const Pick& pk : picks) {
		const Run& r = job->runs[pk.run];
		const ResultRow& row = job->rows[pk.row];
		const StatsRow& s = r.stats[pk.k];
		std::string name = fs::path(r.saved.csv).filename().string();
		auto sv = row.vehicleId ? session.find(name + "#" + std::to_string(s.frame)) : session.end();
		const Vehicle* v = sv == session.end() ? nullptr : &sv->second;
		json trend = nullptr;
		if (!row.trend.empty())
			trend = { { "kind", row.trend }, { "entry", row.entryMph }, { "exit", row.exitMph }, { "accel", row.accel }, { "in20ft", row.mphIn20ft } };
		out.push_back({
			{ "id", v ? json(v->id) : json(nullptr) }, { "time", pk.time }, { "video", row.source }, { "frame", s.frame },
			{ "dir", row.l2r ? "L2R" : "R2L" }, { "compass", row.compass }, { "wrongWay", row.wrongWay },
			{ "speed", row.speed }, { "speeding", row.speed > job->limit }, { "egregious", row.speed >= job->egregious },
			{ "area", row.area }, { "large", row.large }, { "frames", row.frames },
			{ "startFrame", s.startFrame }, { "endFrame", s.endFrame }, { "fps", row.fps },
			{ "snapshot", v ? json("/api/vehicles/" + std::to_string(v->id) + "/snapshot.jpg") : json(nullptr) },
			{ "clip", v && !v->clipPath.empty() ? json("/api/vehicles/" + std::to_string(v->id) + "/clip.mp4") : json(nullptr) },
			{ "belowMin", false },
			{ "trend", trend },
			{ "run", name },
			{ "set", row.setId ? json(row.setId) : json(nullptr) },
			{ "measured", row.measured.speed },
			{ "adjusted", row.adjusted },
			{ "calibration", row.calKind == 1 ? json("drive") : row.calKind == 2 ? json("check") : json(nullptr) },
		});
	}
	return true;
}

// The calibration sets for the phone (GET /api/sets), oldest first: the same model as Results' stripes and set bar
// (CalSets.h).  `stripe` is the colour slot, chosen by set number like App::setColor (null for the set of runs with no
// calibration on record, drawn faint); `summary` is calsets::describe(), the desktop's tooltip.
json setsJson(App& app) {
	app.syncSets();
	const calsets::View now = app.currentView();
	const calsets::Scale nowFt = app.currentScale();
	auto r1 = [](double v) { return std::round(v * 10) / 10; };
	json out = json::array();
	for (const calsets::Set& s : app.sets().sets) {
		calsets::Scale shown = calsets::shownScale(s, now, nowFt);
		out.push_back({
			{ "id", s.id }, { "known", s.known }, { "state", s.locked() ? "locked" : "open" },
			{ "lockReason", s.locked() ? json(s.lockReason) : json(nullptr) }, { "lockedAt", s.lockedAt.empty() ? json(nullptr) : json(s.lockedAt) },
			{ "first", s.first }, { "last", s.last }, { "runs", s.runs.size() },
			{ "published", !s.published.empty() },
			{ "followsCalibration", s.known && !s.locked() && s.view.same(now) },
			{ "newerCalibration", calsets::newerCalibration(s, now, nowFt) },
			{ "zoneFt", s.known && shown.valid() ? json{ { "L2R", r1(shown.l2r) }, { "R2L", r1(shown.r2l) } } : json(nullptr) },
			{ "stripe", s.known ? json((s.id - 1) % 4) : json(nullptr) },
			{ "summary", calsets::describe(s, shown) },
		});
	}
	return out;
}

// Step 2's site planner for the phone (GET/PUT /api/plan).  It shows Pick your spot's draft, the same one the desktop
// page previews (App::planDraft_): the street as it stands and where it came from, the presets, the camera the plan
// assumes, the verdict in the desktop's words, what the calibration measured, and what saving would change (`dirty`,
// `changes`, `saved`).  Lengths in feet, rounded to 0.1.
json planJson(App& app) {
	app.syncPlanFromCalibration(false);
	app.planSync();
	auto r1 = [](double v) { return std::round(v * 10) / 10; };
	const PlanState& d = app.planDraft_.plan;
	const PlanState saved = app.planSaved();
	plan::Inputs in = App::planInputs(d);
	plan::Source src = plan::sourceOf(d.source);
	bool shortOn = app.cfg.g.subFrameTiming != 0 || app.planDraft_.shortView;
	plan::Verdict v = plan::verdict(in, shortOn);
	plan::Verdict sv = plan::verdict(App::planInputs(saved), app.cfg.g.subFrameTiming != 0);
	json presets = json::array();
	for (const plan::StreetPreset& sp : plan::kStreetPresets)
		presets.push_back({ { "key", sp.key }, { "name", sp.name }, { "hint", sp.hint }, { "distFt", sp.distFt }, { "clearFt", sp.clearFt } });
	plan::Measured m;
	std::string how;
	json measured = nullptr;
	if (app.planFromCalibration(m, how)) measured = { { "coverFt", r1(m.coverFt) }, { "clearFt", r1(m.clearFt) }, { "how", how } };
	bool finite = std::isfinite(v.at.total);
	json map = nullptr;
	if (d.camLat != 0 || d.camLon != 0) {
		plan::MapPick mp;
		mp.camLat = d.camLat; mp.camLon = d.camLon; mp.laneLat = d.laneLat; mp.laneLon = d.laneLon;
		plan::MapResult mr = plan::fromMap(mp);
		map = { { "camLat", d.camLat }, { "camLon", d.camLon }, { "laneLat", d.laneLat }, { "laneLon", d.laneLon }, { "street", d.street },
			{ "distFt", r1(mr.distM * plan::kFt) }, { "facing", plan::compass8(mr.facingDeg) } };
	}
	const Globals& g = app.cfg.g;
	const App::PlanDraft& pd = app.planDraft_;
	return json{
		{ "street", { { "distFt", r1(d.dist * plan::kFt) }, { "clearFt", r1(d.clear * plan::kFt) },
			{ "coverFt", r1(plan::coverAt(d.dist, d.hfov) * plan::kFt) }, { "source", plan::sourceKey(src) },
			{ "sourceName", plan::sourceName(src) }, { "note", d.note }, { "speedLimit", d.speedLimit } } },
		{ "presets", presets },
		{ "camera", { { "hfov", d.hfov }, { "width", d.width }, { "fps", d.fps }, { "shutter", d.shutter } } },
		{ "verdict", { { "grade", v.grade }, { "label", plan::gradeName(v.grade) }, { "text", plan::verdictText(v) },
			{ "checkMph", v.checkMph }, { "errorMph", finite ? json(r1(v.at.total)) : json(nullptr) }, { "zoneFt", r1(v.at.zone * plan::kFt) },
			{ "shortView", v.shortView }, { "fastest", v.fastestMph > 0 ? json(plan::fastestText(v.fastestMph)) : json(nullptr) },
			{ "fastest60", v.fastest60 > 0 ? json(plan::fastestText(v.fastest60)) : json(nullptr) },
			{ "frameRate", d.fps < 24 ? json(app.frameRateAdvice(d.fps)) : json(nullptr) } } },
		{ "measured", measured },
		{ "carLengths", d.carLengths > 0 ? json(r1(d.carLengths)) : json(nullptr) },
		{ "picture", app.planPictureAvailable() },
		{ "typicalCarFt", plan::kTypicalCarFt },
		{ "map", map },
		// One-way and compass as they will be once saved (the map can set them).
		{ "legalDirection", lowerDir(pd.legal.empty() ? g.LegalDirection : pd.legal) },
		{ "L2RDirection", pd.l2r.empty() ? g.L2RDirection : pd.l2r }, { "R2LDirection", pd.r2l.empty() ? g.R2LDirection : pd.r2l },
		{ "shortViewOnSave", pd.shortView },
		{ "dirty", app.planDirty() },
		{ "changes", app.planChanges() },
		{ "autosave", app.prefs.planAutosave },
		{ "saved", { { "distFt", r1(saved.dist * plan::kFt) }, { "clearFt", r1(saved.clear * plan::kFt) }, { "speedLimit", saved.speedLimit },
			{ "source", plan::sourceKey(plan::sourceOf(saved.source)) }, { "grade", sv.grade }, { "label", plan::gradeName(sv.grade) },
			{ "text", plan::verdictText(sv) } } },
	};
}

// One change at a time, the way the desktop makes them, into Pick your spot's draft (a preview until saved, unless
// autosave is on): a preset ({"preset": "narrow"}), typed numbers ({"distFt", "clearFt"}, either or both), the speed
// limit ({"speedLimit": 20}), car lengths across the picture ({"carLengths": 6}), back to what the calibration measured
// ({"useMeasured": true}), short-view timing on save ({"shortView": true|false}), or the map's two pins and what
// OpenStreetMap says about the street there ({"map": {...}}).  Then {"save": true} saves the draft and
// {"discard": true} drops it; {"autosave": true|false} switches autosave.  Returns the problems in words; empty when
// it was applied.
std::vector<std::string> applyPlan(App& app, const json& in) {
	app.planSync();
	PlanState& d = app.planDraft_.plan;
	auto num = [&](const char* k, double& out) {
		if (!in.contains(k)) return false;
		if (!in[k].is_number()) throw std::string(k) + " must be a number";
		out = in[k].get<double>();
		return true;
	};
	try {
		double dd = 0, c = 0, n = 0, lim = 0;
		if (in.contains("save")) { if (in["save"] == true) app.planCommit(); }
		else if (in.contains("discard")) { if (in["discard"] == true) app.planDiscard(); }
		else if (in.contains("autosave")) {
			if (!in["autosave"].is_boolean()) return { "autosave must be true or false" };
			app.prefs.planAutosave = in["autosave"].get<bool>();
			if (app.prefs.planAutosave && app.planDirty()) app.planCommit(true);
		}
		else if (in.contains("preset")) {
			const plan::StreetPreset* sp = in["preset"].is_string() ? plan::findStreetPreset(in["preset"].get<std::string>()) : nullptr;
			if (!sp) return { "unknown preset" };
			app.setDraftStreet(sp->distFt / plan::kFt, sp->clearFt / plan::kFt, plan::Source::Guess, sp->name);
		}
		else if (num("carLengths", n)) {
			if (!(n >= 1 && n <= 20)) return { "carLengths must be between 1 and 20" };
			if (!app.planPictureAvailable()) return { "open a picture of your own street first (step 3): the sample video isn't your street" };
			const Globals& g = app.cfg.g;
			bool boxed = app.zonesDone();
			plan::Measured m = plan::fromCarLengths(n, boxed ? g.AnalysisBoxWidth : 0, boxed ? std::max(0, g.obstruction[1] - g.obstruction[0]) : 0);
			plan::Inputs pi = App::planInputs(d);
			plan::applyMeasured(pi, m);
			char note[96];
			std::snprintf(note, sizeof note, "%.1f car lengths across the picture", n);
			d.carLengths = n;
			app.setDraftStreet(pi.distM, pi.clearM, plan::Source::Picture, note);
		}
		else if (in.value("useMeasured", false)) {
			plan::Measured m;
			std::string how;
			if (!app.planFromCalibration(m, how)) return { "there is no calibration for this camera yet (step 5)" };
			plan::Inputs pi = App::planInputs(d);
			plan::applyMeasured(pi, m);
			app.setDraftStreet(pi.distM, pi.clearM, plan::Source::Calibration, "Measured by " + how);
		}
		else if (in.contains("shortView")) {
			if (!in["shortView"].is_boolean()) return { "shortView must be true or false" };
			app.planDraft_.shortView = in["shortView"].get<bool>();
			app.planEdited();
		}
		else if (in.contains("map")) {
			const json& mj = in["map"];
			if (!mj.is_object()) return { "map must be an object" };
			plan::MapPick mp;
			auto get = [&](const char* k, double& out) {
				if (!mj.contains(k)) return;
				if (!mj[k].is_number()) throw std::string("map.") + k + " must be a number";
				out = mj[k].get<double>();
			};
			get("camLat", mp.camLat); get("camLon", mp.camLon); get("laneLat", mp.laneLat); get("laneLon", mp.laneLon);
			if (mj.contains("wayBearing") && mj["wayBearing"].is_number()) { mp.haveWay = true; mp.wayBearing = mj["wayBearing"].get<double>(); }
			if (mj.contains("oneway") && mj["oneway"].is_number()) mp.oneway = std::clamp(mj["oneway"].get<int>(), -1, 1);
			if (mj.contains("maxspeedMph") && mj["maxspeedMph"].is_number()) mp.maxspeedMph = mj["maxspeedMph"].get<int>();
			if (mj.contains("street") && mj["street"].is_string()) mp.street = mj["street"].get<std::string>().substr(0, 80);
			for (double x : { mp.camLat, mp.laneLat }) if (!(x >= -90 && x <= 90)) return { "latitudes must be between -90 and 90" };
			for (double x : { mp.camLon, mp.laneLon }) if (!(x >= -180 && x <= 180)) return { "longitudes must be between -180 and 180" };
			std::string why;
			if (!app.applyMapToDraft(mp, why)) return { why };
		}
		else {
			bool hasD = num("distFt", dd), hasC = num("clearFt", c), hasL = num("speedLimit", lim);
			if (!hasD && !hasC && !hasL) return { "send preset, distFt/clearFt, speedLimit, carLengths, useMeasured, shortView, map, save, discard or autosave" };
			if (hasD && !(dd >= 3 && dd <= 1000)) return { "distFt must be between 3 and 1000" };
			if (hasC && !(c >= 3 && c <= 1000)) return { "clearFt must be between 3 and 1000" };
			if (hasL && !(lim >= 5 && lim <= 80)) return { "speedLimit must be between 5 and 80" };
			if (hasD || hasC) app.setDraftStreet(hasD ? dd / plan::kFt : d.dist, hasC ? c / plan::kFt : d.clear, plan::Source::Typed, "");
			if (hasL) { d.speedLimit = int(std::lround(lim)); app.planEdited(); }
		}
	}
	catch (const std::string& e) { return { e }; }
	return {};
}

// The settings the companion can read and change: VST.cfg keys by their own names, plus the app's run settings.
json configJson(App& app) {
	const Globals& g = app.cfg.g;
	json r{
		{ "AnalysisBoxLeft", g.AnalysisBoxLeft }, { "AnalysisBoxTop", g.AnalysisBoxTop },
		{ "AnalysisBoxWidth", g.AnalysisBoxWidth }, { "AnalysisBoxHeight", g.AnalysisBoxHeight },
		{ "speedLineLeft", g.speedLineLeft }, { "speedLineRight", g.speedLineRight },
		{ "obstruction", { g.obstruction[0], g.obstruction[1] } }, { "obstruction_extent", g.obstruction_extent },
		{ "R2LStreetY", g.R2LStreetY }, { "L2RStreetY", g.L2RStreetY },
		{ "R2LStreetYRight", g.R2LStreetYRight }, { "L2RStreetYRight", g.L2RStreetYRight },
		{ "streetEdge", { g.streetEdge[0], g.streetEdge[1] } },
		{ "L2RDirection", g.L2RDirection }, { "R2LDirection", g.R2LDirection },
		{ "legalDirection", lowerDir(g.LegalDirection) },
		{ "CalibrationFramesL2R", g.CalibrationFramesL2R }, { "CalibrationFramesR2L", g.CalibrationFramesR2L },
		{ "CalibrationSpeed", g.CalibrationSpeed }, { "CalibrationFPS", g.CalibrationFPS },
		{ "SENSITIVITY_VALUE", g.SENSITIVITY_VALUE }, { "BLUR_SIZE", g.BLUR_SIZE }, { "largeVehicleArea", g.largeVehicleArea },
		{ "minLoggedSpeed", g.minLoggedSpeed }, { "subFrameTiming", g.subFrameTiming }, { "levelDegrees", g.levelDegrees },
		{ "maxL2RDistOnEntry", g.maxL2RDistOnEntry }, { "maxR2LDistOnEntry", g.maxR2LDistOnEntry }, { "entryLookBack", g.entryLookBack },
		{ "speedLimit", app.prefs.speedLimit }, { "egregious", app.prefs.egregious }, { "siteName", app.prefs.siteName },
		{ "frameWidth", 1280 }, { "frameHeight", 720 },
	};
	// The parking lane (ParkingLane.h), Studio's own setting: rows at frame x 0 and 1280, the measured distance (0 =
	// from the picture), and read-only what Studio makes of it in the middle of the picture.
	r["parkingLane"] = { int(std::lround(app.prefs.parkY0)), int(std::lround(app.prefs.parkY1)) };
	r["parkingLaneFt"] = app.prefs.parkFt;
	calib::ParkingLane park = app.parkingLane();
	r["parkingLaneBeyondFt"] = nullptr;
	r["parkingLaneHow"] = nullptr;
	if (park.set()) {
		const bool oneWay = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
		calib::CarSpot sp;
		calib::parkedBeyond(app.cameraSetup(), 640, app.laneRowAt(oneWay ? g.LegalDirection == "L2R" : true, 640), park, sp);
		if (std::isfinite(sp.parkedFt)) {
			r["parkingLaneBeyondFt"] = std::round(sp.parkedFt * 10) / 10;
			r["parkingLaneHow"] = sp.fromPicture ? "picture" : sp.typical ? "typical" : "measured";
		}
	}
	return r;
}

// Per-direction calibration standing, from calibration-history.csv (App::calibrationState).
json calibrationJson(App& app) {
	auto one = [&app](bool l2r) {
		CalState s = app.calibrationState(l2r);
		bool checked = s.kind == CalState::Verified || s.kind == CalState::Failed;
		return json{ { "state", s.apiName() }, { "error", checked ? json(s.error) : json(nullptr) },
			{ "applied", s.applied.empty() ? json(nullptr) : json(s.applied) }, { "checked", checked ? json(s.checked) : json(nullptr) },
			{ "frames", l2r ? app.cfg.g.CalibrationFramesL2R : app.cfg.g.CalibrationFramesR2L } };
	};
	// What the inputs add up to per direction (CalEvidence.h), with the novice summary and each counted input's share,
	// and the inputs themselves, so the phone can list them and take one out (POST /api/calibration/inputs).
	auto combined = [&app](bool l2r) {
		calib::Combined c = app.calibrationCombined(l2r);
		std::string detail, head = calib::summary(c, &detail);
		json shares = json::array();
		for (const calib::Share& s : c.shares)
			shares.push_back({ { "id", s.id }, { "contribution", std::round(s.contribution * 1000) / 1000 },
				{ "offPct", std::round(s.offPct * 10) / 10 }, { "outlier", s.outlier } });
		return json{ { "summary", head }, { "detail", detail }, { "n", c.n }, { "agree", c.agree() },
			{ "zoneFt", c.valid ? json(std::round(c.zoneFt * 100) / 100) : json(nullptr) },
			{ "sigmaPct", c.valid ? json(std::round(c.sigma * 1000) / 10) : json(nullptr) }, { "setAside", c.setAside }, { "shares", shares } };
	};
	json inputs = json::array();
	std::string lines = app.speedLinesSig();
	for (const calib::Input& in : app.calibrationInputs())
		inputs.push_back({ { "id", in.id }, { "method", in.method }, { "methodName", calib::methodName(in.method) }, { "label", in.label },
			{ "time", in.time }, { "dirs", in.dirs == calib::kL2R ? "L2R" : in.dirs == calib::kR2L ? "R2L" : "both" },
			{ "zoneFt", std::round(in.zoneFt * 100) / 100 }, { "sigmaPct", std::round(in.sigma * 1000) / 10 }, { "removed", in.removed },
			{ "otherLines", !in.lines.empty() && in.lines != lines } });
	json one2r = one(true), r2l = one(false);
	one2r["combined"] = combined(true);
	r2l["combined"] = combined(false);
	return json{ { "L2R", one2r }, { "R2L", r2l }, { "inputs", inputs } };
}

json statusJson(App& app) {
	auto prog = app.tracker.progress();
	Counts c = app.tracker.counts();
	const SourceInfo& in = app.player.info();
	SourceInfo::Kind k = classifySource(app.source);
	json src = app.source.empty() ? json(nullptr) : json{
		{ "kind", k == SourceInfo::Camera ? "camera" : k == SourceInfo::Stream ? "stream" : k == SourceInfo::Image ? "image" : "file" },
		{ "name", friendlySourceName(app.source) }, { "fps", in.fps }, { "width", in.width }, { "height", in.height } };
	return json{
		{ "version", VST_VERSION }, { "siteName", app.prefs.siteName },
		{ "running", app.tracker.running() }, { "paused", app.tracker.paused() },
		{ "reconnecting", app.tracker.running() && prog.reconnecting },
		{ "source", src }, { "frame", app.tracker.running() ? prog.frame : std::max(app.player.position(), 0) },
		{ "totalFrames", app.tracker.running() ? prog.totalFrames : in.frames }, { "procFps", prog.procFps },
		{ "batch", { { "index", prog.job }, { "count", prog.jobs } } },
		{ "counts", { { "vehicles", c.vehicles }, { "speeders", c.speeders }, { "egregious", c.egregious },
			{ "wrongWay", c.wrongWay }, { "topSpeed", c.topSpeed }, { "avgSpeed", c.avgSpeed },
			{ "tries", c.tries }, { "slow", c.slow }, { "untimed", c.untimed } } },
		{ "speedLimit", app.prefs.speedLimit }, { "egregious", app.prefs.egregious },
		{ "legalDirection", lowerDir(app.cfg.g.LegalDirection) },
		{ "calibrationMarks", { { "L2R", { app.calibMarks[0][0], app.calibMarks[0][1] } }, { "R2L", { app.calibMarks[1][0], app.calibMarks[1][1] } } } },
		{ "calibration", calibrationJson(app) },
		{ "liveViews", app.server ? app.server->openStreams() : 0 },
		{ "health", { { "camera", app.health.viewCheckedAt.empty() ? "unchecked" : app.health.view.name() },
			{ "cameraWarning", app.health.cameraWarning }, { "lowDisk", app.health.lowDisk } } },
	};
}

// GET /api/health: today's numbers (a health.csv row in the making), the disk budget and the latest camera check.
json healthJson(App& app) {
	const App::HealthState& h = app.health;
	const health::Day& d = h.today;
	auto orNull = [](std::uintmax_t v) { return v ? json(v) : json(nullptr); };
	return json{
		{ "today", { { "date", d.date }, { "trackedHours", d.trackedHours }, { "outageHours", d.outageHours }, { "outages", d.outages },
			{ "vehicles", d.vehicles }, { "clipsRemoved", d.clipsRemoved }, { "camera", d.camera } } },
		{ "disk", { { "videoBytes", h.video.bytes }, { "videoFiles", h.video.files }, { "budgetBytes", orNull(h.budgetBytes) },
			{ "freeBytes", orNull(h.freeBytes) }, { "lowDisk", h.lowDisk } } },
		{ "camera", { { "result", h.viewCheckedAt.empty() ? "unchecked" : h.view.name() }, { "warning", h.cameraWarning },
			{ "dx", h.view.dx }, { "dy", h.view.dy }, { "confidence", h.view.confidence },
			{ "checked", h.viewCheckedAt.empty() ? json(nullptr) : json(h.viewCheckedAt) } } },
	};
}

// Applies a partial config object; returns the problems (empty = applied).  Runs on the GUI thread.
std::vector<std::string> applyConfig(App& app, const json& in) {
	Globals g(app.cfg.g);
	std::vector<std::string> problems;
	auto setInt = [&](const char* k, int& f) { if (in.contains(k)) { if (in[k].is_number()) f = in[k].get<int>(); else problems.push_back(std::string(k) + " must be a number"); } };
	auto setDbl = [&](const char* k, double& f) { if (in.contains(k)) { if (in[k].is_number()) f = in[k].get<double>(); else problems.push_back(std::string(k) + " must be a number"); } };
	auto setStr = [&](const char* k, std::string& f) { if (in.contains(k)) { if (in[k].is_string()) f = in[k].get<std::string>(); else problems.push_back(std::string(k) + " must be a string"); } };
	setInt("AnalysisBoxLeft", g.AnalysisBoxLeft); setInt("AnalysisBoxTop", g.AnalysisBoxTop);
	setInt("AnalysisBoxWidth", g.AnalysisBoxWidth); setInt("AnalysisBoxHeight", g.AnalysisBoxHeight);
	setInt("speedLineLeft", g.speedLineLeft); setInt("speedLineRight", g.speedLineRight);
	setInt("obstruction_extent", g.obstruction_extent); setInt("R2LStreetY", g.R2LStreetY); setInt("L2RStreetY", g.L2RStreetY);
	setInt("R2LStreetYRight", g.R2LStreetYRight); setInt("L2RStreetYRight", g.L2RStreetYRight);
	setInt("CalibrationFramesL2R", g.CalibrationFramesL2R); setInt("CalibrationFramesR2L", g.CalibrationFramesR2L);
	setDbl("CalibrationSpeed", g.CalibrationSpeed); setDbl("CalibrationFPS", g.CalibrationFPS);
	setInt("SENSITIVITY_VALUE", g.SENSITIVITY_VALUE); setInt("BLUR_SIZE", g.BLUR_SIZE); setInt("largeVehicleArea", g.largeVehicleArea);
	setInt("minLoggedSpeed", g.minLoggedSpeed); setInt("subFrameTiming", g.subFrameTiming); setDbl("levelDegrees", g.levelDegrees);
	setInt("maxL2RDistOnEntry", g.maxL2RDistOnEntry); setInt("maxR2LDistOnEntry", g.maxR2LDistOnEntry); setInt("entryLookBack", g.entryLookBack);
	setStr("L2RDirection", g.L2RDirection); setStr("R2LDirection", g.R2LDirection);
	auto setPair = [&](const char* k, int (&f)[2]) {
		if (!in.contains(k)) return;
		if (in[k].is_array() && in[k].size() == 2 && in[k][0].is_number() && in[k][1].is_number()) {
			f[0] = in[k][0].get<int>();
			f[1] = in[k][1].get<int>();
		}
		else problems.push_back(std::string(k) + " must be [left, right]");
	};
	setPair("obstruction", g.obstruction);
	setPair("streetEdge", g.streetEdge);
	if (in.contains("legalDirection")) {
		std::string d = in["legalDirection"].is_string() ? in["legalDirection"].get<std::string>() : "";
		g.LegalDirection = d == "both" ? "Both" : d;
	}
	int limit = app.prefs.speedLimit, egregious = app.prefs.egregious;
	std::string site = app.prefs.siteName;
	// The parking lane: [0, 0] (none) or two rows on the picture; parkingLaneFt 0 (from the picture) to 40.
	int park[2] = { int(std::lround(app.prefs.parkY0)), int(std::lround(app.prefs.parkY1)) };
	double parkFt = app.prefs.parkFt;
	setPair("parkingLane", park);
	setDbl("parkingLaneFt", parkFt);
	if ((park[0] || park[1]) && (park[0] < 1 || park[1] < 1 || park[0] > 719 || park[1] > 719))
		problems.push_back("parkingLane must be [0,0] (none) or two rows between 1 and 719, where the line on the parked cars' wheels meets the picture's left and right edges");
	if (!(parkFt >= 0 && parkFt <= 40)) problems.push_back("parkingLaneFt must be between 0 (work it out from the picture) and 40");
	setInt("speedLimit", limit);
	setInt("egregious", egregious);
	setStr("siteName", site);
	if (limit < 5 || limit > 80) problems.push_back("speedLimit must be between 5 and 80");
	for (auto& p : g.problems()) problems.push_back(p);
	if (!problems.empty()) return problems;
	// A run in progress keeps the settings it started with; changes apply from the next run.
	app.cfg.g.~Globals();
	new (&app.cfg.g) Globals(g);
	int limitBefore = app.prefs.speedLimit;
	app.prefs.speedLimit = limit;
	app.prefs.egregious = std::max(egregious, limit);
	if (!in.contains("minLoggedSpeed")) app.followSpeedLimit(limitBefore);   // an explicit minLoggedSpeed wins
	app.prefs.siteName = site;
	app.prefs.parkY0 = park[0];
	app.prefs.parkY1 = park[1];
	app.prefs.parkFt = parkFt;
	app.prefs.save();
	if (app.cfg.dirty() && !app.cfg.save()) problems.push_back(app.cfg.error);
	return problems;
}

// Draws the zones and live boxes onto a frame for remote aiming.
void drawOverlay(cv::Mat& f, const Globals& g, const cv::Rect& ab, const std::vector<vst::TrackBox>& boxes) {
	cv::Rect box(g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight);
	cv::rectangle(f, box, cv::Scalar(238, 211, 34), 2);
	for (int x : { g.speedLineLeft, g.speedLineRight })
		cv::line(f, { box.x + x, box.y }, { box.x + x, box.y + box.height }, cv::Scalar(255, 255, 255), 2);
	if (g.obstruction[0] < g.obstruction[1])
		for (int x : { g.obstruction[0], g.obstruction[1] })
			cv::line(f, { box.x + x, box.y }, { box.x + x, box.y + box.height }, cv::Scalar(21, 204, 250), 2);
	// Lane lines at their angle (Perspective.h), and the street edge thin, when it's set.
	for (bool l2r : { false, true }) {
		perspective::Line l = g.lane(l2r);
		cv::line(f, { box.x, box.y + int(std::lround(l.yLeft)) }, { box.x + box.width, box.y + int(std::lround(l.yRight)) },
			l2r ? cv::Scalar(255, 108, 176) : cv::Scalar(61, 138, 255), 2);
	}
	if (g.hasStreetEdge())
		cv::line(f, { box.x, box.y + g.streetEdge[0] }, { box.x + box.width, box.y + g.streetEdge[1] }, cv::Scalar(238, 211, 34), 1);
	for (const auto& t : boxes) {
		cv::Rect r = t.box + ab.tl();
		cv::Scalar col = g.isWrongWay(t.leftToRight) ? cv::Scalar(94, 63, 244) : cv::Scalar(94, 197, 34);
		cv::rectangle(f, r, col, 2);
		if (t.speed > 0) cv::putText(f, std::to_string(t.speed) + " mph", { r.x, std::max(20, r.y - 8) }, cv::FONT_HERSHEY_SIMPLEX, .8, col, 2);
	}
}

}  // namespace

Server::Server(App& app) : app_(app) {}

Server::~Server() {
	if (http_) http_->stop();
	if (thread_.joinable()) thread_.join();
}

std::string Server::baseUrl() const { return "http://" + ip_ + ":" + std::to_string(port_) + "/"; }
std::string Server::pairingUrl() const { return baseUrl() + "#token=" + app_.prefs.remoteToken; }

void Server::setToken(const std::string& token) {
	std::lock_guard<std::mutex> lock(tokenMu_);
	token_ = token;
}

// Compared without early exit, so the response time doesn't say how much of a guess was right.
bool Server::tokenMatches(const std::string& t) const {
	std::lock_guard<std::mutex> lock(tokenMu_);
	if (t.size() != token_.size() || token_.empty()) return false;
	unsigned diff = 0;
	for (size_t i = 0; i < t.size(); i++) diff |= unsigned(t[i] ^ token_[i]);
	return diff == 0;
}

// A picked address that's gone (the laptop left that network) falls back to the automatic guess rather than
// putting a dead address in the QR code.
void Server::setAddress(const std::string& ip) {
	auto all = lanAddresses();
	ip_ = !ip.empty() && std::find(all.begin(), all.end(), ip) != all.end() ? ip : lanAddress();
}

bool Server::start(int port, bool localOnly) {
	port_ = port;
	setAddress(app_.prefs.remoteAddress);
	setToken(app_.prefs.remoteToken);
	http_ = std::make_unique<httplib::Server>();
	routes();
	if (!http_->bind_to_port(localOnly ? "127.0.0.1" : "0.0.0.0", port)) {
		error_ = "the port is in use or not allowed";
		http_.reset();
		return false;
	}
	running_ = true;
	thread_ = std::thread([this] { http_->listen_after_bind(); running_ = false; });
	return true;
}

void Server::routes() {
	using httplib::Request;
	using httplib::Response;
	App& app = app_;

	auto sendJson = [](Response& res, const json& j, int status = 200) {
		res.status = status;
		res.set_content(j.dump(), "application/json");
	};
	// Sends a JSON body produced by App::post(), which returns "" when the GUI thread didn't get to it within 5 s.
	auto sendPosted = [sendJson](Response& res, const std::string& out) {
		json j = json::parse(out, nullptr, false);
		if (j.is_discarded()) sendJson(res, { { "error", "VST Studio is busy; try again" } }, 503);
		else sendJson(res, j);
	};
	// The token as presented, from the header or the query string ("" if neither).
	auto presented = [](const Request& req) {
		std::string h = req.get_header_value("Authorization");
		if (h.rfind("Bearer ", 0) == 0) return h.substr(7);
		return req.has_param("token") ? req.get_param_value("token") : std::string();
	};
	auto authed = [this, presented](const Request& req) { return tokenMatches(presented(req)); };
	auto guard = [authed, sendJson](auto handler) {
		return [=](const Request& req, Response& res) {
			if (!authed(req)) { sendJson(res, { { "error", "pairing token missing or wrong; scan the QR code in VST Studio again" } }, 401); return; }
			handler(req, res);
		};
	};
	// Takes one of the kMaxStreams stream slots, or answers 503 and returns null.  The slot is held by the stream's
	// content provider, which keeps the returned pointer: it's given back when the last copy of the provider is
	// destroyed, however the stream ends (client gone, provider returned false, server stopping).  Not tied to
	// cpp-httplib's resource releaser: Response is copyable and every copy runs the releaser when destroyed, so it
	// can run more than once for one stream (counting that way went negative in the smoke test).
	struct StreamSlot {
		std::atomic<int>& n;
		explicit StreamSlot(std::atomic<int>& n) : n(n) {}
		StreamSlot(const StreamSlot&) = delete;
		~StreamSlot() { --n; }
	};
	auto takeStream = [this, sendJson](Response& res) -> std::shared_ptr<StreamSlot> {
		if (++streams_ > kMaxStreams) {
			--streams_;
			sendJson(res, { { "error", "too many live views are open; close one on another phone or tab and try again" } }, 503);
			return nullptr;
		}
		return std::make_shared<StreamSlot>(streams_);
	};
	auto parseBody = [](const Request& req, json& out) {
		out = json::parse(req.body, nullptr, false);
		return !out.is_discarded() && out.is_object();
	};

	http_->set_default_headers({ { "Access-Control-Allow-Origin", "*" }, { "Access-Control-Allow-Headers", "Authorization, Content-Type" },
		{ "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS" }, { "Cache-Control", "no-store" } });
	http_->Options(R"(/api/.*)", [](const Request&, Response& res) { res.status = 204; });
	// Every error is JSON {"error": "..."} (docs/companion-api.md).  httplib calls this for any response with a
	// status of 400 or more, including ones a handler already filled in (401, 404 for a vehicle...), so only empty
	// bodies are replaced: unmatched routes, and paths the static mount point refuses (such as "/../").
	http_->set_error_handler([](const Request&, Response& res) {
		if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
		const char* msg = res.status == 404 ? "not found" : res.status == 400 ? "bad request" : "request failed";
		res.set_content(json{ { "error", msg } }.dump(), "application/json");
		return httplib::Server::HandlerResponse::Handled;
	});

	http_->Get("/api/hello", [sendJson](const Request&, Response& res) {
		sendJson(res, { { "app", "VST" }, { "version", VST_VERSION }, { "auth", true } });
	});

	http_->Get("/api/status", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return statusJson(app).dump(); }));
	}));

	http_->Get("/api/vehicles", guard([&app, sendJson](const Request& req, Response& res) {
		int since = req.has_param("since") ? std::atoi(req.get_param_value("since").c_str()) : 0;
		size_t limit = req.has_param("limit") ? std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 5000) : 100;
		std::string from = req.has_param("from") ? req.get_param_value("from") : "", to = req.has_param("to") ? req.get_param_value("to") : "";
		// passes=1 adds the slow passes (timed under minLoggedSpeed, `belowMin`), for picking a calibration pass.
		bool passes = req.has_param("passes") && req.get_param_value("passes") == "1";
		// scope=saved: every saved run's passes, as Results' "All saved runs" (savedPasses above).  `since` and
		// `passes` are about this session's ids and slow passes, so they don't apply there.
		if (req.has_param("scope") && req.get_param_value("scope") == "saved") {
			json out;
			std::string err;
			std::string run = req.has_param("run") ? req.get_param_value("run") : "";
			size_t savedLimit = req.has_param("limit") ? std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 20000) : 100;
			int set = req.has_param("set") ? std::atoi(req.get_param_value("set").c_str()) : 0;
			if (!savedPasses(app, run, set, from, to, savedLimit, out, err)) { sendJson(res, { { "error", err } }, 503); return; }
			sendJson(res, out);
			return;
		}
		if (req.has_param("scope") && req.get_param_value("scope") != "session") { sendJson(res, { { "error", "scope must be session or saved" } }, 400); return; }
		auto vs = passes ? app.tracker.passesSince(since) : app.tracker.vehiclesSince(since);
		json out = json::array();
		// Newest first; ISO local timestamps compare correctly as strings.
		for (auto it = vs.rbegin(); it != vs.rend() && out.size() < limit; ++it) {
			if (!from.empty() && it->time < from) continue;
			if (!to.empty() && it->time > to) continue;
			out.push_back(vehicleJson(*it));
		}
		sendJson(res, out);
	}));

	http_->Get("/api/sets", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return setsJson(app).dump(); }));
	}));

	http_->Get(R"(/api/vehicles/(\d+))", guard([&app, sendJson](const Request& req, Response& res) {
		Vehicle v;
		if (!app.tracker.vehicle(std::atoi(req.matches[1].str().c_str()), v)) { sendJson(res, { { "error", "no such vehicle" } }, 404); return; }
		sendJson(res, vehicleJson(v));
	}));

	http_->Get(R"(/api/vehicles/(\d+)/snapshot\.jpg)", guard([&app, sendJson](const Request& req, Response& res) {
		Vehicle v;
		std::vector<uchar> jpg;
		if (!app.tracker.vehicle(std::atoi(req.matches[1].str().c_str()), v) || v.snapshot.empty() || !cv::imencode(".jpg", v.snapshot, jpg)) {
			sendJson(res, { { "error", "no snapshot for this vehicle" } }, 404);
			return;
		}
		res.set_content(std::string(jpg.begin(), jpg.end()), "image/jpeg");
	}));

	http_->Get(R"(/api/vehicles/(\d+)/clip\.mp4)", guard([&app, sendJson](const Request& req, Response& res) {
		Vehicle v;
		if (!app.tracker.vehicle(std::atoi(req.matches[1].str().c_str()), v) || v.clipPath.empty()) { sendJson(res, { { "error", "no clip kept for this vehicle" } }, 404); return; }
		std::ifstream in(v.clipPath, std::ios::binary);
		if (!in) { sendJson(res, { { "error", "the clip file is gone" } }, 404); return; }
		std::ostringstream buf;
		buf << in.rdbuf();
		res.set_content(buf.str(), "video/mp4");
	}));

	// Current picture: the tracker's latest frame while running, else the preview player's frame.
	auto grab = [&app](bool overlay) {
		Tracker::Live live;
		auto shared = std::make_shared<std::pair<cv::Mat, std::unique_ptr<Globals>>>();
		if (app.tracker.running() && app.tracker.latest(live, false)) shared->first = live.frame;
		// Shared state rather than references: post() gives up after 5 s and the lambda may still run later.
		std::string ok = app.post([&app, shared] {
			if (shared->first.empty() && app.player.isOpen()) shared->first = app.player.frame().clone();
			shared->second = std::make_unique<Globals>(app.cfg.g);
			return std::string("1");
		});
		cv::Mat f = shared->first.clone();
		if (!f.empty() && overlay && !ok.empty()) drawOverlay(f, *shared->second, live.analysisBox, live.boxes);
		return f;
	};

	http_->Get("/api/frame.jpg", guard([grab, sendJson](const Request& req, Response& res) {
		cv::Mat f = grab(req.has_param("overlay") && req.get_param_value("overlay") != "0");
		std::vector<uchar> jpg;
		if (f.empty() || !cv::imencode(".jpg", f, jpg, { cv::IMWRITE_JPEG_QUALITY, 80 })) { sendJson(res, { { "error", "no picture yet; open a source in VST Studio" } }, 404); return; }
		res.set_content(std::string(jpg.begin(), jpg.end()), "image/jpeg");
	}));

	http_->Get("/api/stream.mjpg", guard([this, grab, presented, takeStream](const Request& req, Response& res) {
		auto slot = takeStream(res);
		if (!slot) return;
		bool overlay = req.has_param("overlay") && req.get_param_value("overlay") != "0";
		std::string token = presented(req);
		res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=vstframe", [this, grab, overlay, token, slot](size_t, httplib::DataSink& sink) {
			if (!tokenMatches(token)) return false;   // the pairing code was reset: this phone is signed out
			cv::Mat f = grab(overlay);
			std::vector<uchar> jpg;
			if (!f.empty()) {
				cv::resize(f, f, cv::Size(960, 540), 0, 0, cv::INTER_AREA);
				cv::imencode(".jpg", f, jpg, { cv::IMWRITE_JPEG_QUALITY, 70 });
				std::string head = "--vstframe\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpg.size()) + "\r\n\r\n";
				if (!sink.write(head.data(), head.size()) || !sink.write((const char*)jpg.data(), jpg.size()) || !sink.write("\r\n", 2)) return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(125));
			return sink.is_writable();
		});
	}));

	// Server-sent events: status about once a second, each vehicle as it's logged, live boxes ~5 times a second,
	// and the config when the stream opens and again whenever it changes.
	http_->Get("/api/live", guard([this, &app, presented, takeStream](const Request& req, Response& res) {
		auto slot = takeStream(res);
		if (!slot) return;
		struct LiveState {
			int lastVehicle = 0;    // newest vehicle id already sent
			int lastPass = 0;       // newest slow pass id already sent (the `pass` event)
			int tick = 0;           // provider calls so far; every 5th (~1 s) sends status and checks the config
			std::string config;     // the config JSON last sent, "" before the first
		};
		auto state = std::make_shared<LiveState>();
		state->lastVehicle = req.has_param("since") ? std::atoi(req.get_param_value("since").c_str()) : app.tracker.lastVehicleId();
		state->lastPass = req.has_param("since") ? state->lastVehicle : app.tracker.lastPassId();
		std::string token = presented(req);
		res.set_chunked_content_provider("text/event-stream", [this, &app, state, token, slot](size_t, httplib::DataSink& sink) {
			if (!tokenMatches(token)) return false;   // the pairing code was reset: this phone is signed out
			auto send = [&](const char* ev, const json& data) {
				std::string msg = std::string("event: ") + ev + "\ndata: " + data.dump() + "\n\n";
				return sink.write(msg.data(), msg.size());
			};
			if (state->tick++ % 5 == 0) {
				// One trip to the GUI thread for both bodies.  The config can change from anywhere (the desktop's
				// own editors, PUT /api/config or a calibration from any phone), so rather than hooking every
				// writer, each stream compares the config with what it last sent: a change reaches every client
				// within about a second, and the first tick always sends it.
				json both = json::parse(app.post([&app] { return json::array({ statusJson(app), configJson(app) }).dump(); }), nullptr, false);
				if (both.is_array() && both.size() == 2) {
					if (!send("status", both[0])) return false;
					std::string cfg = both[1].dump();
					if (cfg != state->config) {
						if (!send("config", both[1])) return false;
						state->config = cfg;
					}
				}
			}
			for (const Vehicle& v : app.tracker.vehiclesSince(state->lastVehicle)) {
				state->lastVehicle = v.id;
				if (!send("vehicle", vehicleJson(v))) return false;
			}
			// A pass timed under minLoggedSpeed: not a logged vehicle, but a calibration drive at 10 or 15 mph is one.
			for (const Vehicle& v : app.tracker.passesSince(state->lastPass)) {
				state->lastPass = v.id;
				if (v.belowMin && !send("pass", vehicleJson(v))) return false;
			}
			Tracker::Live live;
			if (app.tracker.running() && app.tracker.latest(live, false)) {
				json tracks = json::array();
				int i = 0;
				for (auto& t : live.boxes)
					tracks.push_back({ { "id", i++ }, { "dir", t.leftToRight ? "L2R" : "R2L" }, { "x", live.analysisBox.x + t.box.x },
						{ "y", live.analysisBox.y + t.box.y }, { "w", t.box.width }, { "h", t.box.height }, { "speed", t.speed } });
				if (!send("tracks", tracks)) return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			// A comment line every tick (EventSource ignores it).  A closed client is only noticed when a write to it
			// fails, and while nothing is tracked the events above go out about once a second, longer if the GUI
			// thread is slow to answer post(); without this a closed tab kept its stream slot for that long, and a
			// view reopened straight away could find all kMaxStreams taken.
			return sink.write(":\n\n", 3) && sink.is_writable();
		});
	}));

	http_->Get("/api/health", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return healthJson(app).dump(); }));
	}));

	http_->Get("/api/config", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return configJson(app).dump(); }));
	}));

	http_->Put("/api/config", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			auto problems = applyConfig(app, in);
			return json{ { "ok", problems.empty() }, { "problems", problems }, { "config", configJson(app) } }.dump();
		});
		// Rejected settings are a normal answer (200, ok false, the problems in plain words), not an HTTP error: the
		// companion shows the problems next to the fields, and its client treats any non-2xx as a failed request.
		sendPosted(res, out);
	}));

	http_->Get("/api/plan", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return planJson(app).dump(); }));
	}));

	http_->Put("/api/plan", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		// From this computer it is the map (or the companion page) in a browser here; otherwise the phone.
		std::string who = req.remote_addr == "127.0.0.1" || req.remote_addr == "::1" ? "from your browser" : "from the phone";
		sendPosted(res, app.post([&app, in, who] {
			auto problems = applyPlan(app, in);
			// Say so on the desktop, so a change from the phone never lands unnoticed in the middle of someone's edit.
			if (problems.empty()) {
				if (in.contains("save")) app.toast("Pick your spot was saved " + who + ".", theme::c::ok, ICON_CHECK);
				else if (in.contains("discard")) app.toast("Pick your spot's changes were discarded " + who + ".", theme::c::dim, ICON_UNDO);
				else if (!in.contains("autosave") && !app.prefs.planAutosave)
					app.toast("Pick your spot changed " + who + ": a preview until it's saved.", theme::c::accent2, ICON_BROADCAST);
			}
			return json{ { "ok", problems.empty() }, { "problems", problems }, { "plan", planJson(app) } }.dump();
		}));   // 200 either way, as for PUT /api/config
	}));

	http_->Post("/api/calibration/mark", guard([&app, sendJson, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		int dir = in.value("dir", "L2R") == "R2L" ? 1 : 0;
		int edge = in.value("edge", "start") == "end" ? 1 : 0;
		sendJson(res, json::parse(app.post([&app, dir, edge] {
			int frame = app.tracker.running() ? app.tracker.progress().frame : std::max(app.player.position(), 0);
			app.calibMarks[dir][edge] = frame;
			return json{ { "ok", true }, { "frame", frame }, { "marks", { app.calibMarks[dir][0], app.calibMarks[dir][1] } } }.dump();
		}), nullptr, false));
	}));

	http_->Post("/api/calibration/apply", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			double speed = in.value("speed", 0.0);
			int vid = in.value("vehicleId", 0);
			std::string err;
			if (speed <= 0) err = "speed must be > 0";
			else if (vid) {
				Vehicle v;
				if (!app.tracker.vehicle(vid, v)) err = "no vehicle " + std::to_string(vid);
				else {
					app.applyCalibration(in.contains("dir") ? in.value("dir", "L2R") == "L2R" : v.ev.leftToRight, speed, v.frames(), v.fps);
					app.markCalibrationPass(v, false, speed);   // Results marks it, the street report leaves it out
				}
			}
			else {
				int dir = in.value("dir", "L2R") == "R2L" ? 1 : 0;
				int* m = app.calibMarks[dir];
				if (m[0] < 0 || m[1] <= m[0]) err = "mark the start and end frames first";
				else app.applyCalibration(dir == 0, speed, m[1] - m[0], app.player.info().fps);
			}
			if (err.empty() && !app.cfg.save()) err = app.cfg.error;
			return json{ { "ok", err.empty() }, { "problems", err.empty() ? json::array() : json::array({ err }) }, { "config", configJson(app) } }.dump();
		});
		sendPosted(res, out);   // 200 either way, as for PUT /api/config
	}));

	// Take one calibration input out, or put it back: {"id": "...", "removed": true|false}.  The rest are recombined
	// into CalibrationFrames at once.  200 {ok, problems, calibration} like apply; an unknown id is a problem.
	http_->Post("/api/calibration/inputs", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		sendPosted(res, app.post([&app, in] {
			std::string err, id = in.value("id", "");
			bool removed = in.value("removed", true);
			if (id.empty()) err = "id is required";
			else if (!app.setCalibrationInputRemoved(id, removed)) err = "no calibration input " + id;
			else {
				app.toast(removed ? "A calibration measurement was taken out from the phone." : "A calibration measurement was put back from the phone.",
					theme::c::accent2, ICON_BROADCAST);
				if (!app.cfg.save()) err = app.cfg.error;
			}
			return json{ { "ok", err.empty() }, { "problems", err.empty() ? json::array() : json::array({ err }) },
				{ "calibration", calibrationJson(app) }, { "config", configJson(app) } }.dump();
		}));
	}));

	// Calibration from two cones a measured distance apart (the Calibrate page's "Cones or pole" mode, from the street):
	// {"cones": [[x, y], [x, y]] in the 1280x720 grid, "feet": 12, "dir": "both"|"L2R"|"R2L"}.  Refusals are 200
	// {ok: false, problems}, as for apply.  The cones also show on VST Studio's Calibrate page.
	http_->Post("/api/calibration/distance", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			std::string err, dir = in.value("dir", "both");
			double feet = in.contains("feet") && in["feet"].is_number() ? in["feet"].get<double>() : 0;
			double xy[2][2] = { { -1, -1 }, { -1, -1 } };
			const json* cones = in.contains("cones") ? &in["cones"] : nullptr;
			bool shaped = cones && cones->is_array() && cones->size() == 2;
			for (int i = 0; shaped && i < 2; i++) {
				const json& c = (*cones)[i];
				shaped = c.is_array() && c.size() == 2 && c[0].is_number() && c[1].is_number();
				if (shaped) { xy[i][0] = c[0].get<double>(); xy[i][1] = c[1].get<double>(); }
				shaped = shaped && xy[i][0] >= 0 && xy[i][0] <= 1280 && xy[i][1] >= 0 && xy[i][1] <= 720;
			}
			const Globals& g = app.cfg.g;
			double markPx = shaped ? std::fabs(xy[1][0] - xy[0][0]) : 0;
			double frames = calib::framesForDistance(markPx, feet, g.speedLineRight - g.speedLineLeft, g.CalibrationSpeed, g.CalibrationFPS);
			if (!shaped) err = "mark both spots on the picture first";
			else if (!(feet > 0)) err = "enter the distance between the spots in feet";
			else if (dir != "both" && dir != "L2R" && dir != "R2L") err = "dir must be both, L2R or R2L";
			else if (markPx < 40) err = "the two spots are too close together on the picture; space them further apart";
			else if (!(frames > 0)) err = "the speed lines aren't set yet";
			if (shaped)
				for (int i = 0; i < 2; i++) { app.coneX_[i] = float(xy[i][0]); app.coneY_[i] = float(xy[i][1]); }
			if (err.empty()) {
				char label[64];
				snprintf(label, sizeof label, "Cones or pole, %g ft apart", feet);
				app.applyDistanceCalibration(dir != "R2L", dir != "L2R", frames, "the cones", "cones", calib::sigmaCones(markPx), label);
				if (!app.cfg.save()) err = app.cfg.error;
			}
			json r{ { "ok", err.empty() }, { "problems", err.empty() ? json::array() : json::array({ err }) }, { "config", configJson(app) } };
			if (frames > 0) { r["zoneFeet"] = std::round((g.speedLineRight - g.speedLineLeft) / markPx * feet * 10) / 10; r["frames"] = std::round(frames * 10) / 10; }
			return r.dump();
		});
		sendPosted(res, out);   // 200 either way, as for apply
	}));

	// The Known car mode's models, so the phone's picker matches Studio's.
	http_->Get("/api/calibration/cars", guard([sendJson](const Request&, Response& res) {
		json cars = json::array();
		for (const calib::KnownCar& k : calib::kKnownCars) cars.push_back({ { "name", k.name }, { "lengthIn", k.inches }, { "wheelbaseIn", k.wheelbaseIn } });
		sendJson(res, { { "cars", cars }, { "typicalWheelbaseIn", std::round(calib::kTypicalWheelbaseFt * 12) } }, 200);
	}));

	// Known car: two marks on a car (its wheel centres, or its bumpers) `feet` apart.  The marks are Studio's own Known car
	// marks (so the desktop shows them too), "where" says whether the car is parked: "auto" asks the parking lane (no
	// parking lane: in the travel lane), "parked" with no parking lane uses beyondFt (default 7) through the camera model.
	// "preview": true answers without applying.  On a two-way street "both" calibrates each lane with its own scale.
	http_->Post("/api/calibration/car", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			std::string err, dir = in.value("dir", "both"), where = in.value("where", "auto"), how = in.value("how", "wheels");
			double feet = in.contains("feet") && in["feet"].is_number() ? in["feet"].get<double>() : 0;
			double beyond = in.contains("beyondFt") && in["beyondFt"].is_number() ? in["beyondFt"].get<double>() : calib::kTypicalParkFt;
			bool preview = in.value("preview", false);
			double xy[2][2] = { { -1, -1 }, { -1, -1 } };
			const json* marks = in.contains("marks") ? &in["marks"] : nullptr;
			bool shaped = marks && marks->is_array() && marks->size() == 2;
			for (int i = 0; shaped && i < 2; i++) {
				const json& c = (*marks)[i];
				shaped = c.is_array() && c.size() == 2 && c[0].is_number() && c[1].is_number();
				if (shaped) { xy[i][0] = c[0].get<double>(); xy[i][1] = c[1].get<double>(); }
				shaped = shaped && xy[i][0] >= 0 && xy[i][0] <= 1280 && xy[i][1] >= 0 && xy[i][1] <= 720;
			}
			const Globals& g = app.cfg.g;
			const bool oneWay = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
			double markPx = shaped ? std::fabs(xy[1][0] - xy[0][0]) : 0;
			if (!shaped) err = "mark both points on the car first";
			else if (!(feet > 0)) err = "enter the car's wheelbase (or length) in feet";
			else if (dir != "both" && dir != "L2R" && dir != "R2L") err = "dir must be both, L2R or R2L";
			else if (where != "auto" && where != "lane" && where != "parked") err = "where must be auto, lane or parked";
			else if (how != "wheels" && how != "bumpers") err = "how must be wheels or bumpers";
			else if (!(beyond >= -30 && beyond <= 50)) err = "beyondFt must be between -30 and 50";
			else if (markPx < 60) err = "the car is too small on the picture to measure; pick a nearer one";
			else if (g.speedLineRight <= g.speedLineLeft) err = "the speed lines aren't set yet";
			json r{ { "ok", false }, { "problems", json::array() } };
			if (shaped) {
				// Studio's Known car state follows, so the desktop shows the same car and choice.
				for (int i = 0; i < 2; i++) { app.carX_[i] = float(xy[i][0]); app.carY_[i] = float(xy[i][1]); }
				app.carWheels_ = how == "wheels";
				if (feet > 0) { app.carFt_ = feet; app.carModel_ = -1; }
				const bool primaryL2R = oneWay ? g.LegalDirection == "L2R" : dir != "R2L";
				calib::CarSpot spot = app.carSpot(primaryL2R);
				bool parkSet = app.parkingLane().set();
				app.carParked_ = where == "parked" || (where == "auto" && spot.where == calib::CarSpot::Parked);
				if (!parkSet) app.carFartherFt_ = beyond;
				std::copy(app.carX_, app.carX_ + 2, app.carAutoFor_);
				std::copy(app.carY_, app.carY_ + 2, app.carAutoFor_ + 2);
				r["where"] = app.carParked_ ? "parked" : "lane";
				r["detected"] = spot.where == calib::CarSpot::Unknown ? json(nullptr) : json(spot.where == calib::CarSpot::Parked ? "parked" : "lane");
				r["offLines"] = parkSet && spot.offLines;
				double bft = parkSet ? spot.parkedFt : app.carFartherFt_;
				r["beyondFt"] = app.carParked_ && std::isfinite(bft) ? json(std::round(bft * 10) / 10) : json(nullptr);
				r["beyondHow"] = !app.carParked_ ? json(nullptr) : !parkSet ? json("typed") : json(spot.fromPicture ? "picture" : spot.typical ? "typical" : "measured");
				json byDir = json::object();
				std::map<std::string, double> rawFrames;   // unrounded, for the apply
				double zonePx = g.speedLineRight - g.speedLineLeft;
				for (bool l2r : { true, false }) {
					if ((l2r && dir == "R2L") || (!l2r && dir == "L2R")) continue;
					if (oneWay && l2r != primaryL2R && dir == "both") continue;   // one lane: the same numbers twice
					double sc = app.carScale(l2r);
					double frames = calib::framesForDistance(markPx * sc, feet, zonePx, g.CalibrationSpeed, g.CalibrationFPS);
					rawFrames[l2r ? "L2R" : "R2L"] = frames;
					byDir[l2r ? "L2R" : "R2L"] = { { "scale", std::round(sc * 1000) / 1000 }, { "frames", std::round(frames * 10) / 10 },
						{ "zoneFeet", frames > 0 ? json(std::round(zonePx / (markPx * sc) * feet * 10) / 10) : json(nullptr) } };
				}
				const json& first = byDir[primaryL2R ? "L2R" : "R2L"];
				r["scale"] = first["scale"];
				r["frames"] = first["frames"];
				r["zoneFeet"] = first["zoneFeet"];
				r["byDir"] = byDir;
				if (err.empty() && !preview) {
					// Added to the calibration as a "car" input per direction, weighted like Studio's Known car card.
					const char* from = how == "wheels" ? "the car's wheelbase" : "the car's length";
					double farFt = parkSet && std::isfinite(spot.parkedFt) ? spot.parkedFt : app.carFartherFt_;
					double sigma = calib::sigmaCar(markPx, app.carParked_, app.prefs.planDist * plan::kFt, farFt);
					std::string label = std::string(how == "wheels" ? "A car's wheelbase" : "A car of known length") + (app.carParked_ ? ", parked" : "");
					for (const auto& [d, frames] : rawFrames) {
						bool l2r = d == "L2R";
						// One lane on a one-way street: "both" applies the one number to both directions.
						bool both = oneWay && dir == "both";
						app.applyDistanceCalibration(l2r || both, !l2r || both, frames, from, "car", sigma,
							both || rawFrames.size() == 1 ? label : label + (l2r ? ", →" : ", ←"));
					}
					if (!app.cfg.save()) err = app.cfg.error;
				}
			}
			r["ok"] = err.empty();
			if (!err.empty()) r["problems"] = json::array({ err });
			r["config"] = configJson(app);
			return r.dump();
		});
		sendPosted(res, out);   // 200 either way, as for apply
	}));

	// A check pass: the same inputs as apply (a logged pass, or the hand-marked span), but nothing changes except a
	// "check" row in calibration-history.csv.  The answer says what the calibration in force makes of the pass.
	http_->Post("/api/calibration/check", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			double speed = in.value("speed", 0.0);
			int vid = in.value("vehicleId", 0);
			std::string err;
			CalCheck k;
			if (speed <= 0) err = "speed must be > 0";
			else if (vid) {
				Vehicle v;
				if (!app.tracker.vehicle(vid, v)) err = "no vehicle " + std::to_string(vid);
				else {
					k = app.checkCalibration(in.contains("dir") ? in.value("dir", "L2R") == "L2R" : v.ev.leftToRight, speed, v.frames(), v.fps);
					app.markCalibrationPass(v, true, speed);
				}
			}
			else {
				int dir = in.value("dir", "L2R") == "R2L" ? 1 : 0;
				int* m = app.calibMarks[dir];
				if (m[0] < 0 || m[1] <= m[0]) err = "mark the start and end frames first";
				else k = app.checkCalibration(dir == 0, speed, m[1] - m[0], app.player.info().fps);
			}
			if (!err.empty()) return json{ { "ok", false }, { "problems", json::array({ err }) } }.dump();
			return json{ { "ok", true }, { "problems", json::array() }, { "direction", k.l2r ? "L2R" : "R2L" },
				{ "drivenMph", k.drivenMph }, { "measuredMph", k.measuredMph }, { "error", k.error }, { "verified", k.verified } }.dump();
		});
		sendPosted(res, out);   // 200 either way, as for apply
	}));

	http_->Post("/api/run", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string action = in.value("action", "");
		if (action != "start" && action != "stop" && action != "pause" && action != "resume") {
			sendJson(res, { { "error", "action must be start, stop, pause or resume" } }, 400);
			return;
		}
		std::string out = app.post([&app, action] {
			if (action == "start") app.startTracking();
			else if (action == "stop") app.stopTracking();
			else if (action == "pause") app.tracker.setPaused(true);
			else app.tracker.setPaused(false);
			// The status after the action, so the caller needn't ask again.  "start" can be refused (no source,
			// invalid zones); ok is still true then and status.running stays false, while the desktop shows why.
			return json{ { "ok", true }, { "status", statusJson(app) } }.dump();
		});
		sendPosted(res, out);
	}));

	http_->Get("/api/stats.csv", guard([&app, sendJson](const Request&, Response& res) {
		std::string p = app.tracker.lastStatsPath();
		std::ifstream in(p);
		if (p.empty() || !in) { sendJson(res, { { "error", "no stats yet; run the tracker first" } }, 404); return; }
		std::ostringstream buf;
		buf << in.rdbuf();
		res.set_header("Content-Disposition", "attachment; filename=\"" + fs::path(p).filename().string() + "\"");
		res.set_content(buf.str(), "text/csv");
	}));

	// The street report (Report.h) for the phone to open or share: the same file "Make report" writes, without example
	// frames (those stay an explicit choice on the desktop).  The GUI thread only copies the settings; reading the csvs
	// and building the page happen here, so a big data folder never stalls the window.
	http_->Get("/api/report.html", guard([&app, sendJson](const Request& req, Response& res) {
		std::string from = req.has_param("from") ? req.get_param_value("from") : "";
		std::string to = req.has_param("to") ? req.get_param_value("to") : "";
		int64_t day;
		for (const std::string* d : { &from, &to })
			if (!d->empty() && !report::parseDay(*d, day)) { sendJson(res, { { "error", "from and to must be dates like 2026-11-02" } }, 400); return; }
		// Shared with the posted task, which may still run after a timeout here.
		auto job = std::make_shared<std::pair<report::Input, report::Settings>>();
		std::string ok = app.post([&app, job] {
			job->first = app.reportInput(false, "", "");
			job->second = app.reportSettings();
			return std::string("ok");
		});
		if (ok.empty()) { sendJson(res, { { "error", "VST Studio is busy; try again" } }, 503); return; }
		job->second.from = from;
		job->second.to = to;
		std::string path, html, err;
		if (!report::writeReport(job->first, job->second, path, html, err)) { sendJson(res, { { "error", err } }, 500); return; }
		res.set_header("Content-Disposition", "inline; filename=\"" + fs::path(path).filename().string() + "\"");
		res.set_content(html, "text/html; charset=utf-8");
	}));

	// The companion web app itself, if installed next to the program.
	fs::path web = userpaths::executableDir() / "companion";
	std::error_code ec;
	if (!fs::is_directory(web, ec)) web = userpaths::executableDir() / ".." / "share" / "videospeedtracker" / "companion";
	if (!fs::is_directory(web, ec)) web = userpaths::executableDir() / ".." / "Resources" / "companion";  // VSTStudio.app
	if (fs::is_directory(web, ec)) http_->set_mount_point("/", web.string());
	else
		http_->Get("/", [](const Request&, Response& res) {
			res.set_content("<!doctype html><meta name=viewport content='width=device-width'><title>VST</title>"
				"<body style='font:16px system-ui;background:#0b0e14;color:#e7ebf3;padding:24px'>"
				"<h2>VST Studio is running</h2><p>The companion web app isn't installed with this copy. Its API is at <code>/api</code>.</p>",
				"text/html");
		});
}

void Server::drawQr(const std::string& text, float size) {
	using qrcodegen::QrCode;
	QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
	int n = qr.getSize(), quiet = 3;
	float cell = std::floor(size / (n + quiet * 2));
	float total = cell * (n + quiet * 2);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, ImVec2(p.x + total, p.y + total), IM_COL32_WHITE, theme::px(10));
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qr.getModule(x, y)) {
				ImVec2 a(p.x + (x + quiet) * cell, p.y + (y + quiet) * cell);
				dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(11, 14, 20, 255));
			}
	ImGui::Dummy(ImVec2(total, total));
}
