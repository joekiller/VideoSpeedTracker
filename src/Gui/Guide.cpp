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
#include "Server.h"
#include <GLFW/glfw3.h>
#include "Theme.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <string>

using namespace theme;

namespace {

struct Preset { const char* name; double hfov, width, fps, shutter; };
const Preset kPresets[] = {
	// Phones first: the commonest first camera.  A phone streamed as a webcam usually sends 30 fps on an automatic
	// shutter; a recorded video from a camera app with manual controls (Blackmagic Camera, free on iPhone, Galaxy and
	// Pixel) can be 60 fps at 1/1000 s, which VST times afterwards like any recording.
	{ "Phone as a USB webcam (1080p, 30 fps)", 66, 1920, 30, 120 },
	{ "Phone video at 60 fps (Blackmagic Camera app)", 66, 1920, 60, 1000 },
	// USB webcams.  The 60 fps ones are run at 1/1000 s by VST's opening policy (FrameInput.h) when the camera lets it;
	// field of view is the horizontal angle (the C922's 78 degree diagonal is about 70 across, 1080p60 webcams 70-80).
	{ "Webcam at 60 fps, e.g. Logitech C922", 70, 1280, 60, 1000 },
	{ "Webcam at 1080p 60 fps, e.g. Elgato Facecam", 75, 1920, 60, 1000 },
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
	// Wraps where the caller's wrap is (a column's edge), or else at the window's edge.
	float wrap = ImGui::GetCurrentWindow()->DC.TextWrapPos;
	ImGui::PushTextWrapPos(wrap > 0 ? wrap : 0);
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
void planDiagram(const PlanState& pr, const Eval& r, const Globals& g, ImVec2 size) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, p + size, c::rgb(0x0E121A), px(10));
	float laneY = p.y + size.y * .16f, laneH = size.y * .22f, camY = p.y + size.y * .88f;
	double span = std::max({ r.cover, pr.clear, 10.0 }) * 1.15;
	float sx = float(size.x / span), cx = p.x + size.x * .5f;
	float half = float(r.cover / 2 * sx), clearHalf = float(pr.clear / 2 * sx), zoneHalf = float(r.zone / 2 * sx);
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
	snprintf(t, sizeof t, "zone %.0f ft   ·   clear %.0f ft   ·   in frame %.0f ft", r.zone * 3.281, pr.clear * 3.281, r.cover * 3.281);
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
	snprintf(t, sizeof t, ICON_CAMERA "  camera, %.0f ft from the lane", pr.dist * 3.281);
	dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(cx + px(14), camY - px(8)), c::dim, t);
	ImGui::Dummy(size);
}

// What each camera tier is, for "Which camera?" (step 1 and step 2): a suggestion, never a sale.  `preset` is the
// planner's kPresets entry it plans with; the cost and where-to-buy are the hardware notes' (chain stores first).
struct Tier { const char* name; const char* cost; const char* where; int preset; const char* note; };
const Tier kTiers[] = {
	{ "Your phone, as a webcam", "$0", "already in your pocket", 0, "Live, at 30 fps. The easiest start." },
	{ "Your phone, recording at 60 fps", "$0", "free Blackmagic Camera app", 1, "Record, then open the video here. Fast shutter, great timing." },
	{ "Logitech C922 webcam", "about $70-100", "Best Buy, Walmart, Staples", 2, "60 fps at 720p; VST sets its shutter for you." },
	{ "Raspberry Pi 5 + Camera Module 3", "about $150", "online or Micro Center", 8, "Always on, 60 fps, sips power. For later." },
};

// Short-view timing's settings for a plan.  The tracker works in a 1280-wide frame: pixels a car moves in two frames at
// the fastest speed it tracks sets its look-ahead (and the margins scaled from it), and a car length sets how far back
// it looks on entry.  Speed lines go about 7% in from each side of the analysis box, so the bumper is seen before and
// after the zone.
void applyShortView(Globals& g, const PlanState& s) {
	double coverM = plan::coverAt(s.dist, s.hfov);
	double pxPerM = 1280 / std::max(coverM, .5);
	int step = int(std::ceil(pxPerM * kMaxTrackMph * kMph * 2 / std::max(s.fps, 1.0)));
	g.subFrameTiming = 1;
	g.maxL2RDistOnEntry = g.maxR2LDistOnEntry = std::clamp(step, 75, 1280);
	g.entryLookBack = std::clamp(int(pxPerM * s.carLen), 250, g.AnalysisBoxWidth);
	g.speedLineLeft = int(g.AnalysisBoxWidth * .07);
	g.speedLineRight = g.AnalysisBoxWidth - g.speedLineLeft;
}

double now() { return glfwGetTime(); }

std::string feet(double m) { char b[32]; snprintf(b, sizeof b, "%.0f ft", m * plan::kFt); return b; }

const char* directionWords(const std::string& legal) { return legal == "L2R" ? "left to right" : legal == "R2L" ? "right to left" : "both ways"; }

}  // namespace

// ---- Pick your spot's draft: preview until saved ------------------------------------------------------------------

plan::Inputs App::planInputs(const PlanState& s) {
	plan::Inputs in;
	in.distM = s.dist; in.clearM = s.clear; in.carLenM = s.carLen;
	in.hfovDeg = s.hfov; in.widthPx = s.width; in.fps = s.fps; in.shutter = s.shutter;
	in.speedLimit = s.speedLimit;
	return in;
}

plan::Inputs App::planInputs() const { return planInputs(planSaved()); }

bool App::planDirty() const {
	const PlanDraft& d = planDraft_;
	return !d.plan.same(planSaved()) || d.shortView || !d.legal.empty() || !d.l2r.empty() || !d.r2l.empty() || !d.siteName.empty();
}

