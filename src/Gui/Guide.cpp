// The first two steps of the setup flow (kFlow in App.cpp), for someone who has never set up a speed camera:
//   1 What you need      what each later step will need, one card per step in step order
//   2 Pick your spot     the site planner: the street in feet first, then the camera, which can stay a guess
//                        (a phone) until the user has one
// Steps 3-5 are the Connect the camera, Mark your street and Calibrate pages themselves; this file also draws the
// aiming card shown on Connect the camera.
// The planner's model is the one in the project's site calculator: view width at the lane from the field of
// view, limited by the clear stretch, minus a car length and a lock-in run-up gives the timing zone; frames in
// the zone give the one-frame timing error, and speed x shutter time gives motion blur.  The calibration error is the
// timing error of the calibration drive itself (at the speed limit), carried into every speed: VST learns the zone
// from that pass rather than from a measured length.
// A short view (an old-city street seen across 14 to 25 ft or so) uses the engine's short-view timing (subFrameTiming = 1)
// instead.  That times the front bumper with a line fitted through its positions, so a whole car never has to fit
// in the zone: the zone is the view less about a foot at each end, the limit is how many bumper positions the fit
// gets (one per frame pair, at least kMinSamples), and the timing error comes from the bumper's pixel jitter
// spread over the zone rather than from a whole frame.  Both numbers are from the simulations in
// tests/short_view.cmake (p90 error 1 mph at 30 fps on 15 and 20 ft crops, fits rejected below 3 points).
// The fastest car a short view can time follows from kMinSamples: it needs 2 (kMinSamples - 1) frames in the zone.
// Halving the frame rate of the 15 ft crop (the same as doubling every car's speed at 30 fps) bore that out: cars up
// to about 45 mph at 30 fps were timed, half of those at 50-59 mph, few above 60, and the ones timed stayed within
// 2 mph.  The model's 44 mph at 30 fps for 15 ft is the planner's number.
#include "App.h"
#include "Canvas.h"
#include "Icons.h"
#include "Theme.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <string>

using namespace theme;

namespace {

struct Preset { const char* name; double hfov, width, fps, shutter; };
const Preset kPresets[] = {
	{ "Phone as a USB webcam (1080p, 30 fps)", 66, 1920, 30, 120 },
	{ "USB webcam, e.g. Logitech C920 (1080p, 30 fps)", 70, 1920, 30, 250 },
	{ "PoE IP camera, 2.8 mm lens (1080p, 30 fps)", 100, 1920, 30, 250 },
	{ "PoE IP camera, 4 mm lens (1080p, 30 fps)", 80, 1920, 30, 250 },
	// Raspberry Pi cameras at VST's defaults (PiCamera.h): 1280x720 and a 1/1000 s shutter.  At that size Camera Module 3
	// and v2 run a sensor mode that is a centre crop (3072 of 4608 and 1920 of 3280 pixels across), so their view is
	// narrower than the lens's full 66 / 102 / 62 degrees.  The Global Shutter Camera uses its whole sensor.
	{ "Raspberry Pi Global Shutter + 6 mm lens (60 fps)", 45, 1440, 60, 1000 },
	{ "Raspberry Pi Camera Module 3 (720p, 60 fps)", 47, 1280, 60, 1000 },
	{ "Raspberry Pi Camera Module 3 Wide (720p, 60 fps)", 79, 1280, 60, 1000 },
	{ "Raspberry Pi Camera Module v2 (720p, 47 fps)", 39, 1280, 47, 1000 },
	{ "720p camera at 60 fps, fixed shutter (short views)", 66, 1280, 60, 500 },
	{ "Original VST camera: Foscam 720p, 30 fps", 70, 1280, 30, 250 },
	{ "Custom", 0, 0, 0, 0 },
};

// The model is plan:: (Plan.h), shared with the companion API; these are the names this file has always used.
using plan::kMph;
using plan::kMinSamples;
using plan::kMaxTrackMph;
using plan::grade;
using Eval = plan::Eval;

plan::Inputs inputs(const AppPrefs& p) {
	plan::Inputs in;
	in.distM = p.planDist; in.clearM = p.planClear; in.carLenM = p.planCarLen;
	in.hfovDeg = p.planHfov; in.widthPx = p.planWidth; in.fps = p.planFps; in.shutter = p.planShutter;
	in.speedLimit = p.speedLimit;
	return in;
}

Eval evaluate(const AppPrefs& p, double mph, bool shortView) { return plan::evaluate(inputs(p), mph, shortView); }

ImU32 gradeColor(int g) { return g == 0 ? c::ok : g == 1 ? c::warn : c::bad; }

void bullet(const char* icon, ImU32 col, const char* head, const char* text) {
	// The text column is measured from where the bullet starts (SameLine(x) would be from the window's edge, which
	// put the text on top of the icon in cards that aren't inside a group).
	float x0 = ImGui::GetCursorScreenPos().x;
	ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
	ImGui::TextUnformatted(icon);
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::SetCursorScreenPos(ImVec2(x0 + px(34), ImGui::GetCursorScreenPos().y));
	ImGui::BeginGroup();
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(head);
	ImGui::PopFont();
	ImGui::PushTextWrapPos(0);
	dimText("%s", text);
	ImGui::PopTextWrapPos();
	ImGui::EndGroup();
	ImGui::Dummy(ImVec2(0, px(4)));
}

// A card with a big icon, a title, a price/kind line and a sentence: one piece of equipment.
void gearCard(const char* id, const char* icon, ImU32 col, const char* title, const char* tag, const char* text, ImVec2 size) {
	beginCard(id, nullptr, nullptr, size);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, p + ImVec2(px(44), px(44)), alpha(col, .14f), px(10));
	ImVec2 is = fonts.title->CalcTextSizeA(fsz(fonts.title), FLT_MAX, 0, icon);
	dl->AddText(fonts.title, fsz(fonts.title), p + (ImVec2(px(44), px(44)) - is) * .5f, col, icon);
	ImGui::Dummy(ImVec2(px(44), px(48)));
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(title);
	ImGui::PopFont();
	pill(tag, col, alpha(col, .14f));
	ImGui::PushTextWrapPos(0);
	dimText("%s", text);
	ImGui::PopTextWrapPos();
	endCard();
}

