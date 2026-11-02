// VST Studio: calibration sets in Results (CalSets.h) and published datasets (Dataset.h).
//
// Flow:  syncSets() files every saved run into a calibration set (throttled; forced after a calibration, a removal or a
// put-back)  ->  applySets() re-times Results' rows to their set's calibration and sizes them at the same scale  ->
// the table draws a colour band per set down each row's left edge (setStripe)  ->  setsBar() names the sets on screen
// and, for the one picked (a band, a chip or a row), says what state it is in and offers Lock / Unlock, Publish,
// Remove and "check against your latest calibration"  ->  the dialogs do those through calsets, archive and dataset.
//
// Words are written for someone who has never calibrated anything; the statistics stay one click deeper.
#include "App.h"
#include "Icons.h"
#include "Theme.h"
#include <imgui.h>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace theme;

namespace {
// The stripe colours, by set number: blue, pale slate, teal, sand.  None is a speed or direction colour,
// so a band reads as "which set" and never as "how fast".  Neighbouring sets always differ (Joe's alternating stripes),
// and four of them keep up to four sets apart when the table is sorted by speed rather than time.
const ImU32 kStripes[] = { c::accent, c::rgb(0xC3CBD9), c::rgb(0x2DD4BF), c::rgb(0xD6C7A1) };

bool sameDir(const fs::path& a, const fs::path& b) { return a.lexically_normal() == b.lexically_normal(); }

ImU32 verdictColor(dataset::Verdict v) {
	switch (v) {
	case dataset::Verdict::Accurate: return c::ok;
	case dataset::Verdict::Review: return c::warn;
	case dataset::Verdict::Tainted: return c::bad;
	default: return c::bg3;
	}
}
const char* verdictIcon(dataset::Verdict v) {
	switch (v) {
	case dataset::Verdict::Accurate: return ICON_CHECK_CIRCLE;
	case dataset::Verdict::Review: return ICON_INFO;
	case dataset::Verdict::Tainted: return ICON_WARN;
	default: return ICON_CERTIFICATE;
	}
}
std::string pct(double x, int places = 1) {
	char b[32];
	snprintf(b, sizeof b, "%.*f%%", places, x * 100);
	return b;
}
std::string signedPct(double x) {
	char b[32];
	snprintf(b, sizeof b, "%+.1f%%", x * 100);
	return b;
}
}  // namespace

// ---- the model ------------------------------------------------------------------------------------------------

calsets::View App::currentView() const {
	const Globals& g = cfg.g;
	calsets::View v;
	v.lineLeft = g.speedLineLeft;
	v.lineRight = g.speedLineRight;
	v.edge[0] = g.streetEdge[0];
	v.edge[1] = g.streetEdge[1];
	v.level = g.levelDegrees;
	v.box[0] = g.AnalysisBoxLeft;
	v.box[1] = g.AnalysisBoxTop;
	v.box[2] = g.AnalysisBoxWidth;
	v.box[3] = g.AnalysisBoxHeight;
	return v;
}

calsets::Scale App::currentScale() const {
	const Globals& g = cfg.g;
	calsets::Scale s;
	s.l2r = calsets::zoneFeet(g.CalibrationFramesL2R, g.CalibrationSpeed, g.CalibrationFPS);
	s.r2l = calsets::zoneFeet(g.CalibrationFramesR2L, g.CalibrationSpeed, g.CalibrationFPS);
	return s;
}

calsets::Scale App::currentSigma() {
	calsets::Scale s;
	calib::Combined l = calibrationCombined(true), r = calibrationCombined(false);
	s.l2r = l.valid ? l.sigma : 0;
	s.r2l = r.valid ? r.sigma : 0;
	return s;
}

dataset::Calibration App::calibrationNow() {
	dataset::Calibration c;
	calsets::Scale ft = currentScale(), sg = currentSigma();
	c.zoneFt[0] = ft.l2r;
	c.zoneFt[1] = ft.r2l;
	c.sigma[0] = sg.l2r;
	c.sigma[1] = sg.r2l;
	// In words, from the direction the street is legal in (either when both are).
	bool l2r = cfg.g.LegalDirection != "R2L";
	calib::Combined k = calibrationCombined(l2r);
	c.how = k.valid ? calib::summary(k) : std::string("one calibration, method not recorded");
	return c;
}

bool App::syncSets(bool force) {
	if (screenshotMode) return false;
	double t = ImGui::GetTime();
	if (!force && setsAt_ >= 0 && t - setsAt_ < 3) return false;
	setsAt_ = t;
	fs::path stats = fs::path(dataDir()) / "Stats";
	if (!setsLoaded_ || setsFor_ != stats.string()) {
		sets_ = calsets::load(stats.string());
		setsFor_ = stats.string();
		setsLoaded_ = true;
	}
	// Every saved run at the top of Stats/.  The run being written now joins once its manifest exists (a live run's
	// is written at its first frames, a video's only at the end): until then it would look like a run with no
	// calibration on record.
	std::string running = tracker.runningStatsPath();
	std::vector<calsets::RunInfo> runs;
	std::error_code ec;
	for (fs::directory_iterator it(stats, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_regular_file(ec) || it->path().extension() != ".csv") continue;
		calsets::RunInfo r = calsets::readRun(it->path().string());
		if (!r.known && !running.empty() && sameDir(it->path(), running)) continue;
		runs.push_back(r);
	}
	bool changed = calsets::sync(sets_, runs, currentView(), currentScale(), currentSigma(), calib::nowIso());
	if (changed) {
		std::string err;
		if (!calsets::save(stats.string(), sets_, err)) toast(err, c::warn, ICON_WARN);
		if (selectedSet_ && !sets_.find(selectedSet_)) selectedSet_ = 0;
		if (setFilter_ && !sets_.find(setFilter_)) setFilter_ = 0;
	}
	return changed;
}