std::vector<std::string> App::planChanges() const {
	std::vector<std::string> out;
	const PlanState s = planSaved(), &d = planDraft_.plan;
	char b[200];
	auto len = [&](const char* what, double was, double now) {
		if (std::fabs(was - now) * plan::kFt < .5) return;
		snprintf(b, sizeof b, "%s: %.0f → %.0f ft", what, was * plan::kFt, now * plan::kFt);
		out.push_back(b);
	};
	len("Window to the lane", s.dist, d.dist);
	len("Clear stretch", s.clear, d.clear);
	if (d.source != s.source) out.push_back(std::string("Street numbers: ") + plan::sourceName(plan::sourceOf(s.source)) + " → " + plan::sourceName(plan::sourceOf(d.source)));
	if (d.speedLimit != s.speedLimit) { snprintf(b, sizeof b, "Speed limit: %d → %d mph", s.speedLimit, d.speedLimit); out.push_back(b); }
	int n = int(IM_ARRAYSIZE(kPresets));
	if (d.preset != s.preset || d.hfov != s.hfov || d.fps != s.fps || d.shutter != s.shutter || d.width != s.width) {
		snprintf(b, sizeof b, "Camera: %s, %.0f fps → %s, %.0f fps", kPresets[std::clamp(s.preset, 0, n - 1)].name, s.fps,
			kPresets[std::clamp(d.preset, 0, n - 1)].name, d.fps);
		out.push_back(b);
	}
	if (std::fabs(d.carLen - s.carLen) * plan::kFt >= .1) { snprintf(b, sizeof b, "Car length: %.1f → %.1f ft", s.carLen * plan::kFt, d.carLen * plan::kFt); out.push_back(b); }
	if (d.camLat != s.camLat || d.camLon != s.camLon || d.laneLat != s.laneLat || d.laneLon != s.laneLon || d.street != s.street)
		out.push_back(d.street.empty() ? std::string("Map: camera and lane pins placed") : "Map: camera and lane pins on " + d.street);
	if (planDraft_.shortView) out.push_back(cfg.g.subFrameTiming ? "Re-fit short-view timing to this plan (step 4's settings)" : "Turn on short-view timing (step 4's settings)");
	if (!planDraft_.legal.empty()) out.push_back(std::string("One-way street: legal direction ") + directionWords(planDraft_.legal) + " in the picture");
	if (!planDraft_.l2r.empty() || !planDraft_.r2l.empty()) {
		snprintf(b, sizeof b, "Compass: left to right is %s-bound, right to left %s-bound",
			(planDraft_.l2r.empty() ? cfg.g.L2RDirection : planDraft_.l2r).c_str(), (planDraft_.r2l.empty() ? cfg.g.R2LDirection : planDraft_.r2l).c_str());
		out.push_back(b);
	}
	if (!planDraft_.siteName.empty()) out.push_back("Site name: " + prefs.siteName + " → " + planDraft_.siteName);
	return out;
}

// Called at the top of every frame.  The first call picks up a draft kept over a restart.  After that, when the saved
// plan has moved (a calibration, the lane card on Mark your street, the Live page's limit, the phone saving), each part
// the draft hasn't touched follows it; a part both changed keeps the draft's value and says the saved plan moved.
void App::planSync() {
	PlanState saved = planSaved();
	if (!planLoaded_) {
		planLoaded_ = true;
		planBase_ = planDraft_.plan = saved;
		PlanState kept = saved;
		if (kept.parse(prefs.planDraft) && !kept.same(saved)) planDraft_.plan = kept;
		else prefs.planDraft.clear();
		return;
	}
	if (saved.same(planBase_)) return;
	PlanState& d = planDraft_.plan;
	const PlanState& b = planBase_;
	auto group = [&](auto equal, auto take) {
		if (equal(d, b)) take(d, saved);          // untouched: follow the saved plan
		else if (!equal(saved, b)) planMoved_ = true;   // both changed it
	};
	auto eq = [](double x, double y) { return std::fabs(x - y) <= 1e-5 * std::max(1.0, std::fabs(x)); };
	group([&](const PlanState& x, const PlanState& y) { return eq(x.dist, y.dist) && eq(x.clear, y.clear) && x.source == y.source && x.note == y.note && eq(x.carLengths, y.carLengths); },
		[](PlanState& x, const PlanState& y) { x.dist = y.dist; x.clear = y.clear; x.source = y.source; x.note = y.note; x.carLengths = y.carLengths; });
	group([&](const PlanState& x, const PlanState& y) { return eq(x.hfov, y.hfov) && eq(x.width, y.width) && eq(x.fps, y.fps) && eq(x.shutter, y.shutter) && x.preset == y.preset; },
		[](PlanState& x, const PlanState& y) { x.hfov = y.hfov; x.width = y.width; x.fps = y.fps; x.shutter = y.shutter; x.preset = y.preset; });
	group([&](const PlanState& x, const PlanState& y) { return eq(x.carLen, y.carLen); }, [](PlanState& x, const PlanState& y) { x.carLen = y.carLen; });
	group([&](const PlanState& x, const PlanState& y) { return x.speedLimit == y.speedLimit; }, [](PlanState& x, const PlanState& y) { x.speedLimit = y.speedLimit; });
	group([&](const PlanState& x, const PlanState& y) { return eq(x.camLat, y.camLat) && eq(x.camLon, y.camLon) && eq(x.laneLat, y.laneLat) && eq(x.laneLon, y.laneLon) && x.street == y.street; },
		[](PlanState& x, const PlanState& y) { x.camLat = y.camLat; x.camLon = y.camLon; x.laneLat = y.laneLat; x.laneLon = y.laneLon; x.street = y.street; });
	planBase_ = saved;
	prefs.planDraft = planDirty() ? d.serialize() : "";
}

bool App::planCommit(bool quiet) {
	PlanDraft& d = planDraft_;
	bool wasShort = cfg.g.subFrameTiming != 0;
	bool cfgTouched = d.shortView || !d.legal.empty() || !d.l2r.empty() || !d.r2l.empty();
	d.plan.store(prefs);
	prefs.planConfirmed = true;
	Globals& g = cfg.g;
	if (d.shortView) applyShortView(g, d.plan);
	if (!d.l2r.empty()) g.L2RDirection = d.l2r;
	if (!d.r2l.empty()) g.R2LDirection = d.r2l;
	if (!d.legal.empty()) g.LegalDirection = d.legal;
	if (!d.siteName.empty()) prefs.siteName = d.siteName;
	bool turnedShort = d.shortView && !wasShort;
	d = PlanDraft{ planSaved() };
	planBase_ = d.plan;
	planMoved_ = false;
	prefs.planDraft.clear();
	planSavedAt_ = now();
	if (persist) prefs.save();      // the step's numbers are saved now, not at exit
	if (cfgTouched && cfg.path().empty()) toast("Saved here. Open a VST.cfg to keep the one-way and short-view settings too.", c::warn, ICON_WARN);
	if (!quiet) {
		if (turnedShort) toast("Saved. Short-view timing is on: check the speed lines in step 4, Mark your street, then calibrate.", c::ok, ICON_CHECK);
		else toast("Pick your spot saved.", c::ok, ICON_CHECK);
	}
	return true;
}

void App::planDiscard() {
	planDraft_ = PlanDraft{ planSaved() };
	planBase_ = planDraft_.plan;
	planMoved_ = false;
	prefs.planDraft.clear();
	toast("Changes discarded. Pick your spot is as it was saved.", c::dim, ICON_UNDO);
}

void App::planEdited() {
	prefs.planDraft = planDirty() ? planDraft_.plan.serialize() : "";
}