// Sketch of a good camera placement: window above the street, view square-on, street across the middle.
void aimSketch(ImVec2 size) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, p + size, c::rgb(0x0E121A), px(10));
	// The frame as the camera sees it.
	ImVec2 f0 = p + ImVec2(size.x * .08f, size.y * .1f), f1 = p + ImVec2(size.x * .92f, size.y * .9f);
	dl->AddRectFilled(f0, f1, c::rgb(0x1A2232), px(6));
	float sky = f0.y + (f1.y - f0.y) * .3f;
	dl->AddRectFilled(f0, ImVec2(f1.x, sky), c::rgb(0x1E2A3F), px(6), ImDrawFlags_RoundCornersTop);
	// Houses across the street.
	for (int i = 0; i < 4; i++) {
		float x = f0.x + (f1.x - f0.x) * (.06f + i * .24f);
		dl->AddRectFilled(ImVec2(x, sky - px(26)), ImVec2(x + px(54), sky + px(14)), c::rgb(0x2A3448), px(2));
		dl->AddTriangleFilled(ImVec2(x - px(4), sky - px(26)), ImVec2(x + px(58), sky - px(26)), ImVec2(x + px(27), sky - px(44)), c::rgb(0x323D54));
	}
	// Road across the middle of the frame, two lanes.
	float r0 = f0.y + (f1.y - f0.y) * .42f, r1 = f0.y + (f1.y - f0.y) * .7f;
	dl->AddRectFilled(ImVec2(f0.x, r0), ImVec2(f1.x, r1), c::rgb(0x2B2F38));
	for (float x = f0.x + px(8); x < f1.x; x += px(36)) dl->AddLine(ImVec2(x, (r0 + r1) * .5f), ImVec2(x + px(18), (r0 + r1) * .5f), c::rgb(0xC9A227), px(2));
	// Cars.
	auto car = [&](float x, float y, ImU32 col) {
		dl->AddRectFilled(ImVec2(x, y - px(12)), ImVec2(x + px(56), y + px(6)), col, px(5));
		dl->AddRectFilled(ImVec2(x + px(12), y - px(22)), ImVec2(x + px(42), y - px(10)), alpha(col, .8f), px(4));
		dl->AddCircleFilled(ImVec2(x + px(12), y + px(6)), px(6), c::rgb(0x111111));
		dl->AddCircleFilled(ImVec2(x + px(44), y + px(6)), px(6), c::rgb(0x111111));
	};
	car(f0.x + (f1.x - f0.x) * .58f, r0 + (r1 - r0) * .36f, c::r2l);
	car(f0.x + (f1.x - f0.x) * .2f, r0 + (r1 - r0) * .86f, c::l2r);
	// Overlay: analysis box, speed lines, centre guides.
	ImVec2 a0(f0.x + px(8), r0 - px(30)), a1(f1.x - px(8), r1 + px(6));
	dl->AddRect(a0, a1, c::accent2, px(4), 0, 2);
	float sl = f0.x + (f1.x - f0.x) * .3f, sr = f0.x + (f1.x - f0.x) * .72f;
	dl->AddLine(ImVec2(sl, a0.y), ImVec2(sl, a1.y), c::text, 2);
	dl->AddLine(ImVec2(sr, a0.y), ImVec2(sr, a1.y), c::text, 2);
	for (float y = f0.y; y < f1.y; y += px(10)) dl->AddLine(ImVec2((f0.x + f1.x) * .5f, y), ImVec2((f0.x + f1.x) * .5f, y + px(4)), alpha(c::text, .25f));
	dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(f0.x + px(10), f1.y - px(22)), c::dim, "street across the middle, roughly square-on, 14 ft or more in view");
	ImGui::Dummy(size);
}

// Top-down sketch to scale: camera at the bottom, lane across the top, field-of-view cone, clear stretch and zone.
void planDiagram(const AppPrefs& pr, const Eval& r, const Globals& g, ImVec2 size) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, p + size, c::rgb(0x0E121A), px(10));
	float laneY = p.y + size.y * .16f, laneH = size.y * .22f, camY = p.y + size.y * .88f;
	double span = std::max({ r.cover, pr.planClear, 10.0 }) * 1.15;
	float sx = float(size.x / span), cx = p.x + size.x * .5f;
	float half = float(r.cover / 2 * sx), clearHalf = float(pr.planClear / 2 * sx), zoneHalf = float(r.zone / 2 * sx);
	float laneMid = laneY + laneH * .5f;
	dl->AddRectFilled(ImVec2(p.x, laneY), ImVec2(p.x + size.x, laneY + laneH), c::rgb(0x2B2F38));
	dl->AddTriangleFilled(ImVec2(cx, camY), ImVec2(cx - half, laneMid), ImVec2(cx + half, laneMid), alpha(c::accent, .14f));
	dl->AddLine(ImVec2(cx, camY), ImVec2(cx - half, laneMid), alpha(c::accent, .7f), 1.5f);
	dl->AddLine(ImVec2(cx, camY), ImVec2(cx + half, laneMid), alpha(c::accent, .7f), 1.5f);
	dl->AddRect(ImVec2(cx - clearHalf, laneY + px(4)), ImVec2(cx + clearHalf, laneY + laneH - px(4)), alpha(c::text, .4f), px(3), 0, 1.f);
	if (r.zone > 0) {
		dl->AddRectFilled(ImVec2(cx - zoneHalf, laneY + px(2)), ImVec2(cx + zoneHalf, laneY + laneH - px(2)), alpha(c::warn, .18f));
		dl->AddLine(ImVec2(cx - zoneHalf, laneY + px(2)), ImVec2(cx - zoneHalf, laneY + laneH - px(2)), c::warn, px(3));
		dl->AddLine(ImVec2(cx + zoneHalf, laneY + px(2)), ImVec2(cx + zoneHalf, laneY + laneH - px(2)), c::warn, px(3));
	}
	char t[160];
	snprintf(t, sizeof t, "zone %.0f ft   ·   clear %.0f ft   ·   in frame %.0f ft", r.zone * 3.281, pr.planClear * 3.281, r.cover * 3.281);
	ImVec2 ts = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, t);
	dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(cx - ts.x * .5f, laneY - ts.y - px(4)), c::text, t);
	// One-way callout on the lane, using the street's configured legal direction.
	if (g.LegalDirection != "Both") {
		bool legalL2R = g.LegalDirection == "L2R";
		std::string legal = std::string(legalL2R ? "" : "← ") + "legal: " + (legalL2R ? g.L2RDirection : g.R2LDirection) + (legalL2R ? " →" : "");
		std::string wrong = legalL2R ? "← wrong way" : "wrong way →";
		ImVec2 ls = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, legal.c_str());
		ImVec2 ws = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, wrong.c_str());
		float ty = laneMid - ls.y * .5f;
		dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(legalL2R ? p.x + size.x - ls.x - px(10) : p.x + px(10), ty), c::ok, legal.c_str());
		dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(legalL2R ? p.x + px(10) : p.x + size.x - ws.x - px(10), ty), c::wrong, wrong.c_str());
	}
	dl->AddCircleFilled(ImVec2(cx, camY), px(7), c::warn);
	snprintf(t, sizeof t, ICON_CAMERA "  camera, %.0f ft from the lane", pr.planDist * 3.281);
	dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(cx + px(14), camY - px(8)), c::dim, t);
	ImGui::Dummy(size);
}

}  // namespace