const calsets::Set* App::setOfRow(const ResultRow& r) const {
	if (r.statsPath.empty() || setsFor_.empty()) return nullptr;
	fs::path p(r.statsPath);
	if (!sameDir(p.parent_path(), setsFor_)) return nullptr;   // a csv opened from elsewhere isn't one of the data folder's runs
	return sets_.ofRun(p.filename().string());
}

void App::applySets() {
	if (reviewing()) return;   // an archive's speeds are as it froze them
	syncSets();
	retimeRows(rows);
	sizeRows();
}

void App::retimeRows(std::vector<ResultRow>& out) {
	const calsets::View now = currentView();
	const calsets::Scale nowFt = currentScale();
	// The run being written now has no set yet; its rows wear the stripe of the open set on today's view, which it joins.
	int openHere = 0;
	for (const calsets::Set& s : sets_.sets) if (s.known && !s.locked() && s.view.same(now)) openHere = s.id;
	for (ResultRow& r : out) {
		const calsets::Set* set = setOfRow(r);
		r.setId = set ? set->id : (!r.statsPath.empty() && r.vehicleId ? openHere : 0);
		calsets::Scale shown = set ? calsets::shownScale(*set, now, nowFt) : calsets::Scale();
		calsets::Retimed t = calsets::retime(r.measured, r.thenFt, set && set->known ? shown.of(r.l2r) : 0);
		if (r.measured.speed <= 0) continue;   // never measured here (nothing to re-time)
		r.speed = t.speed;
		if (r.measured.trend) { r.entryMph = t.entryMph; r.exitMph = t.exitMph; r.mphIn20ft = t.mphIn20ft; r.accel = t.accel; }
		r.adjusted = t.adjusted;
	}
}

ImU32 App::setColor(int setId) const {
	// By set number, not position: removing or archiving a set never repaints the others, and consecutive numbers
	// (sets are numbered as they're made) always get different colours.
	const calsets::Set* s = setId ? sets_.find(setId) : nullptr;
	if (!s) return 0;
	return !s->known ? c::faint : kStripes[(s->id - 1) % std::size(kStripes)];
}

// The band at the row's left edge, in the first column's cell padding.  Hovering it names the set; clicking picks it.
void App::setStripe(const ResultRow& r, float rowH) {
	ImU32 col = setColor(r.setId);
	if (!col) return;
	ImVec2 p = ImGui::GetCursorScreenPos();
	const ImGuiStyle& st = ImGui::GetStyle();
	ImVec2 a(p.x - st.CellPadding.x, p.y - st.CellPadding.y), b(a.x + px(5), a.y + std::max(rowH, ImGui::GetFrameHeight() + 2 * st.CellPadding.y));
	ImGui::GetWindowDrawList()->AddRectFilled(a, b, col);
	if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, ImVec2(b.x + px(3), b.y))) {
		if (const calsets::Set* s = sets_.find(r.setId)) {
			ImGui::SetTooltip("%s", calsets::describe(*s, calsets::shownScale(*s, currentView(), currentScale())).c_str());
			if (ImGui::IsMouseClicked(0)) selectedSet_ = s->id;
		}
		else ImGui::SetTooltip("This run joins the open set when it finishes");
	}
}

// ---- the set bar ---------------------------------------------------------------------------------------------