void App::setDraftStreet(double distM, double clearM, plan::Source source, const std::string& note) {
	PlanState& d = planDraft_.plan;
	d.dist = std::max(distM, 1.0);
	d.clear = std::max(clearM, 1.0);
	d.source = int(source);
	d.note = note;
	planEdited();
}

bool App::applyMapToDraft(const plan::MapPick& m, std::string& why) {
	plan::MapResult r = plan::fromMap(m);
	if (!r.valid) {
		why = r.distM > 0 ? "the two pins are " + feet(r.distM) + " apart; put one on your window and one in the middle of the lane in front of it"
			: "place both pins: your window, then the middle of the lane";
		return false;
	}
	PlanState& d = planDraft_.plan;
	d.camLat = m.camLat; d.camLon = m.camLon; d.laneLat = m.laneLat; d.laneLon = m.laneLon;
	d.street = m.street;
	std::string note = "Measured on the map" + (m.street.empty() ? std::string() : ", " + m.street);
	// The map knows the distance, not what blocks the view; a clear stretch longer than the whole picture is impossible,
	// so it's capped at the picture's width there.
	double clear = std::min(d.clear, plan::coverAt(r.distM, d.hfov));
	setDraftStreet(r.distM, clear, plan::Source::Map, note);
	if (r.speedLimit) d.speedLimit = r.speedLimit;
	const Globals& g = cfg.g;
	planDraft_.l2r = r.l2r != g.L2RDirection ? r.l2r : "";
	planDraft_.r2l = r.r2l != g.R2LDirection ? r.r2l : "";
	planDraft_.legal = r.legal != "Both" && r.legal != g.LegalDirection ? r.legal : "";
	planDraft_.siteName = !m.street.empty() && (prefs.siteName.empty() || prefs.siteName == "My street") ? m.street : "";
	planEdited();
	return true;
}

void App::openMap() {
	if (!server || !server->running()) {
		server = std::make_unique<Server>(*this);
		if (!server->start(prefs.remotePort, true)) {
			toast("Can't open the map: port " + std::to_string(prefs.remotePort) + " " + server->error() + ". Change the port on the Companion app page.", c::bad, ICON_WARN);
			server.reset();
			return;
		}
		localServer_ = true;
	}
	openWithSystem("http://127.0.0.1:" + std::to_string(prefs.remotePort) + "/#token=" + prefs.remoteToken + "&to=map");
	toast("The map is opening in your web browser. Drop the two pins, press \"Send to Pick your spot\", then come back here to save.", c::accent2, ICON_MAP);
}

// Under the planner's verdict on a short view: what short-view timing is, a button that sets it up from the planner's
// numbers when the draft is saved, and the things that go wrong on a narrow street with parked cars.
void App::shortViewNote(double coverM, bool on) {
	(void)coverM;
	bool pending = planDraft_.shortView;
	ImGui::PushTextWrapPos(0);
	if (pending) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::TextWrapped(ICON_INFO "  Short-view timing will be %s when you save.", on ? "re-fitted to this plan" : "turned on");
		ImGui::PopStyleColor();
	}
	else if (on) dimText(ICON_CHECK "  Short-view timing is on: VST times each car's front bumper to a fraction of a frame, so it doesn't need a whole car in view.");
	else {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::accent));
		ImGui::TextWrapped(ICON_INFO "  A short view like this is what short-view timing is for: it times each car's front bumper to a fraction of a frame. One click sets it up.");
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	if (pending) {
		if (ghostButton(ICON_UNDO "  Don't change the timing")) { planDraft_.shortView = false; planEdited(); }
	}
	else if (ghostButton(on ? ICON_SLIDERS "  Re-fit the short-view settings to this plan" : ICON_SLIDERS "  Use short-view timing")) {
		planDraft_.shortView = true;
		planEdited();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("On Save, turns on subFrameTiming and sets, from this plan:\n  the look-ahead (max step) for cars up to %.0f mph at %.0f fps\n  the entry look-back to a car length\n  the speed lines 7%% in from each side of the analysis box",
			kMaxTrackMph, planDraft_.plan.fps);
	if (ImGui::TreeNode("Narrow streets: parked cars, shadows and wrong-way drivers")) {
		ImGui::PushTextWrapPos(0);
		bullet(ICON_CAR, c::accent, "Keep parked cars out of the box",
			"Draw the analysis box on the moving lane only: from just above the moving cars' roofs to just below their wheels. A parked car "
			"behind them doesn't move, so it isn't tracked, but a box that takes in the parking lane catches doors, people and parking moves.");
		bullet(ICON_WAVE, c::warn, "Tree shadows",
			"Shadows moving in the wind look like motion. Keep them out of the box where you can, or raise Sensitivity a little on sunny days. "
			"A fixed shutter (not auto) stops the picture brightening and dimming as cars pass.");
		bullet(ICON_BAN, c::wrong, "Wrong-way drivers",
			"On a one-way street set the legal direction in step 4, Mark your street (the map in this step can set it for you). Every car going "
			"the other way is logged and flagged as wrong-way, timed like any other.");
		bullet(ICON_CAMERA, c::ok, "Through a window",
			"Take out any insect screen, and put the lens against the glass with a rubber hood so the room doesn't reflect. "
			"At 30 fps a 15 ft view times cars up to about 45 mph and a 20 ft view up to about 60; at 60 fps both time cars over 70. Use 60 fps and a 1/1000 s shutter if you can.");
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}
}

// ---- the street: a rough guess first, refined as setup goes on ------------------------------------------------

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

// A calibration is a measurement the user made on purpose in step 5, so it updates the saved plan directly (once per
// new calibration) rather than waiting in a draft; an untouched draft follows it (planSync).
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