// Under the planner's verdict on a short view: what short-view timing is, a button that sets it up from the planner's
// numbers, and the things that go wrong on a narrow street with parked cars.
void App::shortViewNote(double coverM, bool on) {
	ImGui::PushTextWrapPos(0);
	if (on) dimText(ICON_CHECK "  Short-view timing is on: VST times each car's front bumper to a fraction of a frame, so it doesn't need a whole car in view.");
	else {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::accent));
		ImGui::TextWrapped(ICON_INFO "  A short view like this is what short-view timing is for: it times each car's front bumper to a fraction of a frame. One click sets it up.");
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	if (ghostButton(on ? ICON_SLIDERS "  Re-fit the short-view settings to this plan" : ICON_SLIDERS "  Use short-view timing")) {
		// The tracker works in a 1280-wide frame: pixels a car moves in two frames at the fastest speed it tracks
		// sets its look-ahead (and the margins scaled from it), and a car length sets how far back it looks on entry.
		Globals& g = cfg.g;
		double pxPerM = 1280 / std::max(coverM, .5);
		int step = int(std::ceil(pxPerM * kMaxTrackMph * kMph * 2 / std::max(prefs.planFps, 1.0)));
		g.subFrameTiming = 1;
		g.maxL2RDistOnEntry = g.maxR2LDistOnEntry = std::clamp(step, 75, 1280);
		g.entryLookBack = std::clamp(int(pxPerM * prefs.planCarLen), 250, g.AnalysisBoxWidth);
		// Speed lines about 7% in from each side of the analysis box, so the bumper is seen before and after the zone.
		g.speedLineLeft = int(g.AnalysisBoxWidth * .07);
		g.speedLineRight = g.AnalysisBoxWidth - g.speedLineLeft;
		toast("Short-view timing is on. Check the speed lines in step 4, Mark your street, then calibrate.", c::ok, ICON_CHECK);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Turns on subFrameTiming and sets, from this plan:\n  the look-ahead (max step) for cars up to %.0f mph at %.0f fps\n  the entry look-back to a car length\n  the speed lines 7%% in from each side of the analysis box", kMaxTrackMph, prefs.planFps);
	if (ImGui::TreeNode("Narrow streets: parked cars, shadows and wrong-way drivers")) {
		ImGui::PushTextWrapPos(0);
		bullet(ICON_CAR, c::accent, "Keep parked cars out of the box",
			"Draw the analysis box on the moving lane only: from just above the moving cars' roofs to just below their wheels. A parked car "
			"behind them doesn't move, so it isn't tracked, but a box that takes in the parking lane catches doors, people and parking moves.");
		bullet(ICON_WAVE, c::warn, "Tree shadows",
			"Shadows moving in the wind look like motion. Keep them out of the box where you can, or raise Sensitivity a little on sunny days. "
			"A fixed shutter (not auto) stops the picture brightening and dimming as cars pass.");
		bullet(ICON_BAN, c::wrong, "Wrong-way drivers",
			"On a one-way street set the legal direction in step 4, Mark your street. Every car going the other way is logged and flagged as wrong-way, "
			"timed like any other.");
		bullet(ICON_CAMERA, c::ok, "Through a window",
			"Take out any insect screen, and put the lens against the glass with a rubber hood so the room doesn't reflect. "
			"At 30 fps a 15 ft view times cars up to about 45 mph and a 20 ft view up to about 60; at 60 fps both time cars over 70. Use 60 fps and a 1/1000 s shutter if you can.");
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}
}

// ---- the street: a rough guess first, refined as setup goes on ------------------------------------------------

plan::Inputs App::planInputs() const { return inputs(prefs); }

void App::setPlanStreet(double distM, double clearM, plan::Source source, const std::string& note) {
	prefs.planDist = std::max(distM, 1.0);
	prefs.planClear = std::max(clearM, 1.0);
	prefs.planSource = int(source);
	prefs.planNote = note;
}

bool App::planPictureAvailable() {
	if (sampleCache_.empty()) sampleCache_ = findSample();
	// The documentation tour only has the sample, and shows this card on it.
	return player.isOpen() && !source.empty() && (source != sampleCache_ || screenshotMode);
}

