// "Get started": a guided walk-through for someone who has never set up a speed camera.
//
// Six steps, each a plain-language card with a picture or a live tool:
//   1 What you need      camera, computer, mount, and the few numbers that matter (720p, 25-30 fps, fixed focus)
//   2 Pick your spot     the site planner: field of view and distance -> usable zone, frames, error, blur
//   3 Set up the camera  aiming advice with a sketch, then connect the camera or open a test video
//   4 Mark your street   one-way or two-way, compass directions, then the zone editor
//   5 Calibrate          one drive past at a known speed
//   6 Start measuring    run, results, companion
// The planner's model is the one in the project's site calculator: view width at the lane from the field of
// view, limited by the clear stretch, minus a car length and a lock-in run-up gives the timing zone; frames in
// the zone give the one-frame timing error, the distance error over the zone gives the calibration error, and
// speed x shutter time gives motion blur.
// A short view (an old-city street seen across 20 ft or so) uses the engine's short-view timing (subFrameTiming = 1)
// instead.  That times the front bumper with a line fitted through its positions, so a whole car never has to fit
// in the zone: the zone is the view less about a foot at each end, the limit is how many bumper positions the fit
// gets (one per frame pair, at least kMinSamples), and the timing error comes from the bumper's pixel jitter
// spread over the zone rather than from a whole frame.  Both numbers are from the 20 ft simulation in
// tests/short_view.cmake (p90 error 1 mph at 30 fps, fits rejected below 3 points).
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
	{ "Raspberry Pi Global Shutter + 6 mm lens (60 fps)", 45, 1440, 60, 500 },
	{ "720p camera at 60 fps, fixed shutter (short views)", 66, 1280, 60, 500 },
	{ "Original VST camera: Foscam 720p, 30 fps", 70, 1280, 30, 250 },
	{ "Custom", 0, 0, 0, 0 },
};

constexpr double kMph = 0.44704;    // m/s per mph
constexpr double kLockTime = 0.2;   // s of full visibility the tracker needs before timing starts
// Short-view timing (see the top of the file).
constexpr double kEdgeM = 0.3;       // m kept clear inside each end of the view: speed lines about 5-10% in
constexpr double kJitterPx = 12;     // front-bumper jitter in 1280-wide tracker pixels (fits on the 20 ft clip)
constexpr double kMinSamples = 4;    // bumper positions the fit needs to be trusted; the engine refuses fewer than 3
constexpr double kShortFrames = 15;  // below this many whole frames in the zone, recommend short-view timing
constexpr double kMaxTrackMph = 70;  // fastest car the tracker's margins are set for (maxDistOnEntry)

struct Eval { double cover, visible, zone, frames, quant, calib, total, pxPerM, blurCm, blurPx, samples; bool shortView; };

Eval evaluate(const AppPrefs& p, double mph, bool shortView) {
	Eval e{};
	double v = mph * kMph;
	e.shortView = shortView;
	e.cover = 2 * p.planDist * std::tan(p.planHfov * IM_PI / 180 / 2);
	e.visible = std::min(e.cover, p.planClear);
	e.zone = shortView ? std::max(0.0, e.visible - 2 * kEdgeM) : std::max(0.0, e.visible - p.planCarLen - v * kLockTime);
	e.frames = e.zone / v * p.planFps;
	// The engine looks at frames in pairs: one bumper position per pair, plus the one beyond the far line.
	e.samples = e.zone > 0 ? e.frames / 2 + 1 : 0;
	if (!shortView) e.quant = e.frames > 0 ? mph / e.frames : INFINITY;
	else if (e.samples < kMinSamples) e.quant = INFINITY;
	else {
		// Slope error of a least-squares line through n evenly spaced points with position noise s over a run of
		// length L: s * sqrt(12 / n) / L, as a fraction of the speed.
		double jitterM = kJitterPx * e.cover / 1280;
		e.quant = mph * jitterM * std::sqrt(12 / e.samples) / e.zone;
	}
	e.calib = e.zone > 0 ? mph * (p.planCalib / 100) / e.zone : INFINITY;
	e.total = std::hypot(e.quant, e.calib);
	e.pxPerM = p.planWidth / std::max(e.cover, .01);
	e.blurCm = v / p.planShutter * 100;
	e.blurPx = e.blurCm / 100 * e.pxPerM;
	return e;
}