void App::setsBar(float width) {
	if (reviewing()) return;
	if (applySetsDue_ || syncSets()) { applySetsDue_ = false; applySets(); }
	// The sets on screen, oldest first.
	std::vector<const calsets::Set*> here;
	for (const calsets::Set& s : sets_.sets)
		for (const ResultRow& r : rows)
			if (r.setId == s.id) { here.push_back(&s); break; }
	if (here.empty()) return;
	if (here.size() == 1 && !selectedSet_) selectedSet_ = here[0]->id;
	const calsets::View now = currentView();
	const calsets::Scale nowFt = currentScale();

	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextUnformatted(here.size() == 1 ? "CALIBRATION SET" : "CALIBRATION SETS");
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Runs measured from the same camera view and speed lines share a set. An open set's speeds follow your\n"
			"latest calibration; a locked one keeps the calibration it was measured with. The band down each row's\n"
			"left edge shows its set.");
	for (const calsets::Set* s : here) {
		ImGui::SameLine(0, px(10));
		ImGui::PushID(s->id);
		ImVec2 p = ImGui::GetCursorScreenPos();
		float h = ImGui::GetFrameHeight();
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + px(4)), ImVec2(p.x + px(5), p.y + h - px(4)), setColor(s->id));
		ImGui::Dummy(ImVec2(px(8), h));
		ImGui::SameLine(0, 0);
		std::string label = "Set " + std::to_string(s->id);
		if (!s->published.empty()) label += "  " ICON_CERTIFICATE;
		else if (s->locked()) label += "  " ICON_LOCK;
		if (ImGui::Selectable(label.c_str(), selectedSet_ == s->id, 0, ImVec2(ImGui::CalcTextSize(label.c_str()).x, h)))
			selectedSet_ = selectedSet_ == s->id && here.size() > 1 ? 0 : s->id;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", calsets::describe(*s, calsets::shownScale(*s, now, nowFt)).c_str());
		ImGui::PopID();
	}
	const calsets::Set* s = selectedSet_ ? sets_.find(selectedSet_) : nullptr;
	if (!s) {
		dimText("Pick a set (or a row) to lock, publish or remove it.");
		return;
	}

	// The picked set: what it is, its state in a sentence, and what can be done with it.
	beginCard("setbar", nullptr, nullptr, ImVec2(width, 0));
	calsets::Scale shown = calsets::shownScale(*s, now, nowFt);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + px(2)), ImVec2(p.x + px(5), p.y + ImGui::GetTextLineHeight() - px(2)), setColor(s->id));
	ImGui::Dummy(ImVec2(px(10), 0));
	ImGui::SameLine(0, 0);
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(calsets::describe(*s, shown).c_str());
	ImGui::PopFont();
	ImGui::PushTextWrapPos(0);
	if (!s->known)
		dimText("These runs are from before VST kept each run's calibration, so their speeds are shown as measured and can't be adjusted.");
	else if (!s->locked() && s->view.same(now))
		dimText("Open: runs you record from this camera view join it, and its speeds follow your latest calibration. Recalibrate and every "
			"speed here is re-timed to match.");
	else if (!s->locked())
		dimText("Open, but the speed lines have moved since. Its speeds stay at the calibration it last had; it locks when you record from "
			"the new spot, or carries on if you move the lines back.");
	else
		dimText("Locked %s%s: nothing more joins it, and its speeds stay at the calibration it was locked with.",
			calsets::shortDate(s->lockedAt).empty() ? "" : ("on " + calsets::shortDate(s->lockedAt)).c_str(),
			s->lockReason == "view" ? " when the camera view changed" : "");
	ImGui::PopTextWrapPos();

	// A newer calibration of this same view: offer the check (and, unpublished, to take it).
	bool newer = calsets::newerCalibration(*s, now, nowFt);
	if (newer) {
		double d = nowFt.of(cfg.g.LegalDirection != "R2L") / shown.of(cfg.g.LegalDirection != "R2L") - 1;
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(v4(c::warn), ICON_RULER);
		ImGui::SameLine();
		// Plain direction words, not a signed percent: zone feet shorter = every speed lower by the same share.
		char pct[16];
		snprintf(pct, sizeof pct, "%.1f%%", std::fabs(d) * 100);
		ImGui::Text("Your latest calibration of this view would make these speeds %s %s.", pct, d < 0 ? "lower" : "higher");
		ImGui::SameLine();
		if (ghostButton(s->published.empty() ? ICON_GAUGE "  See what it would change" : ICON_GAUGE "  Re-check the published dataset")) {
			setForm_.setId = s->id;
			openRecheck_ = true;
		}
	}
	// Published datasets made from it, with their badge.
	for (size_t i = 0; i < s->published.size(); i++) {
		dataset::Info d;
		std::string err;
		fs::path dir = fs::path(dataDir()) / fs::path(s->published[i]);
		ImGui::PushID(int(i));
		if (dataset::read(dir.string(), d, err)) {
			dataset::Verdict v = d.status();
			ImGui::AlignTextToFramePadding();
			dimText(ICON_CERTIFICATE "  Published %s as \"%s\":", d.published.substr(0, std::min<size_t>(10, d.published.size())).c_str(), d.name.c_str());
			ImGui::SameLine();
			linePill((std::string(verdictIcon(v)) + "  " + dataset::verdictLabel(v)).c_str(), v == dataset::Verdict::None ? c::text : c::bg0, verdictColor(v));
			ImGui::SameLine();
			if (ghostButton(ICON_EYE "  Open")) openArchive(dir.string());
		}
		ImGui::PopID();
	}

	ImGui::Dummy(ImVec2(0, px(2)));
	if (here.size() > 1 || setFilter_) {
		bool only = setFilter_ == s->id;
		if (ghostButton(only ? ICON_FILTER "  Show every set" : ICON_FILTER "  Only this set")) setFilter_ = only ? 0 : s->id;
		ImGui::SameLine();
	}
	if (s->known && !s->locked()) {
		if (ghostButton(ICON_LOCK "  Lock")) {
			calsets::lock(sets_, s->id, now, nowFt, currentSigma(), calib::nowIso());
			std::string err;
			if (!calsets::save(setsFor_, sets_, err)) toast(err, c::warn, ICON_WARN);
			toast("Set " + std::to_string(selectedSet_) + " is locked: nothing more joins it and its speeds stay as they are", c::ok, ICON_LOCK);
			applySets();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Freeze this set: runs you record from now on start a new set, and a later calibration\n"
				"won't change these speeds unless you choose to apply it.");
		ImGui::SameLine();
	}
	else if (calsets::canUnlock(sets_, s->id, now) && s->published.empty()) {
		if (ghostButton(ICON_LOCK_OPEN "  Unlock")) {
			calsets::unlock(sets_, s->id, now);
			std::string err;
			if (!calsets::save(setsFor_, sets_, err)) toast(err, c::warn, ICON_WARN);
			syncSets(true);
			applySets();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collect again: new runs from this view join it, and its speeds follow your latest calibration.");
		ImGui::SameLine();
	}
	s = sets_.find(selectedSet_);   // Lock/Unlock saved; the pointer is still good, re-found for clarity
	if (s && s->known) {
		if (ghostButton(ICON_PUBLISH "  Publish…")) {
			setForm_ = SetForm();
			setForm_.setId = s->id;
			std::string def = (prefs.siteName.empty() ? std::string("Speeds") : prefs.siteName) + ", " + calsets::shortDate(s->first)
				+ (calsets::shortDate(s->last) != calsets::shortDate(s->first) && !s->last.empty() ? " to " + calsets::shortDate(s->last) : "");
			snprintf(setForm_.name, sizeof setForm_.name, "%s", def.c_str());
			openPublish_ = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Make a frozen copy that anyone can open and check, with the accuracy it promises.\n"
				"For neighbours, the city or a council meeting.");
		ImGui::SameLine();
	}
	if (s && ghostButton(ICON_TRASH "  Remove…")) {
		setForm_ = SetForm();
		setForm_.setId = s->id;
		if (selectedRow_ >= 0 && selectedRow_ < int(rows.size()) && rows[selectedRow_].setId == s->id && s->runs.size() > 1)
			setForm_.run = rows[selectedRow_].statsPath;
		snprintf(setForm_.name, sizeof setForm_.name, "Set %d, %s", s->id, calsets::shortDate(s->first).c_str());
		openRemove_ = true;
	}
	endCard();
}