// A calibration knows the speed zone in feet (how long a car at CalibrationSpeed took to cross it) and the speed lines
// say how many pixels that is, which scales the whole picture (plan::fromCalibration).  On a one-way street the legal
// direction's lane is the one traffic uses; on a two-way street the two lanes sit at different depths, so the
// planner takes the mean of the two, which is about the middle of the road.
bool App::planFromCalibration(plan::Measured& m, std::string& how) {
	if (sampleCache_.empty()) sampleCache_ = findSample();
	if (cfg.path().empty() || prefs.calibratedFor != cfg.path() || source == sampleCache_ || !cfg.problems().empty()) return false;
	const Globals& g = cfg.g;
	double frames = g.LegalDirection == "L2R" ? g.CalibrationFramesL2R : g.LegalDirection == "R2L" ? g.CalibrationFramesR2L
		: (g.CalibrationFramesL2R + g.CalibrationFramesR2L) / 2.0;
	double obstruction = std::max(0, g.obstruction[1] - g.obstruction[0]);
	m = plan::fromCalibration(frames, g.CalibrationSpeed, g.CalibrationFPS, g.speedLineRight - g.speedLineLeft, g.AnalysisBoxWidth, obstruction);
	if (!m.valid) return false;
	// When: the latest calibration on record for this config (the date is what a reader wants; the method isn't kept).
	how = "your step 5 calibration";
	for (auto it = calibrationHistory().rbegin(); it != calibrationHistory().rend(); ++it)
		if (!it->check) { how += ", " + it->time.substr(0, 10); break; }
	return true;
}

void App::syncPlanFromCalibration(bool announce) {
	plan::Measured m;
	std::string how;
	if (!planFromCalibration(m, how)) {
		// Right after a calibration, say why step 2 didn't follow rather than leave it silently unchanged.
		if (announce && prefs.calibratedFor == cfg.path() && !cfg.path().empty()) {
			if (source == sampleCache_) toast("Step 2 keeps your own street's numbers: this calibration was on the sample video.", c::dim, ICON_INFO);
			else if (!cfg.problems().empty()) toast("Step 2 will update from this calibration once the problems on Mark your street are fixed.", c::warn, ICON_WARN);
			else toast("This calibration doesn't look like a street (under 3 ft or over 1000 ft across), so step 2 is unchanged. Check the speed you entered.", c::warn, ICON_WARN);
		}
		return;
	}
	// The calibration's identity: a new one (or new speed lines, box or direction) measures again; the same one
	// doesn't, so street numbers typed after it stay.
	const Globals& g = cfg.g;
	char sig[256];
	snprintf(sig, sizeof sig, "%s|%d|%d|%g|%g|%d|%d|%d|%d|%d|%s", cfg.path().c_str(), g.CalibrationFramesL2R, g.CalibrationFramesR2L,
		g.CalibrationSpeed, g.CalibrationFPS, g.speedLineLeft, g.speedLineRight, g.AnalysisBoxWidth, g.obstruction[0], g.obstruction[1],
		g.LegalDirection.c_str());
	if (prefs.planCalSig == sig) return;
	prefs.planCalSig = sig;
	double wasFt = plan::coverAt(prefs.planDist, prefs.planHfov) * plan::kFt;
	plan::Inputs in = planInputs();
	plan::applyMeasured(in, m);
	setPlanStreet(in.distM, in.clearM, plan::Source::Calibration, "Measured by " + how);
	if (announce) {
		char t[320];
		snprintf(t, sizeof t, "Step 2 updated: the calibration measured about %.0f ft of street across the picture (the plan had %.0f ft), "
			"so the camera is about %.0f ft from the lane with your lens.", m.coverFt, wasFt, in.distM * plan::kFt);
		toast(t, c::accent2, ICON_RULER);
	}
}

std::string App::frameRateAdvice(double fps) {
	char b[480];
	plan::Inputs in = planInputs();
	in.fps = fps;
	plan::Verdict v = plan::verdict(in, cfg.g.subFrameTiming != 0);
	std::string reach = v.shortView ? "On the view you planned in step 2 it times cars " + plan::fastestText(v.fastestMph) + "." : "";
	if (fps >= 24) {
		snprintf(b, sizeof b, "%.0f fps is good for timing. %s", fps, reach.c_str());
		return b;
	}
	snprintf(b, sizeof b, "%.0f fps is fine to start: everyday speeds still time. %s Most webcams and phone apps drop to 15 fps in dim light or "
		"on \"auto\": pick 30 or 60 fps in the camera's settings or the phone app, and give it daylight or a lamp off the window.",
		fps, reach.c_str());
	return b;
}