// 0 good, 1 marginal, 2 bad; lower values are better.
int grade(double x, double good, double warn) { return x <= good ? 0 : x <= warn ? 1 : 2; }
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
	dl->AddText(fonts.caption, fsz(fonts.caption), ImVec2(f0.x + px(10), f1.y - px(22)), c::dim, "street level and centred, square-on, 100+ ft in view");
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
	snprintf(t, sizeof t, "zone %.1f m (%.0f ft)   ·   clear %.1f m   ·   in frame %.1f m", r.zone, r.zone * 3.281, pr.planClear, r.cover);
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
	snprintf(t, sizeof t, ICON_CAMERA "  camera, %.1f m from the lane", pr.planDist);
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
		ImGui::PushStyleColor(ImGuiCol_Text, v4(c::warn));
		ImGui::TextWrapped(ICON_INFO "  This is a short view. It needs short-view timing, which times each car's front bumper to a fraction of a frame instead of whole frames.");
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
		toast("Short-view timing is on. Check the speed lines on the Zones page, then calibrate.", c::ok, ICON_CHECK);
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
			"On a one-way street set the legal direction on the Zones page. Every car going the other way is logged and flagged as wrong-way, "
			"timed like any other.");
		bullet(ICON_CAMERA, c::ok, "Through a window",
			"Take out any insect screen, and put the lens against the glass with a rubber hood so the room doesn't reflect. "
			"60 fps lets VST time cars up to about 100 mph in a 20 ft view; 30 fps tops out at about 50-60 mph.");
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}
}