// ---- rows of whole runs ----------------------------------------------------------------------------------------

// Every row of these runs at their set's calibration, with this session's snapshots and clips where it measured them:
// what Publish and Remove act on, whichever scope Results happens to show.
std::vector<ResultRow> App::rowsOfRuns(const std::vector<std::string>& csvs) {
	std::vector<ResultRow> was;
	was.swap(rows);
	fs::path stats(setsFor_);
	for (const std::string& c : csvs) {
		fs::path p = fs::path(c).is_absolute() ? fs::path(c) : stats / c;
		int skipped = 0;
		appendStatsRows(p.string(), p.filename().string(), &skipped);
	}
	// This session's vehicles from those runs carry a snapshot and clip: match them by run and frame.
	for (const Vehicle& v : tracker.vehiclesSince(0))
		for (ResultRow& r : rows)
			if (!v.statsPath.empty() && r.frame == v.ev.frame && sameDir(fs::path(r.statsPath), fs::path(v.statsPath))) {
				r.vehicleId = v.id;
				r.widthPx = r.widthPx ? r.widthPx : v.ev.width;
				r.clipPath = v.clipPath;
				r.source = v.video;
			}
	applySets();
	std::vector<ResultRow> out;
	out.swap(rows);
	rows.swap(was);
	return out;
}

// ---- Publish -----------------------------------------------------------------------------------------------------

bool App::publishSet(int setId, const std::string& name, std::string& dir, std::string& error) {
	const calsets::View now = currentView();
	const calsets::Scale nowFt = currentScale();
	calsets::Set* s = sets_.find(setId);
	if (!s || !s->known) { error = "That set can't be published: its calibration isn't on record."; return false; }
	// Publishing freezes the set first, so the numbers can't change under the people reading them.
	if (!s->locked()) calsets::lock(sets_, setId, now, nowFt, currentSigma(), calib::nowIso());
	std::vector<ResultRow> those = rowsOfRuns(s->runs);
	s = sets_.find(setId);
	if (!s) { error = "That set is gone."; return false; }
	if (those.empty()) { error = "This set has no vehicles to publish yet."; return false; }

	dataset::Info d;
	d.name = name;
	d.site = prefs.siteName;
	d.published = calib::nowIso();
	d.vst = VST_VERSION;
	d.speedLimit = prefs.speedLimit;
	d.wayOver = prefs.egregious;
	d.l2rCompass = cfg.g.L2RDirection;
	d.r2lCompass = cfg.g.R2LDirection;
	d.legal = cfg.g.LegalDirection;
	d.calibrationSet = s->id;
	d.view = s->view;
	d.cal.zoneFt[0] = s->scale.l2r;
	d.cal.zoneFt[1] = s->scale.r2l;
	d.cal.sigma[0] = s->sigma.l2r;
	d.cal.sigma[1] = s->sigma.r2l;
	d.cal.how = s->view.same(now) && s->scale.same(nowFt) ? calibrationNow().how : ("the calibration in effect when this set locked" + (s->lockedAt.empty() ? std::string() : " on " + calsets::shortDate(s->lockedAt))
		+ "; how it was measured wasn't recorded");
	for (size_t i = 0; i < those.size(); i++) {
		const ResultRow& r = those[i];
		dataset::Point p;
		p.n = int(i) + 1;
		p.passed = r.time;
		p.l2r = r.l2r;
		p.compass = r.compass;
		p.wrongWay = r.wrongWay;
		p.zoneSeconds = r.measured.zoneSeconds;
		p.speed = r.speed;
		p.trend = r.trend;
		p.entryMph = r.entryMph;
		p.exitMph = r.exitMph;
		p.mphIn20ft = r.mphIn20ft;
		p.accel = r.accel;
		p.lengthFt = r.lengthFt;
		p.flagged = r.flagged;
		p.ownDrive = r.calKind;
		p.run = fs::path(r.statsPath).filename().string();
		p.frame = r.frame;
		if (!r.time.empty() && (d.first.empty() || r.time < d.first)) d.first = r.time;
		if (!r.time.empty() && r.time > d.last) d.last = r.time;
		d.points.push_back(p);
	}
	d.summary = dataset::summarize(d.points, d.speedLimit, d.wayOver);

	archive::Request req = archiveRequest(name, false, &those);
	req.root = "Published";
	req.checksums = true;
	req.info.from = "Calibration set " + std::to_string(s->id);
	req.info.created = d.published;
	d.id = fs::path(archive::folderFor((fs::path(dataDir()) / "Published").string(), d.published, name)).filename().string();
	req.info.dataset = dataset::coverJson(d);
	req.extra = { { "dataset.json", dataset::datasetJson(d) }, { "points.csv", dataset::pointsCsv(d) } };
	std::vector<std::string> warnings;
	if (!archive::write(req, dir, error, &warnings)) return false;
	{
		std::ofstream st(fs::path(dir) / "status.json", std::ios::binary);
		st << dataset::statusJson(d);
	}
	s->published.push_back(fs::relative(fs::path(dir), fs::path(dataDir())).generic_string());
	std::string err;
	if (!calsets::save(setsFor_, sets_, err)) toast(err, c::warn, ICON_WARN);
	for (const std::string& w : warnings) toast(w, c::warn, ICON_WARN);
	return true;
}