// Under "Your street": the ways to turn a rough guess into real numbers, in the order setup reaches them.
void App::zeroInCard(float width) {
	if (showZeroIn_) { ImGui::SetScrollHereY(0); showZeroIn_ = false; }
	beginCard("zeroin", ICON_BULLSEYE, "Zero in on it", ImVec2(width, 0));
	ImGui::PushTextWrapPos(0);
	dimText("Start rough. These firm the numbers up as you go, no tape measure needed.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, px(4)));

	// 1. From the picture: how many car lengths fit across it.  A ruler of that many blocks is drawn over the picture
	// at lane height, so matching one block to a car is the whole job.
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(ICON_CAMERA "  From your camera's picture");
	ImGui::PopFont();
	if (planPictureAvailable()) {
		float w = ImGui::GetContentRegionAvail().x;
		View v = drawFrame(player.texture().imguiId(), ImVec2(w, w * 9 / 16));
		if (prefs.planCarLengths <= 0) {
			// Start the ruler at what the plan says now, so a good guess needs no change.
			prefs.planCarLengths = std::clamp(plan::coverAt(prefs.planDist, prefs.planHfov) * plan::kFt / plan::kTypicalCarFt, 1.0, 20.0);
		}
		const Globals& g = cfg.g;
		bool boxed = zonesDone();
		float y = boxed ? float(g.AnalysisBoxTop + g.AnalysisBoxHeight / 2) : 720 * .55f;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		double blockPx = plan::kFrameWidthPx / prefs.planCarLengths;
		for (int i = 0; i * blockPx < plan::kFrameWidthPx; i++) {
			float x0 = float(i * blockPx), x1 = float(std::min((i + 1) * blockPx, plan::kFrameWidthPx));
			ImU32 col = i % 2 ? c::accent2 : c::warn;
			dl->AddRectFilled(v.at(x0, y - 16), v.at(x1, y + 16), alpha(col, .5f));
			dl->AddLine(v.at(x0, y - 40), v.at(x0, y + 40), col, px(2));
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		float n = float(prefs.planCarLengths);
		if (ImGui::SliderFloat("##carlengths", &n, 1, 20, "%.1f car lengths across", ImGuiSliderFlags_Logarithmic)) prefs.planCarLengths = n;
		ImGui::PushTextWrapPos(0);
		dimText("Slide until one block is as long as a car in the picture, ideally one driving past (a car parked on the far side reads a little short).");
		ImGui::PopTextWrapPos();
		plan::Measured m = plan::fromCarLengths(prefs.planCarLengths, boxed ? g.AnalysisBoxWidth : 0,
			boxed ? std::max(0, g.obstruction[1] - g.obstruction[0]) : 0);
		char b[96];
		snprintf(b, sizeof b, ICON_CHECK "  Use it: about %.0f ft across", m.coverFt);
		if (ghostButton(b, ImVec2(-FLT_MIN, 0)) && m.valid) {
			plan::Inputs in = planInputs();
			plan::applyMeasured(in, m);
			char note[96];
			snprintf(note, sizeof note, "%.1f car lengths across the picture", prefs.planCarLengths);
			setPlanStreet(in.distM, in.clearM, plan::Source::Picture, note);
			toast("Planned from your picture. A calibration in step 5 will measure it exactly.", c::ok, ICON_CHECK);
		}
	}
	else {
		ImGui::PushTextWrapPos(0);
		dimText("Connect your camera in step 3 (or open a photo of the view) and this sizes the street from the picture in a few seconds.");
		ImGui::PopTextWrapPos();
		if (ghostButton(ICON_CAMERA "  Go to step 3, Connect the camera")) goFlow(2);
	}

	// 2. A calibration measures it outright, whichever way it was done.
	ImGui::Dummy(ImVec2(0, px(8)));
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(ICON_RULER "  From your calibration");
	ImGui::PopFont();
	plan::Measured m;
	std::string how;
	ImGui::PushTextWrapPos(0);
	if (planFromCalibration(m, how)) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok));
		ImGui::TextWrapped(ICON_CHECK_CIRCLE "  Measured: %.0f ft across the picture, %.0f ft of it clear (%s).", m.coverFt, m.clearFt, how.c_str());
		ImGui::PopStyleColor();
		if (prefs.planSource != int(plan::Source::Calibration) && ghostButton(ICON_UNDO "  Use the measured numbers again")) {
			prefs.planCalSig.clear();
			syncPlanFromCalibration(false);
		}
	}
	else {
		dimText("Any way you calibrate in step 5 measures the street, and this page updates by itself: a drive past, VST Ride-along "
			"on a phone (a bike at 12-15 mph works too), a Tesla clip, a car of known length, or two cones or a pole.");
	}
	ImGui::PopTextWrapPos();
	endCard();
}