void App::sitePlanner() {
	float avail = ImGui::GetContentRegionAvail().x;
	float inW = px(360);
	float outW = avail - inW - px(20);

	beginCard("planin", ICON_SLIDERS, "Your camera and street", ImVec2(inW, 0));
	sectionLabel("CAMERA");
	ImGui::SetNextItemWidth(-FLT_MIN);
	int n = int(IM_ARRAYSIZE(kPresets));
	prefs.planPreset = std::clamp(prefs.planPreset, 0, n - 1);
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
	const float col = px(190);
	auto num = [&](const char* label, double* v, double step, const char* fmt, const char* help, bool camera) {
		formLabel(label, col);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));
		ImGui::PushID(label);
		if (ImGui::InputDouble("##v", v, step, step * 5, fmt) && camera) prefs.planPreset = n - 1;
		ImGui::PopID();
		if (help) helpMarker(help);
	};
	num("Field of view (°)", &prefs.planHfov, 1, "%.0f", "Horizontal angle the camera sees. Phones and webcams: 60-80°. Wide IP cameras: 90-110°.", true);
	num("Frame width (px)", &prefs.planWidth, 10, "%.0f", nullptr, true);
	num("Frame rate (fps)", &prefs.planFps, 1, "%.0f", nullptr, true);
	num("Shutter (1/x s)", &prefs.planShutter, 10, "%.0f", "Faster shutters freeze motion. Phones choose this themselves and can drop to 1/60 in shade.", true);
	sectionLabel("STREET");
	num("Camera to lane (m)", &prefs.planDist, .5, "%.1f", "Straight-line distance from the lens to the middle of the nearer lane. Pace it out or use a map.", false);
	num("Clear stretch (m)", &prefs.planClear, .5, "%.1f", "The longest stretch of the moving lane you can see with nothing in the way: no trees, poles or parked trucks. A narrow old-city street seen from a front window is often only 5-7 m (15-25 ft); that works with short-view timing.", false);
	num("Distance error (cm)", &prefs.planCalib, 5, "%.0f", "How well you know the zone's length. Tape over 10 m: 10-20 cm. Guessed from a photo: 100 cm or more. A drive-through calibration avoids this.", false);
	num("Car length (m)", &prefs.planCarLen, .1, "%.1f", nullptr, false);
	formLabel("Speed limit (mph)", col);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(26));   // lines up with the inputs above
	ImGui::InputInt("##lim", &prefs.speedLimit, 5);
	ImGui::Dummy(ImVec2(0, px(2)));
	if (ghostButton(ICON_LOCATE "  Narrow street from a front window", ImVec2(-FLT_MIN, 0))) {
		// An old-city one-way with parking, about 13 ft (4 m) curb to curb, seen from across the sidewalk: about 20 ft
		// (6 m) of it between the trees and porches.  The camera distance is to the middle of the moving lane.
		prefs.planDist = 6;
		prefs.planClear = 6.1;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fills in a typical narrow one-way street: 6 m from the camera to the moving lane, 6 m (20 ft) of it clear.\nChange either to match yours.");
	prefs.planHfov = std::clamp(prefs.planHfov, 5.0, 170.0);
	prefs.planFps = std::clamp(prefs.planFps, 5.0, 240.0);
	prefs.planShutter = std::max(prefs.planShutter, 15.0);
	prefs.planDist = std::max(prefs.planDist, 1.0);
	prefs.planClear = std::max(prefs.planClear, 1.0);
	prefs.planWidth = std::max(prefs.planWidth, 320.0);
	prefs.speedLimit = std::clamp(prefs.speedLimit, 5, 80);
	endCard();

	ImGui::SameLine(0, px(20));
	ImGui::BeginGroup();
	int check = prefs.speedLimit + 10;
	// A view too short for whole-frame timing is planned with short-view timing, whether or not it's on yet.
	Eval whole = evaluate(prefs, check, false);
	bool shortOn = cfg.g.subFrameTiming != 0;
	bool shortView = shortOn || whole.zone <= 0 || whole.frames < kShortFrames;
	Eval r = shortView ? evaluate(prefs, check, true) : whole;
	bool tooFew = shortView && r.samples < kMinSamples;
	int verdict = r.zone <= 0 || tooFew ? 2 : grade(r.total, 1.5, 3);
	beginCard("planout", nullptr, nullptr, ImVec2(outW, 0));
	const char* vtext[] = { "Workable", "Marginal", "Not enough room" };
	linePill(vtext[verdict], c::bg0, gradeColor(verdict), fonts.strong);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	if (r.zone <= 0) ImGui::TextUnformatted("Less than a couple of feet of street is in view. Move back, or use a wider lens.");
	else if (tooFew) ImGui::Text("At %d mph a car crosses the %.0f ft zone in %.0f frames, too few to time. Use a 60 fps camera, or see more of the street.", check, r.zone * 3.281, r.frames);
	else ImGui::Text("About ±%.1f mph at %d mph, over a %.1f m (%.0f ft) zone.", r.total, check, r.zone, r.zone * 3.281);
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
	snprintf(sub, sizeof sub, "%.0f cm at 1/%.0f s", r.blurCm, prefs.planShutter);
	statTile(ICON_WAVE, "Motion blur", v, sub, gradeColor(grade(r.blurPx, 10, 25)), ImVec2(tw, th));
	ImGui::SameLine(0, px(10));
	snprintf(v, sizeof v, "%.0f", r.pxPerM);
	snprintf(sub, sizeof sub, "px per metre · %.1f cm/px", 100 / r.pxPerM);
	statTile(ICON_IMAGE, "Detail", v, sub, gradeColor(grade(-r.pxPerM, -40, -20)), ImVec2(tw, th));
	endCard();

	ImGui::Dummy(ImVec2(0, px(2)));
	beginCard("plantable", ICON_TABLE, "Worst-case error by speed", ImVec2(outW, 0));
	if (ImGui::BeginTable("errs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
		for (const char* h : { "Speed", shortView ? "Positions" : "Frames", "Timing ±", "Distance ±", "Combined ±", "Blur" }) ImGui::TableSetupColumn(h);
		ImGui::TableHeadersRow();
		for (int mph : { prefs.speedLimit, prefs.speedLimit + 5, prefs.speedLimit + 10, prefs.speedLimit + 20, prefs.speedLimit + 30 }) {
			Eval e = evaluate(prefs, mph, shortView);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextColored(v4(speedColor(mph, prefs.speedLimit, prefs.egregious)), "%d mph", mph);
			ImGui::TableNextColumn(); ImGui::Text("%.0f", shortView ? e.samples : e.frames);
			if (!std::isfinite(e.total)) {
				ImGui::TableNextColumn(); ImGui::TextColored(v4(c::bad), "too fast");
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

void App::pageGuide() {
	static const char* const steps[] = { "What you need", "Pick your spot", "Set up the camera", "Mark your street", "Calibrate", "Start measuring" };
	const int n = IM_ARRAYSIZE(steps);
	float avail = ImGui::GetContentRegionAvail().x;

	// Stepper.
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = avail / n, h = px(58);
		for (int i = 0; i < n; i++) {
			ImVec2 c0 = p + ImVec2(w * i + px(16), px(16));
			bool done = i < guideStep_, cur = i == guideStep_;
			if (i < n - 1) dl->AddLine(c0 + ImVec2(px(18), 0), ImVec2(p.x + w * (i + 1) + px(2), c0.y), done ? c::accent : c::border, 2);
			dl->AddCircleFilled(c0, px(14), cur ? c::accent : done ? alpha(c::accent, .25f) : c::bg3);
			std::string num = done ? ICON_CHECK : std::to_string(i + 1);
			ImFont* f = fonts.caption;
			ImVec2 ts = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, num.c_str());
			dl->AddText(f, fsz(f), c0 - ts * .5f, cur ? c::rgb(0xFFFFFF) : done ? c::accent : c::dim, num.c_str());
			dl->AddText(cur ? fonts.strong : fonts.body, fsz(fonts.body), c0 + ImVec2(-px(14), px(20)), cur ? c::text : c::dim, steps[i]);
			ImGui::SetCursorScreenPos(p + ImVec2(w * i, 0));
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("step", ImVec2(w, h))) guideStep_ = i;
			ImGui::PopID();
		}
		ImGui::SetCursorScreenPos(p + ImVec2(0, h + px(8)));
	}

	auto heading = [&](const char* title, const char* lead) {
		ImGui::PushFont(fonts.title);
		ImGui::TextUnformatted(title);
		ImGui::PopFont();
		ImGui::PushTextWrapPos(std::min(avail, px(900)));
		dimText("%s", lead);
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, px(6)));
	};

	switch (guideStep_) {
	case 0: {
		heading("Measure the traffic on your street",
			"VST times every car that passes between two lines on your video and works out its speed. You need a camera that can see "
			"the street from the side, a computer to run VST, and about half an hour to set it up. Nothing leaves your computer.");
		float cw = (avail - px(48)) / 4, ch = px(212);
		gearCard("g1", ICON_CAMERA, c::accent, "A camera", "phone, webcam or IP camera",
			"Anything that records 1280×720 or better at 25-30 frames a second. An old phone in USB webcam mode is a free way to start.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g2", ICON_KEYBOARD, c::ok, "A computer", "PC, Mac, mini PC or Pi 5",
			"Any recent computer keeps up with live video. A mini PC or Raspberry Pi can run it all day by the window.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g3", ICON_LOCATE, c::warn, "A steady mount", "window clamp or tripod",
			"The camera must not move once it's set up. Even a small bump means redoing the zones and calibration.", ImVec2(cw, ch));
		ImGui::SameLine(0, px(16));
		gearCard("g4", ICON_CAR, c::l2r, "One drive past", "for calibration",
			"You (or a friend) drive past once at a known speed on cruise control. That's how VST learns distances.", ImVec2(cw, ch));
		ImGui::Dummy(ImVec2(0, px(8)));
		beginCard("need2", ICON_CHECK_CIRCLE, "What makes a good setup", ImVec2(avail, 0));
		float half = (ImGui::GetContentRegionAvail().x - px(20)) * .5f;
		ImGui::BeginGroup();
		ImGui::PushItemWidth(half);
		bullet(ICON_ROAD, c::accent2, "A long, open view", "100 feet or more of street with nothing in the way. A tree trunk in the foreground is OK.");
		bullet(ICON_STOPWATCH, c::accent2, "At least a second of zone", "A car at the speed limit should take a second or more between the lines (about 37 ft at 25 mph).");
		bullet(ICON_ARROWS_LR, c::accent2, "Square-on to the street", "Point straight across, so cars move left-right across the middle of the picture.");
		ImGui::PopItemWidth();
		ImGui::EndGroup();
		ImGui::SameLine(0, px(20));
		ImGui::BeginGroup();
		bullet(ICON_EYE, c::accent2, "Fixed focus and exposure", "Turn off auto-zoom and digital stabilisation; lock focus if the camera allows it.");
		bullet(ICON_BOLT, c::accent2, "Daylight first", "Start with daytime video. Night needs streetlights or an infrared camera.");
		bullet(ICON_FOLDER, c::accent2, "Your data stays yours", "Results are CSV files and videos in a folder on this computer.");
		ImGui::EndGroup();
		endCard();
		break;
	}
	case 1:
		heading("Pick your spot",
			"Before buying or mounting anything, check that your window gives VST enough street to work with. Enter roughly what your "
			"camera and street are like: the planner shows how big the timing zone will be and how accurate speeds can get.");
		sitePlanner();
		break;
	case 2: {
		heading("Set up the camera",
			"Mount the camera where it will stay put, then connect it here. You can also open a video you've already recorded, or even "
			"a photo of the view to plan the zones before the camera arrives.");
		float half = (avail - px(20)) * .5f;
		beginCard("aim", ICON_CROSSHAIRS, "Aim it like this", ImVec2(half, 0));
		aimSketch(ImVec2(ImGui::GetContentRegionAvail().x, px(250)));
		bullet(ICON_ARROWS_LR, c::accent2, "Street across the middle", "Lenses distort near the edges; keep the lanes in the middle third of the picture.");
		bullet(ICON_SUN, c::accent2, "Mind the glare", "Avoid pointing into the sun, and put the lens right against the glass (a dark cloth around it kills reflections).");
		endCard();
		ImGui::SameLine(0, px(20));
		beginCard("connect", ICON_LINK, "Connect it", ImVec2(half, 0));
		drawFrame(player.isOpen() ? player.texture().imguiId() : nullptr, ImVec2(ImGui::GetContentRegionAvail().x, px(250)), "No source yet");
		bullet(ICON_CAMERA, c::ok, "Phone or USB webcam", "Plug it in, then on the Source page pick it by number (0 is usually the first camera).");
		bullet(ICON_BROADCAST, c::accent2, "IP / PoE camera", "Find its RTSP address in the manual, e.g. rtsp://user:pass@192.168.1.20:554/stream1.");
		if (primaryButton(ICON_VIDEO "  Choose a source", ImVec2(-FLT_MIN, 0))) page = Page::Source;
		endCard();
		break;
	}
	case 3: {
		heading("Mark your street",
			"Tell VST which part of the picture is street, where timing starts and stops, and which way traffic is allowed to go.");
		float half = (avail - px(20)) * .5f;
		beginCard("oneway", ICON_SIGNS, "Is it a one-way street?", ImVec2(half, 0));
		dimText("On a one-way street VST flags every car going the other way. Pick the direction cars are allowed to drive, as it "
			"looks in your camera's picture.");
		int legal = cfg.g.LegalDirection == "L2R" ? 1 : cfg.g.LegalDirection == "R2L" ? 2 : 0;
		static const char* const legalLabels[] = { "Two-way", "One-way →", "One-way ←" };
		if (segmented("glegal", &legal, legalLabels, 3)) cfg.g.LegalDirection = legal == 1 ? "L2R" : legal == 2 ? "R2L" : "Both";
		ImGui::Dummy(ImVec2(0, px(6)));
		dimText("Compass directions name each direction in your results, e.g. on a street running NE-SW:");
		static const char* compass[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
		auto combo = [&](const char* label, std::string& v, ImU32 cc) {
			formLabel(label, px(170));
			ImGui::PushStyleColor(ImGuiCol_Text, v4(cc));
			if (ImGui::BeginCombo((std::string("##g") + label).c_str(), v.c_str())) {
				for (const char* cpt : compass) if (ImGui::Selectable(cpt, v == cpt)) v = cpt;
				ImGui::EndCombo();
			}
			ImGui::PopStyleColor();
		};
		combo("Cars going → head", cfg.g.L2RDirection, c::l2r);
		combo("Cars going ← head", cfg.g.R2LDirection, c::r2l);
		endCard();
		ImGui::SameLine(0, px(20));
		beginCard("zonesx", ICON_VECTOR, "Then draw the zones", ImVec2(half, 0));
		bullet(ICON_VECTOR, c::accent2, "Analysis box", "Around the street only. Leave out treetops, sidewalks and parked cars.");
		bullet(ICON_STOPWATCH, c::text, "Two white speed lines", "Timing runs between them. As far apart as the clear view allows.");
		bullet(ICON_ARROWS_LR, c::r2l, "Lane lines", "At hubcap height for each lane.");
		bullet(ICON_BAN, c::obstruction, "Obstruction", "If a tree or pole hides part of the street, mark it so tracking coasts through.");
		if (primaryButton(ICON_VECTOR "  Open the zone editor", ImVec2(-FLT_MIN, 0))) page = Page::Zones;
		endCard();
		break;
	}
	case 4: {
		heading("Calibrate with one drive past",
			"VST doesn't know how many feet are between the lines. Instead it learns how many frames a car at a known speed takes to "
			"cross them. Every other speed is worked out from that.");
		float third = (avail - px(40)) / 3;
		beginCard("c1", nullptr, nullptr, ImVec2(third, px(112)));
		bullet(ICON_PLAY, c::accent, "1. Start tracking", "Live from the camera, or record a video and track that later.");
		endCard();
		ImGui::SameLine(0, px(20));
		beginCard("c2", nullptr, nullptr, ImVec2(third, px(112)));
		bullet(ICON_CAR, c::accent, "2. Drive past steadily", "Use cruise control at, say, 25 mph, through the whole zone. Both ways on a two-way street.");
		endCard();
		ImGui::SameLine(0, px(20));
		beginCard("c3", nullptr, nullptr, ImVec2(third, px(112)));
		bullet(ICON_CHECK_CIRCLE, c::accent, "3. Tell VST which car was you", "Pick your pass on the Calibrate page and enter your speed. Done.");
		endCard();
		ImGui::Dummy(ImVec2(0, px(4)));
		beginCard("ctip", ICON_INFO, "Tips", ImVec2(avail, 0));
		dimText("Check your car's speedometer against a phone GPS app first; many read a few mph high. Do two or three passes and use "
			"the one in the middle. With the companion app you can mark your pass from the car instead of walking back inside.");
		if (primaryButton(ICON_RULER "  Go to Calibrate")) page = Page::Calibrate;
		endCard();
		break;
	}
	default: {
		heading("Start measuring",
			"Press Start tracking. Each vehicle that crosses the zone shows up with its speed, direction and a snapshot; wrong-way drivers "
			"and big speeders pop up as alerts. Everything is saved as you go.");
		float third = (avail - px(40)) / 3;
		gearCard("s1", ICON_GAUGE, c::ok, "Live tracking", "real time or batch", "Watch it work, or run a folder of recorded video as fast as the computer allows.", ImVec2(third, px(180)));
		ImGui::SameLine(0, px(20));
		gearCard("s2", ICON_CHART, c::warn, "Results", "CSV, charts, highlights", "Speed histogram, 85th percentile, speeders and wrong-way drivers with snapshots.", ImVec2(third, px(180)));
		ImGui::SameLine(0, px(20));
		gearCard("s3", ICON_BROADCAST, c::accent2, "Companion app", "on your phone", "Pair a phone to check the camera, calibrate from the curb and see speeders as they happen.", ImVec2(third, px(180)));
		ImGui::Dummy(ImVec2(0, px(8)));
		if (primaryButton(ICON_PLAY "  Start tracking", ImVec2(px(240), px(48)))) { prefs.guideDone = true; startTracking(); }
		ImGui::SameLine(0, px(12));
		if (ghostButton(ICON_BROADCAST "  Pair a phone", ImVec2(0, px(48)))) { prefs.guideDone = true; page = Page::Companion; }
		break;
	}
	}

	// Back / next.
	ImGui::Dummy(ImVec2(0, px(16)));
	if (guideStep_ > 0) {
		if (ghostButton(ICON_ARROW_LEFT "  Back")) guideStep_--;
		ImGui::SameLine();
	}
	if (guideStep_ < n - 1) {
		// Flush with the right edge of the cards above (the content starts at the window padding, not at 0).
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorStartPos().x + avail - px(130)));
		if (primaryButton("Next  " ICON_ARROW_RIGHT, ImVec2(px(130), 0))) guideStep_++;
	}
}