void App::publishDialog() {
	if (openPublish_) { openPublish_ = false; setForm_.error.clear(); ImGui::OpenPopup("Publish this set"); }
	ImGui::SetNextWindowSize(ImVec2(px(600), 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Publish this set", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
	SetForm& f = setForm_;
	const calsets::Set* s = sets_.find(f.setId);
	if (!s) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
	calsets::Scale shown = calsets::shownScale(*s, currentView(), currentScale());
	dataset::Calibration cal;
	cal.sigma[0] = s->locked() ? s->sigma.l2r : currentSigma().l2r;
	cal.sigma[1] = s->locked() ? s->sigma.r2l : currentSigma().r2l;
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted(calsets::describe(*s, shown).c_str());
	dimText("Publishing makes a frozen copy that anyone can open and check: in VST Studio (Open), or by double-clicking review.html "
		"in any web browser. It states how accurate the speeds are (%s, with 95%% confidence) and carries checksums, so a copy can be "
		"shown to be unaltered. If you calibrate this view again later, you can re-check it and everyone sees the result.",
		dataset::accuracyWords(cal, prefs.speedLimit).c_str());
	if (!s->locked()) {
		ImGui::Dummy(ImVec2(0, px(4)));
		ImGui::TextColored(v4(c::warn), ICON_LOCK);
		ImGui::SameLine();
		ImGui::TextUnformatted("This locks the set first: runs you record from now on start a new set.");
	}
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, px(6)));
	float col = px(110);
	formLabel("Name", col);
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##pname", "e.g. Locust Ave, first week of October", f.name, sizeof f.name);
	if (!f.error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad)); ImGui::TextWrapped("%s", f.error.c_str()); ImGui::PopStyleColor(); }
	ImGui::Dummy(ImVec2(0, px(6)));
	if (primaryButton(ICON_PUBLISH "  Publish")) {
		std::string name = f.name;
		if (name.find_first_not_of(" \t") == std::string::npos) name = "Set " + std::to_string(s->id);
		std::string dir;
		f.error.clear();
		if (publishSet(f.setId, name, dir, f.error)) {
			ImGui::CloseCurrentPopup();
			toast("Published \"" + name + "\". Share its folder, or open review.html in it.", c::ok, ICON_CERTIFICATE);
			openArchive(dir);
		}
	}
	ImGui::SameLine();
	if (ghostButton("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// ---- Remove ------------------------------------------------------------------------------------------------------

void App::removeRuns(const std::vector<std::string>& csvs, bool toArchive, const std::string& name) {
	std::vector<ResultRow> those = rowsOfRuns(csvs);
	std::error_code ec;
	int n = int(those.size());
	if (toArchive) {
		std::string dir, err;
		std::vector<std::string> warnings;
		archive::Request req = archiveRequest(name, true, &those);
		req.info.from = name;
		for (const std::string& c : csvs) {   // a run that logged no vehicles still goes, with its manifest
			std::string p = (fs::path(c).is_absolute() ? fs::path(c) : fs::path(setsFor_) / c).string();
			if (std::find(req.statsCsvs.begin(), req.statsCsvs.end(), p) == req.statsCsvs.end()) req.statsCsvs.push_back(p);
		}
		if (!archive::write(req, dir, err, &warnings)) { toast(err, c::bad, ICON_WARN); return; }
		for (const std::string& w : warnings) toast(w, c::warn, ICON_WARN);
		toast("Moved " + std::to_string(n) + " vehicles to the archive \"" + name + "\". Open > Archives puts them back.", c::ok, ICON_ARCHIVE);
	}
	else {
		// For good: the csv, its manifest and highlights video, and this session's clips of its vehicles.
		int failed = 0;
		for (const std::string& c : csvs) {
			fs::path p = fs::path(c).is_absolute() ? fs::path(c) : fs::path(setsFor_) / c;
			for (const std::string& f : { p.string(), archive::manifestFor(p.string()), archive::highlightsFor(p.string()) })
				if (fs::exists(f, ec) && !fs::remove(f, ec)) failed++;
		}
		for (const ResultRow& r : those)
			if (!r.clipPath.empty() && fs::exists(r.clipPath, ec)) fs::remove(r.clipPath, ec);
		if (failed) toast("Some files couldn't be deleted (open in another program?)", c::warn, ICON_WARN);
		else toast("Deleted " + std::to_string(n) + " vehicles for good", c::ok, ICON_TRASH);
	}
	for (const std::string& c : csvs) tracker.forgetRun((fs::path(c).is_absolute() ? fs::path(c) : fs::path(setsFor_) / c).string());
	snapTex_.clear();
	syncSets(true);
	selectedRow_ = -1;
	if (rowsScope == ResultsScope::Saved) showSavedRuns();
	else if (rowsScope == ResultsScope::Session || !fs::exists(rowsFrom, ec)) showSessionResults();
	else applySets();
}

void App::removeDialog() {
	if (openRemove_) { openRemove_ = false; ImGui::OpenPopup("Remove"); }
	ImGui::SetNextWindowSize(ImVec2(px(600), 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Remove", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
	SetForm& f = setForm_;
	const calsets::Set* s = sets_.find(f.setId);
	if (!s) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted(calsets::describe(*s, calsets::shownScale(*s, currentView(), currentScale())).c_str());
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, px(4)));
	sectionLabel("WHAT");
	if (ImGui::RadioButton("The whole set", f.wholeSet)) f.wholeSet = true;
	ImGui::SameLine();
	dimText("%zu %s", s->runs.size(), s->runs.size() == 1 ? "run" : "runs");
	if (!f.run.empty()) {
		if (ImGui::RadioButton("Only the run the selected vehicle came from", !f.wholeSet)) f.wholeSet = false;
		ImGui::SameLine();
		dimText("%s", fs::path(f.run).filename().string().c_str());
	}
	ImGui::Dummy(ImVec2(0, px(4)));
	sectionLabel("HOW");
	if (ImGui::RadioButton("Move it to the archive", f.toArchive)) f.toArchive = true;
	ImGui::SameLine();
	dimText("(recommended)");
	ImGui::Indent(px(28));
	dimText("Out of Results, the street report and the phone's totals. Open > Archives shows it again and can put it back.");
	if (f.toArchive) {
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##rname", "Archive name", f.name, sizeof f.name);
	}
	ImGui::Unindent(px(28));
	if (ImGui::RadioButton("Delete it for good", !f.toArchive)) f.toArchive = false;
	ImGui::Indent(px(28));
	if (!f.toArchive) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::bad));
		ImGui::TextWrapped("This can't be undone: the results files, their highlights video and this session's clips are deleted.");
		ImGui::PopStyleColor();
		dimText("Type delete to confirm:");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(px(120));
		ImGui::InputText("##confirm", f.confirm, sizeof f.confirm);
	}
	else dimText("Removes the files from your data folder. Use it for test runs you never want to see again.");
	ImGui::Unindent(px(28));
	if (!s->published.empty()) {
		ImGui::Dummy(ImVec2(0, px(4)));
		ImGui::PushTextWrapPos(0);
		dimText(ICON_CERTIFICATE "  Its published dataset stays as it is: people you shared it with still have it.");
		ImGui::PopTextWrapPos();
	}
	ImGui::Dummy(ImVec2(0, px(6)));
	bool ok = f.toArchive || std::string(f.confirm) == "delete";
	ImGui::BeginDisabled(!ok);
	if (primaryButton(f.toArchive ? ICON_ARCHIVE "  Move to the archive" : ICON_TRASH "  Delete for good", ImVec2(0, 0), f.toArchive ? c::accent : c::bad)) {
		std::vector<std::string> csvs = f.wholeSet ? s->runs : std::vector<std::string>{ f.run };
		std::string name = f.name;
		if (name.find_first_not_of(" \t") == std::string::npos) name = "Set " + std::to_string(s->id);
		ImGui::CloseCurrentPopup();
		removeRuns(csvs, f.toArchive, name);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ghostButton("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// ---- checking against a newer calibration --------------------------------------------------------------------------

void App::recheckDialog() {
	static dataset::Info target;   // the dataset (or the locked set as one) the dialog previews
	if (openRecheck_) {
		openRecheck_ = false;
		target = dataset::Info();
		std::string err;
		if (!dataset_.dir.empty()) target = dataset_;   // from an open published dataset's cover
		else if (const calsets::Set* s = sets_.find(setForm_.setId)) {
			if (!s->published.empty()) dataset::read((fs::path(dataDir()) / fs::path(s->published.back())).string(), target, err);
			else {
				// A locked set that was never published: the same check, on its rows as they stand.
				target.calibrationSet = s->id;
				target.speedLimit = prefs.speedLimit;
				target.wayOver = prefs.egregious;
				target.cal.zoneFt[0] = s->scale.l2r;
				target.cal.zoneFt[1] = s->scale.r2l;
				target.cal.sigma[0] = s->sigma.l2r;
				target.cal.sigma[1] = s->sigma.r2l;
				for (const ResultRow& r : rowsOfRuns(s->runs)) {
					dataset::Point p;
					p.n = int(target.points.size()) + 1;
					p.l2r = r.l2r;
					p.speed = r.speed;
					p.zoneSeconds = r.measured.zoneSeconds;
					p.wrongWay = r.wrongWay;
					p.flagged = r.flagged;
					p.ownDrive = r.calKind;
					target.points.push_back(p);
				}
			}
		}
		recheck_ = dataset::evaluate(target, calibrationNow(), calib::nowIso());
		ImGui::OpenPopup("Check against your latest calibration");
	}
	ImGui::SetNextWindowSize(ImVec2(px(640), 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Check against your latest calibration", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
	const dataset::Check& k = recheck_;
	const bool published = !target.dir.empty();
	ImGui::PushTextWrapPos(0);
	linePill((std::string(verdictIcon(k.verdict)) + "  " + dataset::verdictLabel(k.verdict)).c_str(), c::bg0, verdictColor(k.verdict), fonts.strong);
	ImGui::Dummy(ImVec2(0, px(4)));
	// The one sentence, the same words the badge will carry.
	dataset::Info preview = target;
	preview.checks.push_back(k);
	if (published) ImGui::TextUnformatted(dataset::sentence(preview).c_str());
	else {
		const dataset::Test& t = k.test[k.test[0].verdict >= k.test[1].verdict ? 0 : 1];
		// Direction in words (zone shorter = speeds lower), then what the verdict means for someone deciding what to do.
		ImGui::Text("Your latest calibration would make this set's speeds %s %s. %s", pct(std::fabs(t.delta)).c_str(),
			t.delta < 0 ? "lower" : "higher",
			k.verdict == dataset::Verdict::Accurate ? "That is within what the set's own calibration promised, so either one is fine."
			: k.verdict == dataset::Verdict::Tainted ? "That is more than the set's own calibration promised: one of the two calibrations is off."
			: "That is close to what the set's own calibration promised, so we can't say yet which is right. Another calibration drive would settle it.");
	}
	ImGui::Dummy(ImVec2(0, px(6)));
	if (ImGui::BeginTable("rechk", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("", 0, 1.6f);
		ImGui::TableSetupColumn("Now", 0, 0.8f);
		ImGui::TableSetupColumn("With the newer calibration", 0, 2.2f);
		ImGui::TableHeadersRow();
		auto row = [](const char* what, const std::string& a, const std::string& b) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(what);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(a.c_str());
			ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
		};
		row("Cars over the limit", std::to_string(k.before.over), std::to_string(k.after.over));
		row("Way over the limit", std::to_string(k.before.wayOver), std::to_string(k.after.wayOver));
		row("85th percentile", std::to_string(k.before.p85) + " mph", std::to_string(k.after.p85) + " mph");
		char b[64];
		snprintf(b, sizeof b, "%.1f mph %s on average, at most %.1f", std::fabs(k.meanDiff), k.meanDiff < 0 ? "slower" : "faster", std::fabs(k.maxDiff));
		row("Each car's speed", "as shown", b);
		ImGui::EndTable();
	}
	if (ImGui::TreeNode("The numbers")) {
		dimText("A calibration only changes the measured distance, so every speed moves by the same percentage. delta = newer zone / %s zone - 1. "
			"Two one-sided tests at 95%%: still accurate when |delta| + 1.645 sigma_new <= T, tainted when |delta| - 1.645 sigma_new > T, "
			"where T = 1.96 sigma is the band %s promised.", published ? "published" : "locked", published ? "the dataset" : "the set's calibration");
		for (int d = 0; d < 2; d++) {
			const dataset::Test& t = k.test[d];
			if (t.verdict == dataset::Verdict::None) continue;
			ImGui::Bullet(); ImGui::TextWrapped("%s: delta %s, 90%% interval %s to %s, T = %s, sigma_new %s: %s", d == 0 ? "Left to right" : "Right to left",
				signedPct(t.delta).c_str(), signedPct(t.lo).c_str(), signedPct(t.hi).c_str(), ("\xC2\xB1" + pct(t.tolerance)).c_str(),
				pct(t.sigmaNew).c_str(), dataset::verdictLabel(t.verdict));
		}
		ImGui::TreePop();
	}
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, px(6)));
	if (published) {
		if (primaryButton(ICON_CHECK "  Record this check")) {
			dataset::Check out;
			std::string err;
			if (dataset::check(target, calibrationNow(), calib::nowIso(), out, err)) {
				toast(std::string("Recorded: ") + dataset::verdictLabel(out.verdict) + ". Everyone opening it sees this.", verdictColor(out.verdict), verdictIcon(out.verdict));
				if (dataset_.dir == target.dir) dataset_ = target;
				ImGui::CloseCurrentPopup();
			}
			else toast(err, c::bad, ICON_WARN);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds this check to the published dataset: its badge changes, and every car's before and after\nspeed is saved beside it. The original numbers are never changed.");
		ImGui::SameLine();
	}
	else if (const calsets::Set* s = sets_.find(setForm_.setId)) {
		if (primaryButton(ICON_CHECK "  Use the newer calibration for this set")) {
			calsets::accept(sets_, s->id, currentView(), currentScale(), currentSigma());
			std::string err;
			if (!calsets::save(setsFor_, sets_, err)) toast(err, c::warn, ICON_WARN);
			applySets();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
	}
	if (ghostButton(published ? "Not now" : "Keep it as it is")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

bool App::publishCorrected(const dataset::Info& old, const dataset::Calibration& newer, std::string& dir, std::string& error) {
	// A new version beside the tainted one: the same cars at the newer calibration, pointing back at what it corrects.
	std::vector<double> after;
	dataset::evaluate(old, newer, calib::nowIso(), &after);
	dataset::Info d = old;
	d.dir.clear();
	d.checks.clear();
	d.version = old.version + 1;
	d.correctionOf = old.id;
	d.published = calib::nowIso();
	d.vst = VST_VERSION;
	d.cal = newer;
	d.name = old.name + " (corrected)";
	for (size_t i = 0; i < d.points.size(); i++) {
		dataset::Point& p = d.points[i];
		double f = p.speed > 0 ? after[i] / p.speed : 1;
		p.speed = int(after[i] + 0.5);
		if (!p.trend.empty()) {
			p.entryMph = int(p.entryMph * f + 0.5);
			p.exitMph = int(p.exitMph * f + 0.5);
			p.mphIn20ft = int(p.mphIn20ft * f + 0.5);
			p.accel = std::round(p.accel * f * 10) / 10;
		}
	}
	d.summary = dataset::summarize(d.points, d.speedLimit, d.wayOver);
	archive::Info a;
	if (!archive::read(old.dir, a, error)) return false;
	archive::Request req;
	req.dataDir = dataDir();
	req.root = "Published";
	req.move = false;
	req.checksums = true;
	req.info = a;
	req.info.name = d.name;
	req.info.created = d.published;
	req.info.from = "Correction of " + old.id;
	req.configFile = (fs::path(old.dir) / "VST.cfg").string();
	req.historyFiles = { (fs::path(old.dir) / "calibration-history.csv").string() };
	std::error_code ec;
	for (fs::directory_iterator it(fs::path(old.dir) / "Stats", ec), end; !ec && it != end; it.increment(ec))
		if (it->path().extension() == ".csv") req.statsCsvs.push_back(it->path().string());
	for (size_t i = 0; i < a.vehicles.size(); i++) {
		archive::Source s;
		s.row = a.vehicles[i];
		if (i < d.points.size()) s.row.speed = d.points[i].speed;
		if (!a.vehicles[i].snapshot.empty()) {
			// imdecode of the bytes rather than imread, which can't open non-ASCII paths on Windows.
			std::ifstream in(fs::path(old.dir) / fs::path(a.vehicles[i].snapshot), std::ios::binary);
			std::vector<uchar> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (!bytes.empty()) s.snapshot = cv::imdecode(bytes, cv::IMREAD_COLOR);
		}
		if (!a.vehicles[i].clip.empty()) s.clipPath = (fs::path(old.dir) / a.vehicles[i].clip).string();
		req.vehicles.push_back(s);
	}
	d.id = fs::path(archive::folderFor((fs::path(dataDir()) / "Published").string(), d.published, d.name)).filename().string();
	req.info.dataset = dataset::coverJson(d);
	req.extra = { { "dataset.json", dataset::datasetJson(d) }, { "points.csv", dataset::pointsCsv(d) } };
	if (!archive::write(req, dir, error)) return false;
	{
		std::ofstream st(fs::path(dir) / "status.json", std::ios::binary);
		st << dataset::statusJson(d);
	}
	if (calsets::Set* s = sets_.find(old.calibrationSet)) {
		s->published.push_back(fs::relative(fs::path(dir), fs::path(dataDir())).generic_string());
		std::string err;
		calsets::save(setsFor_, sets_, err);
	}
	return true;
}

// ---- the cover over an open published dataset -------------------------------------------------------------------------

void App::datasetCover(float width) {
	const dataset::Info& d = dataset_;
	dataset::Verdict v = d.status();
	beginCard("cover", nullptr, nullptr, ImVec2(width, 0));
	// Layer 1, for everyone: the badge, and one sentence.
	linePill((std::string(verdictIcon(v)) + "  " + dataset::verdictLabel(v)).c_str(), v == dataset::Verdict::None ? c::text : c::bg0, verdictColor(v), fonts.strong);
	ImGui::SameLine(0, px(12));
	ImGui::AlignTextToFramePadding();
	dimText("Published %s · version %d%s", d.published.substr(0, std::min<size_t>(10, d.published.size())).c_str(), d.version,
		d.correctionOf.empty() ? "" : (" · corrects " + d.correctionOf).c_str());
	ImGui::PushTextWrapPos(0);
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(dataset::sentence(d).c_str());
	ImGui::PopFont();
	// Layer 2: how we know.
	if (ImGui::TreeNode("How we know")) {
		dimText("The camera times each car between two lines a known distance apart on the street. That distance was set by calibration "
			"(%s). So speeds are accurate %s, with 95%% confidence.", d.cal.how.empty() ? "one calibration, method not recorded" : d.cal.how.c_str(),
			dataset::accuracyWords(d.cal, d.speedLimit).c_str());
		if (!(d.cal.sigma[0] > 0 && d.cal.sigma[1] > 0))
			dimText("How closely that calibration was measured isn't on record, so a cautious \xC2\xB1" "6%% (one standard deviation) is assumed; "
				"that is why the band is wide.");
		if (d.checks.empty()) dimText("No newer calibration has been compared with it yet.");
		for (const dataset::Check& k : d.checks) {
			ImGui::Bullet(); ImGui::TextWrapped("%s: %s. Cars over the limit %d -> %d, 85th percentile %d -> %d mph, average change %+.1f mph.",
				k.when.substr(0, std::min<size_t>(10, k.when.size())).c_str(), dataset::verdictLabel(k.verdict), k.before.over, k.after.over,
				k.before.p85, k.after.p85, k.meanDiff);
		}
		ImGui::TreePop();
	}
	// Layer 3: the numbers, files and checksums.
	if (ImGui::TreeNode("The numbers")) {
		auto sig = [](double x) { return x > 0 ? pct(x) : std::string("not measured (6% assumed)"); };   // as review.html shows it
		dimText("Speed = zone length / the car's time between the lines (zoneSeconds in points.csv). Zone %.2f ft left to right, %.2f ft right "
			"to left; relative 1-sigma %s and %s. Format %s (docs/dataset-schema.md).", d.cal.zoneFt[0], d.cal.zoneFt[1],
			sig(d.cal.sigma[0]).c_str(), sig(d.cal.sigma[1]).c_str(), d.schema.c_str());
		if (!d.checks.empty()) {
			const dataset::Check& k = d.checks.back();
			for (int i = 0; i < 2; i++) {
				const dataset::Test& t = k.test[i];
				if (t.verdict == dataset::Verdict::None) continue;
				ImGui::Bullet(); ImGui::TextWrapped("%s: delta %s, 90%% interval %s to %s, T = %s, sigma_new %s: %s", i == 0 ? "Left to right" : "Right to left",
					signedPct(t.delta).c_str(), signedPct(t.lo).c_str(), signedPct(t.hi).c_str(), ("\xC2\xB1" + pct(t.tolerance)).c_str(),
					pct(t.sigmaNew).c_str(), dataset::verdictLabel(t.verdict));
			}
		}
		if (datasetVerifiedFor_ != d.dir) {
			datasetVerifiedFor_ = d.dir;
			std::vector<std::string> changed;
			datasetVerified_ = !dataset::verify(d.dir, changed) ? "No SHA256SUMS in this folder."
				: changed.empty() ? "Checksums: every file matches SHA256SUMS, so nothing has been changed since it was published."
				: "Checksums: " + std::to_string(changed.size()) + " file(s) differ from SHA256SUMS (" + changed[0] + (changed.size() > 1 ? ", ..." : "")
					+ "). This copy has been changed since it was published.";
		}
		dimText("%s", datasetVerified_.c_str());
		if (ghostButton(ICON_FOLDER "  Show the files")) openWithSystem(d.dir);
		ImGui::TreePop();
	}
	ImGui::PopTextWrapPos();
	// What can be done: re-check on the same view when the calibration has moved; a corrected copy once tainted.
	const calsets::View now = currentView();
	dataset::Calibration nowCal = calibrationNow();
	bool sameView = d.view.same(now);
	bool moved = std::fabs(nowCal.zoneFt[0] - d.cal.zoneFt[0]) > 1e-6 * d.cal.zoneFt[0] || std::fabs(nowCal.zoneFt[1] - d.cal.zoneFt[1]) > 1e-6 * d.cal.zoneFt[1];
	if (sameView && moved) {
		if (ghostButton(ICON_GAUGE "  Check against your latest calibration")) openRecheck_ = true;
		ImGui::SameLine();
	}
	else if (!sameView) { dimText("Measured from another camera view than today's, so only a calibration made from that view can re-check it."); }
	if (v == dataset::Verdict::Tainted && !d.checks.empty()) {
		if (primaryButton(ICON_PUBLISH "  Publish a corrected copy")) {
			std::string dir, err;
			if (publishCorrected(d, d.checks.back().cal, dir, err)) {
				toast("Published a corrected copy beside the original, which stays as it was.", c::ok, ICON_CERTIFICATE);
				openArchive(dir);
				endCard();
				return;
			}
			toast(err, c::bad, ICON_WARN);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new version with every speed at the newer calibration. It names the dataset it corrects;\nthe original is kept, marked tainted.");
	}
	endCard();
}