void App::sitePlanner() {
	syncPlanFromCalibration();
	float avail = ImGui::GetContentRegionAvail().x;
	float inW = px(360);
	float outW = avail - inW - px(20);

	// Street first: it's what the user knows from their window, roughly.  A preset is a fine answer on day one; the
	// numbers are there for anyone who has measured, and "Zero in" below firms them up.  The camera comes second and
	// starts as a guess (a phone, the commonest first camera), so the planner answers "is my window good enough?"
	// before anything is bought.  Prefs stay in metres (the model's units); the street is entered in feet.
	constexpr double kFt = plan::kFt;
	ImGui::BeginGroup();
	beginCard("planin", ICON_SLIDERS, "Your street", ImVec2(inW, 0));
	const float col = px(190);
	int n = int(IM_ARRAYSIZE(kPresets));
	plan::Source src = plan::Source(std::clamp(prefs.planSource, 0, 3));
	ImGui::PushTextWrapPos(0);
	dimText("Which is closest to your view? A rough guess is fine to start.");
	ImGui::PopTextWrapPos();
	for (const plan::StreetPreset& sp : plan::kStreetPresets) {
		bool on = src == plan::Source::Guess && std::fabs(prefs.planDist * kFt - sp.distFt) < .5 && std::fabs(prefs.planClear * kFt - sp.clearFt) < .5;
		ImGui::PushID(sp.key);
		ImGui::PushStyleColor(ImGuiCol_Header, v4(alpha(c::accent, .22f)));
		std::string label = std::string(on ? ICON_CHECK_CIRCLE "  " : ICON_CIRCLE "  ") + sp.name;
		if (ImGui::Selectable(label.c_str(), on, 0, ImVec2(0, px(26))))
			setPlanStreet(sp.distFt / kFt, sp.clearFt / kFt, plan::Source::Guess, sp.name);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", sp.hint);
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0, px(4)));
	auto num = [&](const char* label, double* v, double step, const char* fmt, const char* help, bool camera) {
		formLabel(label, col);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));
		ImGui::PushID(label);
		bool changed = ImGui::InputDouble("##v", v, step, step * 5, fmt);
		if (changed && camera) prefs.planPreset = n - 1;
		ImGui::PopID();
		if (help) helpMarker(help);
		return changed;
	};
	// The same, shown and typed in another unit (feet, inches) while the pref keeps metres.
	auto numIn = [&](const char* label, double* metres, double scale, double step, const char* fmt, const char* help) {
		double shown = *metres * scale;
		num(label, &shown, step, fmt, help, false);
		if (std::fabs(shown - *metres * scale) > 1e-9) { *metres = shown / scale; return true; }
		return false;
	};
	bool typed = numIn("Window to the lane (ft)", &prefs.planDist, kFt, 1, "%.0f", "Straight-line distance from where the camera will sit to the middle of the nearer moving lane.\nNo tape measure? See \"Quick ways to estimate\" below.");
	typed |= numIn("Clear stretch (ft)", &prefs.planClear, kFt, 1, "%.0f", "The longest stretch of the moving lane you can see with nothing in the way: no trees, poles or parked trucks. A narrow old-city street seen from a front window is often only 14-25 ft; that works with short-view timing, down to about 14 ft.");
	if (typed) { prefs.planSource = int(plan::Source::Typed); prefs.planNote.clear(); }
	formLabel("Speed limit (mph)", col);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));   // lines up with the inputs above
	ImGui::InputInt("##lim", &prefs.speedLimit, 5);
	// Where the numbers came from, in the source's colour: amber while it's a guess, green once measured.
	src = plan::Source(std::clamp(prefs.planSource, 0, 3));
	ImU32 srcCol = src == plan::Source::Calibration ? c::ok : src == plan::Source::Guess ? c::warn : c::accent2;
	ImGui::Dummy(ImVec2(0, px(2)));
	pill(plan::sourceName(src), c::bg0, srcCol);
	ImGui::SameLine();
	ImGui::PushTextWrapPos(0);
	if (src == plan::Source::Guess) dimText("Zero in below whenever you're ready.");
	else if (!prefs.planNote.empty()) dimText("%s", prefs.planNote.c_str());
	else dimText("As you typed them.");
	ImGui::PopTextWrapPos();
	if (ImGui::TreeNode("Quick ways to estimate")) {
		ImGui::PushTextWrapPos(0);
		bullet(ICON_ROAD, c::accent2, "Pace it", "One easy step is about 2.5 ft. Pace from the house to the curb, then add half the street.");
		bullet(ICON_RULER, c::accent2, "Count what's there", "Sidewalk slabs are usually 5 ft, a parked car about 15 ft, a parking space about 20 ft.");
		bullet(ICON_LOCATE, c::accent2, "On a map", "Most map apps measure distance: right-click (or long-press) your window, then the middle of the lane.");
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}

	ImGui::Dummy(ImVec2(0, px(8)));
	sectionLabel("CAMERA");
	ImGui::PushTextWrapPos(0);
	dimText("No camera yet? Leave this on a phone. You'll connect the real one in step 3, and can come back to check it.");
	ImGui::PopTextWrapPos();
	ImGui::SetNextItemWidth(-FLT_MIN);
	prefs.planPreset = std::clamp(prefs.planPreset, 0, n - 1);
	// Presets are saved by position, and new ones get inserted (the Pi cameras were); a saved choice whose numbers no
	// longer match the preset now at that position is shown as Custom, keeping the numbers the user had.
	const Preset& chosen = kPresets[prefs.planPreset];
	if (chosen.hfov > 0 && (chosen.hfov != prefs.planHfov || chosen.width != prefs.planWidth || chosen.fps != prefs.planFps
		|| chosen.shutter != prefs.planShutter))
		prefs.planPreset = n - 1;
	if (ImGui::BeginCombo("##preset", kPresets[prefs.planPreset].name)) {
		for (int i = 0; i < n; i++)
			if (ImGui::Selectable(kPresets[i].name, prefs.planPreset == i)) {
				prefs.planPreset = i;
				if (kPresets[i].hfov > 0) {
					prefs.planHfov = kPresets[i].hfov; prefs.planWidth = kPresets[i].width;
					prefs.planFps = kPresets[i].fps; prefs.planShutter = kPresets[i].shutter;
				}
			}
		ImGui::EndCombo();
	}
	// A connected camera knows its own size and frame rate; planning with them shows what a 15 fps webcam means.
	if (planPictureAvailable() && player.info().kind != SourceInfo::Image && player.info().fps > 0
		&& (std::fabs(player.info().fps - prefs.planFps) > .5 || player.info().width != int(prefs.planWidth))) {
		char b[120];
		snprintf(b, sizeof b, ICON_CAMERA "  Plan with your camera: %d × %d, %.0f fps", player.info().width, player.info().height, player.info().fps);
		if (ghostButton(b, ImVec2(-FLT_MIN, 0))) {
			prefs.planWidth = player.info().width;
			prefs.planFps = std::round(player.info().fps);
			prefs.planPreset = n - 1;
		}
	}
	if (prefs.planFps < 24) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::PushTextWrapPos(0);
		ImGui::TextWrapped(ICON_INFO "  %s", frameRateAdvice(prefs.planFps).c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}
	if (ImGui::TreeNode("Camera details")) {
		num("Field of view (°)", &prefs.planHfov, 1, "%.0f", "Horizontal angle the camera sees. Phones and webcams: 60-80°. Wide IP cameras: 90-110°.", true);
		num("Frame width (px)", &prefs.planWidth, 10, "%.0f", nullptr, true);
		num("Frame rate (fps)", &prefs.planFps, 1, "%.0f", nullptr, true);
		num("Shutter (1/x s)", &prefs.planShutter, 10, "%.0f", "Faster shutters freeze motion. Phones choose this themselves and can drop to 1/60 in shade.", true);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Fine-tuning")) {
		numIn("Car length (ft)", &prefs.planCarLen, kFt, .5, "%.1f", "A typical car is 15-16 ft. Whole-frame timing needs one fully in the zone; short-view timing doesn't.");
		ImGui::TreePop();
	}
	prefs.planHfov = std::clamp(prefs.planHfov, 5.0, 170.0);
	prefs.planFps = std::clamp(prefs.planFps, 5.0, 240.0);
	prefs.planShutter = std::max(prefs.planShutter, 15.0);
	prefs.planDist = std::max(prefs.planDist, 1.0);
	prefs.planClear = std::max(prefs.planClear, 1.0);
	prefs.planWidth = std::max(prefs.planWidth, 320.0);
	prefs.speedLimit = std::clamp(prefs.speedLimit, 5, 80);
	endCard();
	ImGui::Dummy(ImVec2(0, px(2)));
	zeroInCard(inW);
	ImGui::EndGroup();

	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	bool shortOn = cfg.g.subFrameTiming != 0;
	plan::Verdict pv = plan::verdict(planInputs(), shortOn);
	int check = pv.checkMph;
	bool shortView = pv.shortView;
	const Eval& r = pv.at;
	int verdict = pv.grade;
	beginCard("planout", nullptr, nullptr, ImVec2(outW, 0));
	linePill(plan::gradeName(verdict), c::bg0, gradeColor(verdict), fonts.strong);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(plan::verdictText(pv).c_str());
	// How firm that is: a verdict on a guessed street says so, and says it's fine.
	if (src == plan::Source::Guess) {
		ImGui::PushTextWrapPos(0);
		dimText(ICON_INFO "  Based on a rough guess at your street. That's enough to choose a camera; it firms up as you zero in.");
		ImGui::PopTextWrapPos();
	}
	if (shortView && r.zone > 0) {
		// The fastest car this view can time, and what 60 fps would make of it: the spec a short view is bought on.
		ImGui::PushTextWrapPos(0);
		if (prefs.planFps < 59) dimText(ICON_GAUGE "  At %.0f fps this view times cars %s. A 60 fps camera would time them %s.",
			prefs.planFps, plan::fastestText(pv.fastestMph).c_str(), plan::fastestText(pv.fastest60).c_str());
		else dimText(ICON_GAUGE "  At %.0f fps this view times cars %s.", prefs.planFps, plan::fastestText(pv.fastestMph).c_str());
		ImGui::PopTextWrapPos();
	}
	if (shortView) shortViewNote(r.cover, shortOn);
	planDiagram(prefs, r, cfg.g, ImVec2(ImGui::GetContentRegionAvail().x, px(230)));
	ImGui::Dummy(ImVec2(0, px(4)));
	float tw = (ImGui::GetContentRegionAvail().x - px(30)) / 4, th = px(96);
	char v[48], sub[64];
	if (shortView) {
		snprintf(v, sizeof v, "%.0f", r.samples);
		snprintf(sub, sizeof sub, "at %d mph · %.0f+ needed, 6+ good", check, kMinSamples);
		statTile(ICON_FILM, "Bumper positions", v, sub, gradeColor(grade(-r.samples, -6, -kMinSamples)), ImVec2(tw, th));
	}
	else {
		snprintf(v, sizeof v, "%.0f", r.frames);
		snprintf(sub, sizeof sub, "at %d mph · VST likes 30+", check);
		statTile(ICON_FILM, "Frames in zone", v, sub, gradeColor(grade(-r.frames, -30, -15)), ImVec2(tw, th));
	}
	ImGui::SameLine(0, px(10));
	snprintf(v, sizeof v, "±%.1f", r.quant);
	statTile(ICON_STOPWATCH, "Timing error", v, shortView ? "mph, fitted bumper track" : "mph, one-frame slip", gradeColor(grade(r.quant, 1, 2)), ImVec2(tw, th));
	ImGui::SameLine(0, px(10));
	snprintf(v, sizeof v, "%.0f px", r.blurPx);
	snprintf(sub, sizeof sub, "%.1f in at 1/%.0f s", r.blurCm / 2.54, prefs.planShutter);
	statTile(ICON_WAVE, "Motion blur", v, sub, gradeColor(grade(r.blurPx, 10, 25)), ImVec2(tw, th));
	ImGui::SameLine(0, px(10));
	snprintf(v, sizeof v, "%.0f", r.pxPerM / 3.281);
	snprintf(sub, sizeof sub, "px per foot · %.2f in/px", 39.37 / r.pxPerM);
	statTile(ICON_IMAGE, "Detail", v, sub, gradeColor(grade(-r.pxPerM, -40, -20)), ImVec2(tw, th));
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("plantable", ICON_TABLE, "Worst-case error by speed", ImVec2(outW, 0));
	if (ImGui::BeginTable("errs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
		for (const char* h : { "Speed", shortView ? "Positions" : "Frames", "Timing ±", "Calibration ±", "Combined ±", "Blur" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		for (int mph : { prefs.speedLimit, prefs.speedLimit + 5, prefs.speedLimit + 10, prefs.speedLimit + 20, prefs.speedLimit + 30 }) {
			Eval e = evaluate(prefs, mph, shortView);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextColored(v4(speedColor(mph, prefs.speedLimit, prefs.egregious)), "%d mph", mph);
			ImGui::TableNextColumn(); ImGui::Text("%.0f", shortView ? e.samples : e.frames);
			if (!std::isfinite(e.total)) {
				ImGui::TableNextColumn(); ImGui::TextColored(v4(c::warn), "past this view");
				ImGui::TableNextColumn(); ImGui::TableNextColumn();
				ImGui::TableNextColumn(); ImGui::Text("%.0f px", e.blurPx);
				continue;
			}
			ImGui::TableNextColumn(); ImGui::Text("%.1f mph", e.quant);
			ImGui::TableNextColumn(); ImGui::Text("%.1f mph", e.calib);
			ImGui::TableNextColumn(); ImGui::TextColored(v4(gradeColor(grade(e.total, 1.5, 3))), "%.1f mph", e.total);
			ImGui::TableNextColumn(); ImGui::Text("%.0f px", e.blurPx);
		}
		ImGui::EndTable();
	}
	dimText("Night is a separate check: a fast shutter at night needs streetlights and high gain, or infrared.");
	endCard();
	ImGui::EndGroup();
}

// "Aim it like this", on the Connect the camera page: where to put the camera once the spot is picked.
void App::aimCard(float width) {
	beginCard("aim", ICON_CROSSHAIRS, "Aim it like this", ImVec2(width, 0));
	aimSketch(ImVec2(ImGui::GetContentRegionAvail().x, px(220)));
	bullet(ICON_ARROWS_LR, c::accent2, "Street across the middle", "Point straight across the street. Lenses distort near the edges, so keep the lanes in the middle third of the picture.");
	bullet(ICON_LOCATE, c::accent2, "Any mount that holds still", "A window clamp, a tripod, even a phone propped on a stack of books. If it gets bumped, VST notices the view moved and says so; marking the street again takes a minute.");
	bullet(ICON_SUN, c::accent2, "Lens to the glass, screen out", "Take out any insect screen and press the lens against the glass. Black craft foam or a dark cloth around it stops the room reflecting. Avoid pointing into the sun.");
	bullet(ICON_CAMERA, c::accent2, "Using a phone? We've got you", "Keep it on the charger and on Do Not Disturb. In its webcam app pick 30 or 60 fps and lock exposure if it can, so it doesn't slow down in shade.");
	endCard();
}

// Steps 1 and 2 of the setup flow.  The stepper and the header's Next are shared with steps 3-5; the button at the
// bottom says the same thing as the header's, for someone who has just read to the end of the page.
void App::pageGuide() {
	guideStep_ = std::clamp(guideStep_, 0, 1);
	flowStepper();
	float avail = ImGui::GetContentRegionAvail().x;

	auto lead = [&](const char* text) {
		ImGui::PushTextWrapPos(std::min(avail, px(900)));
		dimText("%s", text);
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, px(6)));
	};

	if (guideStep_ == 0) {
		ImGui::PushFont(fonts.title);
		ImGui::TextUnformatted("Measure the traffic on your street");
		ImGui::PopFont();
		lead("VST times every car that passes between two lines on your video and works out its speed. Setting it up takes "
			"about half an hour, in five steps. Here is what each one needs. Nothing leaves your computer. Whatever camera you have is a fine "
			"place to start, and every step has a way through if something isn't to hand yet.");
		// One card per later step, in step order, each naming its step, so the list reads as the plan.
		float cw = (avail - px(48)) / 4, ch = px(222);
		gearCard("g2", ICON_LOCATE, c::accent2, "A window on the street", "Step 2 · Pick your spot",
			"Somewhere that sees the street from the side; about 14 ft of clear street is enough. A rough guess is fine: you'll zero in as you go.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g3", ICON_CAMERA, c::accent, "A camera, held still", "Step 3 · Connect the camera",
			"Whatever you have: a phone, webcam, IP or Raspberry Pi camera, on a window clamp or tripod. 30 fps is fine; 60 fps times faster cars on a short view.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g4", ICON_VECTOR, c::warn, "A few minutes with the picture", "Step 4 · Mark your street",
			"Drag a box around the moving lane and two lines to time between. Say which way a one-way street runs.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g5", ICON_CAR, c::l2r, "One drive past", "Step 5 · Calibrate",
			"Drive past at a known speed (cruise control, a Tesla clip, or VST Ride-along on a phone; a bike works too). No car? Two cones or a pole, or a car of known length.", ImVec2(cw, ch));
		ImGui::Dummy(ImVec2(0, px(8)));
		beginCard("need2", ICON_CHECK_CIRCLE, "What makes a good setup", ImVec2(avail, 0));
		float half = (ImGui::GetContentRegionAvail().x - px(20)) * .5f;
		ImGui::BeginGroup();
		ImGui::PushItemWidth(half);
		bullet(ICON_ROAD, c::accent2, "A clear view of the moving lane", "Nothing in the way between the lines. A tree trunk in front, or cars parked behind the lane, is OK.");
		bullet(ICON_STOPWATCH, c::accent2, "Enough frames per car", "At 30 fps a short view times cars up to about 45 mph, at 60 fps over 70. Webcams can drop to 15 fps in dim light, so set 30 or 60 if you can.");
		bullet(ICON_ARROWS_LR, c::accent2, "Square-on to the street", "Point straight across, so cars move left-right across the middle of the picture.");
		bullet(ICON_LOCATE, c::accent2, "Any mount that holds still", "A clamp, a tripod or a stack of books all work. VST checks the view stays put and tells you if it moves.");
		ImGui::PopItemWidth();
		ImGui::EndGroup();
		ImGui::SameLine(0, px(20));
		ImGui::BeginGroup();
		bullet(ICON_SUN, c::accent2, "Lens to the glass", "Take out the insect screen, press the lens to the window and put black foam or a dark cloth around it, so the room doesn't reflect.");
		bullet(ICON_EYE, c::accent2, "Fixed focus and exposure", "Turn off auto-zoom and stabilisation. Lock focus and exposure, with a fast shutter (1/500 s or faster) if the camera lets you.");
		bullet(ICON_BOLT, c::accent2, "Daylight first", "Start with daytime video. Night through glass needs streetlights; infrared bounces off the window.");
		bullet(ICON_FOLDER, c::accent2, "Your data stays yours", "Results are CSV files and videos in a folder on this computer. VST makes traffic studies, not tickets, and reads no plates.");
		ImGui::EndGroup();
		ImGui::PushTextWrapPos(0);
		dimText(ICON_ARROW_UP_RIGHT "  Start with what you have. Later, to time faster cars on a short view: a 60 fps webcam (Logitech C922 at 720p, "
			"or Elgato Facecam), or a Raspberry Pi 5 with the Global Shutter Camera.");
		ImGui::PopTextWrapPos();
		endCard();
	}
	else {
		lead("Start with the street, as you see it from the window. Leave the camera on a phone until you know which one you'll "
			"use. The planner shows how long the timing zone will be and how close the speeds can get.");
		sitePlanner();
	}

	// Back / next, by the steps' own names.
	ImGui::Dummy(ImVec2(0, px(16)));
	int here = flowStep();
	if (here > 0) {
		std::string back = std::string(ICON_ARROW_LEFT "  Back: ") + kFlow[here - 1].name;
		if (ghostButton(back.c_str())) goFlow(here - 1);
		ImGui::SameLine();
	}
	std::string next = std::string("Next: ") + kFlow[here + 1].name + "  " ICON_ARROW_RIGHT;
	float nw = ImGui::CalcTextSize(next.c_str()).x + px(36);
	// Flush with the right edge of the cards above (the content starts at the window padding, not at 0).
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorStartPos().x + avail - nw));
	if (primaryButton(next.c_str(), ImVec2(nw, 0))) goFlow(here + 1);
}