// The bar across the top of Pick your spot: whether what's on the page is saved, and Save / Discard.  Nothing on the
// page changes the app until Save; the bar lists what would change.  The autosave switch (off by default) saves each
// edit as it's made instead, and the bar then just says so.
void App::planSaveBar(float width) {
	bool dirty = planDirty();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetFrameHeight() + px(20);
	ImU32 col = dirty ? c::warn : c::ok;
	dl->AddRectFilled(p, p + ImVec2(width, h), alpha(col, dirty ? .12f : .07f), px(10));
	dl->AddRect(p, p + ImVec2(width, h), alpha(col, dirty ? .55f : .25f), px(10));
	ImGui::SetCursorScreenPos(p + ImVec2(px(14), px(10)));
	ImGui::AlignTextToFramePadding();
	std::vector<std::string> changes = dirty ? planChanges() : std::vector<std::string>();
	if (dirty) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::PushFont(fonts.strong);
		ImGui::Text(ICON_WARN "  Preview: %d unsaved change%s", int(changes.size()), changes.size() == 1 ? "" : "s");
		ImGui::PopFont();
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) {
			std::string tip = "Nothing changes until you save:";
			for (auto& c : changes) tip += "\n  •  " + c;
			ImGui::SetTooltip("%s", tip.c_str());
		}
		ImGui::SameLine();
		dimText(planMoved_ ? "The saved plan changed too (a calibration or the phone); saving keeps what's shown here." : "Everything below shows the preview.");
	}
	else {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok));
		bool justSaved = planSavedAt_ > 0 && now() - planSavedAt_ < 3;
		ImGui::TextUnformatted(prefs.planAutosave ? (justSaved ? ICON_CHECK "  Saved automatically" : ICON_CHECK "  Every change saves as you make it")
			: ICON_CHECK "  All changes saved");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		dimText(prefs.planAutosave ? "" : "Try anything: changes here are a preview until you press Save.");
	}
	// Right side: the autosave switch, then Discard and Save only while there is something to save (disabled buttons
	// next to "All changes saved" read as a broken page).
	const char* saveLabel = ICON_SAVE "  Save changes";
	const char* discardLabel = ICON_UNDO "  Discard";
	float sw = dirty ? buttonWidth(saveLabel) : 0, dw = dirty ? buttonWidth(discardLabel) : 0;
	const char* autoLabel = "Save automatically";
	float aw = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(autoLabel).x;
	float right = p.x + width - px(12);
	float y = p.y + px(10);
	ImGui::SetCursorScreenPos(ImVec2(right - sw - dw - aw - (dirty ? px(28) : 0), y));
	bool autoOn = prefs.planAutosave;
	if (ImGui::Checkbox(autoLabel, &autoOn)) {
		prefs.planAutosave = autoOn;
		if (autoOn && planDirty()) planCommit(true);
		if (persist) prefs.save();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: changes are a preview until you press Save.\nOn: every change is saved as you make it.");
	if (dirty) {
		ImGui::SetCursorScreenPos(ImVec2(right - sw - dw - px(8), y));
		if (ghostButton(discardLabel, ImVec2(dw, 0))) planDiscard();
		ImGui::SetCursorScreenPos(ImVec2(right - sw, y));
		if (primaryButton(saveLabel, ImVec2(sw, 0), c::warn)) planCommit();
	}
	ImGui::SetCursorScreenPos(p + ImVec2(0, h + px(12)));
}

// "Find it on the map": the map itself is a page of the companion app (OpenStreetMap, companion/js/views/map.js),
// opened in a browser here or on the phone; what it sends lands in the draft like any other change.
void App::mapCard(float width) {
	beginCard("map", ICON_MAP, "Find it on the map", ImVec2(width, 0));
	const PlanState& d = planDraft_.plan;
	ImGui::PushTextWrapPos(0);
	bool placed = (d.camLat != 0 || d.camLon != 0) && (d.laneLat != 0 || d.laneLon != 0);
	if (placed) {
		plan::MapPick m;
		m.camLat = d.camLat; m.camLon = d.camLon; m.laneLat = d.laneLat; m.laneLon = d.laneLon;
		plan::MapResult r = plan::fromMap(m);
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::ok));
		ImGui::TextWrapped(ICON_CHECK_CIRCLE "  %s: your window is about %.0f ft from the lane, looking %s.", d.street.empty() ? "On the map" : d.street.c_str(),
			r.distM * plan::kFt, plan::compass8(r.facingDeg));
		ImGui::PopStyleColor();
		const Globals& g = cfg.g;
		std::string legal = planDraft_.legal.empty() ? g.LegalDirection : planDraft_.legal;
		std::string l2r = planDraft_.l2r.empty() ? g.L2RDirection : planDraft_.l2r, r2l = planDraft_.r2l.empty() ? g.R2LDirection : planDraft_.r2l;
		if (legal == "L2R" || legal == "R2L")
			dimText("One-way: traffic should go %s in the picture (%s-bound). Anyone going the other way is flagged wrong-way.", directionWords(legal), (legal == "L2R" ? l2r : r2l).c_str());
		else dimText("Cars crossing left to right head %s, right to left %s.", l2r.c_str(), r2l.c_str());
	}
	else dimText("Search your address on OpenStreetMap, put one pin on your window and one in the middle of the lane. It works out the distance, "
		"and reads the street's one-way direction and speed limit for you.");
	ImGui::PopTextWrapPos();
	if (primaryButton(placed ? ICON_MAP "  Move the pins on the map" : ICON_MAP "  Open the map", ImVec2(-FLT_MIN, 0))) openMap();
	ImGui::PushTextWrapPos(0);
	ImGui::PushFont(fonts.caption);
	dimText("Opens in your web browser. On the phone: Setup, step 2. The map and address search come from OpenStreetMap; nothing else leaves this computer.");
	ImGui::PopFont();
	ImGui::PopTextWrapPos();
	endCard();
}

// "Which camera?": four ways in, cheapest first, each worked out on the street being planned, so "do I need to buy
// anything?" has an answer before anything is bought.  On Pick your spot (`draft`) a row can be planned with.
void App::cameraTiers(float width) {
	bool onPlanner = flowStep() == 1;
	beginCard("tiers", ICON_CART, onPlanner ? "Which camera? Worked out for this street" : "Which camera? Start with what you have", ImVec2(width, 0));
	ImGui::PushTextWrapPos(0);
	dimText(onPlanner ? "Suggestions only: VST sells nothing. Start free with your phone; buy only if your street needs it."
		: "Suggestions only: VST sells nothing. Your phone is a fine first camera; step 2 works out whether your street needs more.");
	ImGui::PopTextWrapPos();
	PlanState base = onPlanner ? planDraft_.plan : planSaved();
	bool shortOn = cfg.g.subFrameTiming != 0 || planDraft_.shortView;
	int n = int(IM_ARRAYSIZE(kPresets));
	if (ImGui::BeginTable("tiers", onPlanner ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
		ImGui::TableSetupColumn("Camera", 0, 2.2f);
		ImGui::TableSetupColumn("Cost, where", 0, 1.6f);
		ImGui::TableSetupColumn(onPlanner ? "On your street" : "Good for", 0, 2.2f);
		if (onPlanner) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(120));
		ImGui::TableHeadersRow();
		for (const Tier& t : kTiers) {
			const Preset& q = kPresets[std::clamp(t.preset, 0, n - 1)];
			PlanState s = base;
			s.hfov = q.hfov; s.width = q.width; s.fps = q.fps; s.shutter = q.shutter;
			plan::Verdict v = plan::verdict(planInputs(s), shortOn);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushFont(fonts.strong);
			ImGui::TextWrapped("%s", t.name);
			ImGui::PopFont();
			ImGui::PushFont(fonts.caption);
			dimText("%s", t.note);
			ImGui::PopFont();
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", t.cost);
			ImGui::PushFont(fonts.caption);
			dimText("%s", t.where);
			ImGui::PopFont();
			ImGui::TableNextColumn();
			if (onPlanner) {
				linePill(plan::gradeName(v.grade), c::bg0, gradeColor(v.grade), fonts.caption);
				ImGui::PushFont(fonts.caption);
				if (v.shortView && v.fastestMph > 0) dimText("times cars %s", plan::fastestText(v.fastestMph).c_str());
				else if (std::isfinite(v.at.total)) dimText("about ±%.1f mph at %d mph", v.at.total, v.checkMph);
				ImGui::PopFont();
			}
			else {
				ImGui::PushFont(fonts.caption);
				ImGui::TextWrapped("%s", q.fps >= 59 ? "Short views too: times cars over 70 mph on 15-20 ft" : "Most streets; on a 15 ft view, cars up to about 45 mph");
				ImGui::PopFont();
			}
			if (onPlanner) {
				ImGui::TableNextColumn();
				bool current = planDraft_.plan.preset == t.preset;
				ImGui::PushID(t.name);
				if (current) dimText(ICON_CHECK "  Planned");
				else if (ghostButton("Plan with it", ImVec2(-FLT_MIN, 0))) {
					PlanState& d = planDraft_.plan;
					d.preset = t.preset; d.hfov = q.hfov; d.width = q.width; d.fps = q.fps; d.shutter = q.shutter;
					planEdited();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	endCard();
}

// Under "Your street": the ways to turn a rough guess into real numbers, in the order setup reaches them.
void App::zeroInCard(float width) {
	if (showZeroIn_) { ImGui::SetScrollHereY(0); showZeroIn_ = false; }
	beginCard("zeroin", ICON_BULLSEYE, "Zero in on it", ImVec2(width, 0));
	ImGui::PushTextWrapPos(0);
	dimText("Start rough. These firm the numbers up as you go, no tape measure needed: the map above, your camera's picture, and step 5's calibration.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, px(4)));
	PlanState& d = planDraft_.plan;

	// 1. From the picture: how many car lengths fit across it.  A ruler of that many blocks is drawn over the picture
	// at lane height, so matching one block to a car is the whole job.
	ImGui::PushFont(fonts.strong);
	ImGui::TextUnformatted(ICON_CAMERA "  From your camera's picture");
	ImGui::PopFont();
	if (planPictureAvailable()) {
		float w = ImGui::GetContentRegionAvail().x;
		View v = drawFrame(player.texture().imguiId(), ImVec2(w, w * 9 / 16));
		// The ruler starts at what the plan says now, so a good guess needs no change.  It's a working value, not part of
		// the plan, until "Use it".
		if (rulerCars_ <= 0) rulerCars_ = d.carLengths > 0 ? d.carLengths
			: std::clamp(plan::coverAt(d.dist, d.hfov) * plan::kFt / plan::kTypicalCarFt, 1.0, 20.0);
		const Globals& g = cfg.g;
		bool boxed = zonesDone();
		// The stripes sit at lane height (the analysis box's middle once it's marked) until dragged.  Dragging the picture
		// slides them sideways and up or down, so one block can sit exactly over a parked car or a car in a frozen frame;
		// the wheel (or the slider) changes their size.  Only the size measures anything; where they sit is just for
		// lining up, so it's a working value of this card, not part of the plan.
		if (rulerY_ < 0) rulerY_ = boxed ? float(g.AnalysisBoxTop + g.AnalysisBoxHeight / 2) : 720 * .55f;
		double blockPx = plan::kFrameWidthPx / rulerCars_;
		ImVec2 after = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(v.origin);
		ImGui::InvisibleButton("##rulerdrag", v.size);
		if (ImGui::IsItemHovered()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			float wheel = ImGui::GetIO().MouseWheel;
			if (wheel != 0) { rulerCars_ = std::clamp(rulerCars_ * std::pow(0.95, wheel), 1.0, 20.0); ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); }
		}
		if (ImGui::IsItemActive()) {
			ImVec2 dlt = ImGui::GetIO().MouseDelta;
			rulerShift_ += dlt.x / v.scale;
			rulerY_ = std::clamp(rulerY_ + dlt.y / v.scale, 20.f, 700.f);
		}
		ImGui::SetCursorScreenPos(after);
		blockPx = plan::kFrameWidthPx / rulerCars_;
		rulerShift_ = float(std::fmod(std::fmod(double(rulerShift_), blockPx) + blockPx, blockPx));   // 0 .. one block
		float y = rulerY_;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->PushClipRect(v.origin, v.origin + v.size, true);
		for (int i = -1; (i + 1) * blockPx + rulerShift_ < plan::kFrameWidthPx + blockPx; i++) {
			float x0 = float(rulerShift_ + i * blockPx), x1 = float(rulerShift_ + (i + 1) * blockPx);
			ImU32 col = (i + 2) % 2 ? c::accent2 : c::warn;
			dl->AddRectFilled(v.at(x0, y - 16), v.at(x1, y + 16), alpha(col, .5f));
			dl->AddLine(v.at(x0, y - 40), v.at(x0, y + 40), col, px(2));
		}
		dl->PopClipRect();
		ImGui::SetNextItemWidth(-FLT_MIN);
		float nCars = float(rulerCars_);
		if (ImGui::SliderFloat("##carlengths", &nCars, 1, 20, "Size: %.1f car lengths across", ImGuiSliderFlags_Logarithmic)) rulerCars_ = nCars;
		ImGui::SetNextItemWidth(-FLT_MIN);
		float shiftPct = float(rulerShift_ / blockPx * 100);
		if (ImGui::SliderFloat("##carshift", &shiftPct, 0, 100, "Slide sideways: %.0f%% of a block")) rulerShift_ = float(shiftPct / 100 * blockPx);
		ImGui::PushTextWrapPos(0);
		dimText("Drag the stripes on the picture onto a car (a parked one is fine, or pause a video on one), then size them until one block "
			"matches it bumper to bumper. Scroll over the picture to size them too.");
		ImGui::PopTextWrapPos();
		plan::Measured m = plan::fromCarLengths(rulerCars_, boxed ? g.AnalysisBoxWidth : 0,
			boxed ? std::max(0, g.obstruction[1] - g.obstruction[0]) : 0);
		char b[96];
		snprintf(b, sizeof b, ICON_CHECK "  Use it: about %.0f ft across", m.coverFt);
		if (ghostButton(b, ImVec2(-FLT_MIN, 0)) && m.valid) {
			plan::Inputs in = planInputs(d);
			plan::applyMeasured(in, m);
			char note[96];
			snprintf(note, sizeof note, "%.1f car lengths across the picture", rulerCars_);
			d.carLengths = rulerCars_;
			setDraftStreet(in.distM, in.clearM, plan::Source::Picture, note);
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
		if (d.source != int(plan::Source::Calibration) && ghostButton(ICON_UNDO "  Use the measured numbers")) {
			plan::Inputs in = planInputs(d);
			plan::applyMeasured(in, m);
			setDraftStreet(in.distM, in.clearM, plan::Source::Calibration, "Measured by " + how);
		}
	}
	else {
		dimText("Any way you calibrate in step 5 measures the street, and this page updates by itself: a drive past, VST Ride-along "
			"on a phone (a bike at 12-15 mph works too), a Tesla clip, a car of a known model, or two cones or a pole.");
	}
	ImGui::PopTextWrapPos();
	endCard();
}

void App::sitePlanner() {
	float avail = ImGui::GetContentRegionAvail().x;
	planSaveBar(avail);
	float inW = px(380);
	float outW = avail - inW - px(20);
	PlanDraft before = planDraft_;
	PlanState& d = planDraft_.plan;

	// Street first: it's what the user knows from their window, roughly.  The map, a preset or typed numbers are all fine
	// answers on day one, and "Zero in" below firms them up.  The camera comes second and starts as a guess (a phone, the
	// commonest first camera), so the planner answers "is my window good enough?" before anything is bought.  The
	// draft is in metres (the model's units); the street is entered in feet.
	constexpr double kFt = plan::kFt;
	ImGui::BeginGroup();
	mapCard(inW);
	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("planin", ICON_SLIDERS, "Your street", ImVec2(inW, 0));
	const float col = px(190);
	int n = int(IM_ARRAYSIZE(kPresets));
	plan::Source src = plan::sourceOf(d.source);
	ImGui::PushTextWrapPos(0);
	dimText("No map handy? Pick the closest view. A rough guess is fine to start.");
	ImGui::PopTextWrapPos();
	for (const plan::StreetPreset& sp : plan::kStreetPresets) {
		bool on = src == plan::Source::Guess && std::fabs(d.dist * kFt - sp.distFt) < .5 && std::fabs(d.clear * kFt - sp.clearFt) < .5;
		ImGui::PushID(sp.key);
		ImGui::PushStyleColor(ImGuiCol_Header, v4(alpha(c::accent, .22f)));
		std::string label = std::string(on ? ICON_CHECK_CIRCLE "  " : ICON_CIRCLE "  ") + sp.name;
		if (ImGui::Selectable(label.c_str(), on, 0, ImVec2(0, px(26))))
			setDraftStreet(sp.distFt / kFt, sp.clearFt / kFt, plan::Source::Guess, sp.name);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", sp.hint);
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0, px(4)));
	// A number box edits a copy; the draft takes the value once it's sensible (typing "1" on the way to "18" doesn't
	// flash a ruined plan) and is clamped when the box is left.
	auto num = [&](const char* label, double* v, double step, const char* fmt, const char* help, double lo, double hi) {
		formLabel(label, col);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));
		ImGui::PushID(label);
		double shown = *v;
		bool changed = ImGui::InputDouble("##v", &shown, step, step * 5, fmt);
		if (changed && shown >= lo && shown <= hi) *v = shown;
		else changed = false;
		ImGui::PopID();
		if (help) helpMarker(help);
		return changed;
	};
	// The same, shown and typed in another unit (feet, inches) while the draft keeps metres.
	auto numIn = [&](const char* label, double* metres, double scale, double step, const char* fmt, const char* help, double lo, double hi) {
		double shown = *metres * scale;
		if (num(label, &shown, step, fmt, help, lo, hi) && std::fabs(shown - *metres * scale) > 1e-9) { *metres = shown / scale; return true; }
		return false;
	};
	bool typed = numIn("Window to the lane (ft)", &d.dist, kFt, 1, "%.0f", "Straight-line distance from where the camera will sit to the middle of the nearer moving lane.\nNo tape measure? Use the map above, or \"Quick ways to estimate\" below.\n3 to 1000 ft.", 3, 1000);
	typed |= numIn("Clear stretch (ft)", &d.clear, kFt, 1, "%.0f", "The longest stretch of the moving lane you can see with nothing in the way: no trees, poles or parked trucks. A narrow old-city street seen from a front window is often only 14-25 ft; that works with short-view timing, down to about 14 ft.\n3 to 1000 ft.", 3, 1000);
	if (typed) { d.source = int(plan::Source::Typed); d.note.clear(); }
	formLabel("Speed limit (mph)", col);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));   // lines up with the inputs above
	int lim = d.speedLimit;
	if (ImGui::InputInt("##lim", &lim, 5) && lim >= 5 && lim <= 80) d.speedLimit = lim;
	// Where the numbers came from, in the source's colour: amber while it's a guess, green once measured.
	src = plan::sourceOf(d.source);
	ImU32 srcCol = src == plan::Source::Calibration ? c::ok : src == plan::Source::Guess ? c::warn : c::accent2;
	ImGui::Dummy(ImVec2(0, px(2)));
	pill(plan::sourceName(src), c::bg0, srcCol);
	ImGui::SameLine();
	ImGui::PushTextWrapPos(0);
	if (src == plan::Source::Guess) dimText("Zero in below whenever you're ready.");
	else if (!d.note.empty()) dimText("%s", d.note.c_str());
	else dimText("As you typed them.");
	ImGui::PopTextWrapPos();
	if (ImGui::TreeNode("Quick ways to estimate")) {
		ImGui::PushTextWrapPos(0);
		bullet(ICON_ROAD, c::accent2, "Pace it", "One easy step is about 2.5 ft. Pace from the house to the curb, then add half the street.");
		bullet(ICON_RULER, c::accent2, "Count what's there", "Sidewalk slabs are usually 5 ft, a parked car about 15 ft, a parking space about 20 ft.");
		bullet(ICON_MAP, c::accent2, "On a map", "\"Open the map\" above does it for you; any map app's measure tool works too.");
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}

	ImGui::Dummy(ImVec2(0, px(8)));
	sectionLabel("CAMERA");
	ImGui::PushTextWrapPos(0);
	dimText("No camera yet? Leave this on a phone. You'll connect the real one in step 3, and can come back to check it.");
	ImGui::PopTextWrapPos();
	ImGui::SetNextItemWidth(-FLT_MIN);
	d.preset = std::clamp(d.preset, 0, n - 1);
	// The combo names the preset whose numbers the draft has, or Custom.  It never changes the draft by itself (the
	// numbers stay), so a camera typed in by hand doesn't flip back and forth as the page is left and opened again.
	auto matches = [&](const Preset& q) { return q.hfov == d.hfov && q.width == d.width && q.fps == d.fps && q.shutter == d.shutter; };
	int shownPreset = n - 1;
	if (kPresets[d.preset].hfov > 0 && matches(kPresets[d.preset])) shownPreset = d.preset;
	else for (int i = 0; i < n - 1; i++) if (matches(kPresets[i])) { shownPreset = i; break; }
	if (ImGui::BeginCombo("##preset", kPresets[shownPreset].name)) {
		for (int i = 0; i < n; i++)
			if (ImGui::Selectable(kPresets[i].name, shownPreset == i)) {
				d.preset = i;
				if (kPresets[i].hfov > 0) { d.hfov = kPresets[i].hfov; d.width = kPresets[i].width; d.fps = kPresets[i].fps; d.shutter = kPresets[i].shutter; }
			}
		ImGui::EndCombo();
	}
	// A connected camera knows its own size and frame rate; planning with them shows what a 15 fps webcam means.
	if (planPictureAvailable() && player.info().kind != SourceInfo::Image && player.info().fps > 0
		&& (std::fabs(player.info().fps - d.fps) > .5 || player.info().width != int(d.width))) {
		char b[120];
		snprintf(b, sizeof b, ICON_CAMERA "  Plan with your camera: %d × %d, %.0f fps", player.info().width, player.info().height, player.info().fps);
		if (ghostButton(b, ImVec2(-FLT_MIN, 0))) {
			d.width = player.info().width;
			d.fps = std::round(player.info().fps);
			d.preset = n - 1;
		}
	}
	if (d.fps < 24) {
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::PushTextWrapPos(0);
		ImGui::TextWrapped(ICON_INFO "  %s", frameRateAdvice(d.fps).c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}
	if (ImGui::TreeNode("Camera details")) {
		bool cam = false;
		cam |= num("Field of view (°)", &d.hfov, 1, "%.0f", "Horizontal angle the camera sees. Phones and webcams: 60-80°. Wide IP cameras: 90-110°.\n5 to 170°.", 5, 170);
		cam |= num("Frame width (px)", &d.width, 10, "%.0f", "320 to 8000.", 320, 8000);
		cam |= num("Frame rate (fps)", &d.fps, 1, "%.0f", "5 to 240.", 5, 240);
		cam |= num("Shutter (1/x s)", &d.shutter, 10, "%.0f", "Faster shutters freeze motion. Phones choose this themselves and can drop to 1/60 in shade.\n15 to 20000.", 15, 20000);
		if (cam) d.preset = n - 1;
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Fine-tuning")) {
		numIn("Car length (ft)", &d.carLen, kFt, .5, "%.1f", "A typical car is 15-16 ft. Whole-frame timing needs one fully in the zone; short-view timing doesn't.\n8 to 30 ft.", 8, 30);
		ImGui::TreePop();
	}
	endCard();
	ImGui::Dummy(ImVec2(0, px(2)));
	zeroInCard(inW);
	ImGui::EndGroup();

	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	bool shortOn = cfg.g.subFrameTiming != 0 || planDraft_.shortView;
	plan::Verdict pv = plan::verdict(planInputs(d), shortOn);
	int check = pv.checkMph;
	bool shortView = pv.shortView;
	const Eval& r = pv.at;
	int verdict = pv.grade;
	beginCard("planout", nullptr, nullptr, ImVec2(outW, 0));
	linePill(plan::gradeName(verdict), c::bg0, gradeColor(verdict), fonts.strong);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	// Wraps inside the card: the blur advice makes the sentence longer than the card on a narrow window.
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted(plan::verdictText(pv).c_str());
	ImGui::PopTextWrapPos();
	// The saved plan's verdict next to the preview's, so a change's effect is plain before it's saved.
	if (planDirty()) {
		plan::Verdict sv = plan::verdict(planInputs(), cfg.g.subFrameTiming != 0);
		ImGui::PushTextWrapPos(0);
		dimText(ICON_SAVE "  Saved plan: %s. %s", plan::gradeName(sv.grade), plan::verdictText(sv).c_str());
		ImGui::PopTextWrapPos();
	}
	// How firm that is: a verdict on a guessed street says so, and says it's fine.
	if (src == plan::Source::Guess) {
		ImGui::PushTextWrapPos(0);
		dimText(ICON_INFO "  Based on a rough guess at your street. That's enough to choose a camera; it firms up as you zero in.");
		ImGui::PopTextWrapPos();
	}
	if (shortView && r.zone > 0) {
		// The fastest car this view can time, and what 60 fps would make of it: the spec a short view is bought on.
		ImGui::PushTextWrapPos(0);
		// The 60 fps sentence only when it says something new: "over 70 mph" twice reads like a mistake.
		if (d.fps < 59 && plan::fastestText(pv.fastest60) != plan::fastestText(pv.fastestMph)) dimText(ICON_GAUGE "  At %.0f fps this view times cars %s. A 60 fps camera would time them %s.",
			d.fps, plan::fastestText(pv.fastestMph).c_str(), plan::fastestText(pv.fastest60).c_str());
		else dimText(ICON_GAUGE "  At %.0f fps this view times cars %s.", d.fps, plan::fastestText(pv.fastestMph).c_str());
		ImGui::PopTextWrapPos();
	}
	if (shortView) shortViewNote(r.cover, cfg.g.subFrameTiming != 0);
	Globals shown = cfg.g;   // the diagram's one-way callout, as it will be once saved
	if (!planDraft_.legal.empty()) shown.LegalDirection = planDraft_.legal;
	if (!planDraft_.l2r.empty()) shown.L2RDirection = planDraft_.l2r;
	if (!planDraft_.r2l.empty()) shown.R2LDirection = planDraft_.r2l;
	planDiagram(d, r, shown, ImVec2(ImGui::GetContentRegionAvail().x, px(230)));
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
	snprintf(sub, sizeof sub, "%.1f in at 1/%.0f s", r.blurCm / 2.54, d.shutter);
	statTile(ICON_WAVE, "Motion blur", v, sub, gradeColor(grade(r.blurPx, 10, 25)), ImVec2(tw, th));
	ImGui::SameLine(0, px(10));
	snprintf(v, sizeof v, "%.0f", r.pxPerM / 3.281);
	snprintf(sub, sizeof sub, "px per foot · %.2f in/px", 39.37 / r.pxPerM);
	statTile(ICON_IMAGE, "Detail", v, sub, gradeColor(grade(-r.pxPerM, -40, -20)), ImVec2(tw, th));
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	cameraTiers(outW);

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("plantable", ICON_TABLE, "Worst-case error by speed", ImVec2(outW, 0));
	if (ImGui::BeginTable("errs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
		for (const char* h : { "Speed", shortView ? "Positions" : "Frames", "Timing ±", "Calibration ±", "Combined ±", "Blur" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		for (int mph : { d.speedLimit, d.speedLimit + 5, d.speedLimit + 10, d.speedLimit + 20, d.speedLimit + 30 }) {
			Eval e = plan::evaluate(planInputs(d), mph, shortView);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextColored(v4(speedColor(mph, d.speedLimit, std::max(prefs.egregious, d.speedLimit))), "%d mph", mph);
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

	bool changed = !before.plan.same(planDraft_.plan) || before.shortView != planDraft_.shortView;
	if (changed) planEdited();
}

// "Aim it like this", on the Connect the camera page: where to put the camera once the spot is picked.
void App::aimCard(float width) {
	beginCard("aim", ICON_CROSSHAIRS, "Aim it like this", ImVec2(width, 0));
	aimSketch(ImVec2(ImGui::GetContentRegionAvail().x, px(220)));
	bullet(ICON_ARROWS_LR, c::accent2, "Street across the middle", "Point straight across the street. Lenses distort near the edges, so keep the lanes in the middle third of the picture.");
	bullet(ICON_LOCATE, c::accent2, "Any mount that holds still", "A window clamp, a tripod, even a phone propped on a stack of books. If it gets bumped, VST notices the view moved and says so; marking the street again takes a minute.");
	bullet(ICON_SUN, c::accent2, "Lens to the glass, screen out", "Take out any insect screen and press the lens against the glass. Black craft foam or a dark cloth around it stops the room reflecting. Avoid pointing into the sun.");
	bullet(ICON_CAMERA, c::accent2, "Using a phone? We've got you", "Android 14 or later: plug it in by USB and pick \"Webcam\" in the USB notification. iPhone with a Mac: "
		"Continuity Camera shows it as a camera by itself. Otherwise a free webcam app (Camo, DroidCam or Iriun) does it over Wi-Fi or USB. "
		"Keep it on the charger and on Do Not Disturb; pick 30 or 60 fps and lock exposure if the app can.");
	bullet(ICON_STOPWATCH, c::accent2, "Or record, then open the video", "For faster cars, or with no webcam app at all: record with a camera app that has manual controls "
		"(Blackmagic Camera is free) at 60 fps and 1/1000 s, copy the video over and open it in step 3. VST times it just the same.");
	endCard();
}


// Steps 1 and 2 of the setup flow.  The stepper and the header's Next are shared with steps 3-5; the button at the
// bottom says the same thing as the header's, for someone who has just read to the end of the page.
void App::pageGuide() {
	guideStep_ = std::clamp(guideStep_, 0, 1);
	flowStepper();
	float avail = ImGui::GetContentRegionAvail().x;

	auto lead = [&](const char* text) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(avail, px(900)));
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
			"Find your window on the map, or pick the closest kind of street; about 14 ft of clear street is enough. A rough guess is fine: you'll zero in as you go.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g3", ICON_CAMERA, c::accent, "A camera, held still", "Step 3 · Connect the camera",
			"Whatever you have: your phone, a webcam, an IP or Raspberry Pi camera, on a window clamp or a stack of books. 30 fps is fine to start.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g4", ICON_VECTOR, c::warn, "A few minutes with the picture", "Step 4 · Mark your street",
			"Drag a box around the moving lane and two lines to time between. Say which way a one-way street runs.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g5", ICON_CAR, c::l2r, "One drive past, or no car at all", "Step 5 · Calibrate",
			"Drive past at a known speed (a bike works too), or with no car: two cones or a pole, or a parked car of a known model.", ImVec2(cw, ch));
		ImGui::Dummy(ImVec2(0, px(8)));
		cameraTiers(avail);
		ImGui::Dummy(ImVec2(0, px(8)));
		beginCard("need2", ICON_CHECK_CIRCLE, "What makes a good setup", ImVec2(avail, 0));
		// Two columns, each wrapping inside its own half (a bare text wrap goes to the card's edge and runs under the other column).
		float half = (ImGui::GetContentRegionAvail().x - px(24)) * .5f;
		float x0 = ImGui::GetCursorPosX();
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(x0 + half);
		bullet(ICON_ROAD, c::accent2, "A clear view of the moving lane", "Nothing in the way between the lines. A tree trunk in front, or cars parked behind the lane, is OK.");
		bullet(ICON_STOPWATCH, c::accent2, "Enough pictures per second", "Frames per second (fps) is how many pictures the camera takes each second. At 30 fps a short view times cars up to about 45 mph, at 60 fps over 70. Webcams can drop to 15 fps in dim light, so pick 30 or 60 if you can.");
		bullet(ICON_ARROWS_LR, c::accent2, "Square-on to the street", "Point straight across, so cars move left-right across the middle of the picture.");
		bullet(ICON_LOCATE, c::accent2, "Any mount that holds still", "A clamp, a tripod or a stack of books all work. VST checks the view stays put and tells you if it moves.");
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::SameLine(x0 + half + px(24));
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(x0 + half * 2 + px(24));
		bullet(ICON_SUN, c::accent2, "Lens to the glass", "Take out the insect screen, press the lens to the window and put black foam or a dark cloth around it, so the room doesn't reflect.");
		bullet(ICON_EYE, c::accent2, "Fixed focus and a fast shutter", "Turn off auto-zoom and stabilisation, and lock focus. The shutter is how long each picture is exposed: 1/1000 s freezes a passing car, "
			"1/250 s will do. VST fixes a USB webcam's shutter for you where the camera allows it.");
		bullet(ICON_BOLT, c::accent2, "Daylight first", "Start with daytime video. Night through glass needs streetlights; infrared bounces off the window.");
		bullet(ICON_FOLDER, c::accent2, "Your data stays yours", "Results are CSV files and videos in a folder on this computer. VST makes traffic studies, not tickets, and reads no plates.");
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		endCard();
	}
	else {
		lead("Where will the camera go, and what can it measure from there? Find your window on the map (or pick the closest kind of street), "
			"leave the camera on a phone until you know which one you'll use, and the planner shows how close the speeds can get. "
			"Try anything: nothing here changes until you save.");
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
	float nx = ImGui::GetCursorStartPos().x + avail - nw;
	if (here == 1 && planDirty()) {
		// Leaving keeps the draft (it's still here when you come back), but say so where the eye is.
		const char* note = ICON_WARN "  Unsaved changes stay as a preview";
		ImVec2 ns = pillSize(note, fonts.body);
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), nx - ns.x - px(12)));
		linePill(note, c::warn, alpha(c::warn, .14f), fonts.body);
		ImGui::SameLine();
	}
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), nx));
	if (primaryButton(next.c_str(), ImVec2(nw, 0))) goFlow(here + 1);
}
