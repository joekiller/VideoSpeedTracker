#include "Canvas.h"
#include "Icons.h"
#include "Theme.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace theme;

View drawFrame(void* tex, ImVec2 size, const char* emptyText) {
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float r = px(12);
	View v;
	float s = std::min(size.x / 1280.f, size.y / 720.f);
	v.scale = s;
	v.size = ImVec2(1280 * s, 720 * s);
	v.origin = ImVec2(std::floor(p.x + (size.x - v.size.x) * .5f), std::floor(p.y));
	if (tex) dl->AddImageRounded((ImTextureID)(intptr_t)tex, v.origin, v.origin + v.size, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
	else {
		// Empty state: faint road sketch and a prompt, so a blank canvas still says what goes here.
		ImVec2 a = v.origin, b = v.origin + v.size;
		dl->AddRectFilled(a, b, c::rgb(0x0E121A), r);
		float y0 = a.y + v.size.y * .55f, y1 = a.y + v.size.y * .78f;
		dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, y1), c::rgb(0x151B26));
		for (float x = a.x + px(20); x < b.x; x += px(70))
			dl->AddLine(ImVec2(x, (y0 + y1) * .5f), ImVec2(x + px(34), (y0 + y1) * .5f), c::rgb(0x3A4253), px(3));
		ImFont* f = fonts.icons;
		const char* icon = ICON_VIDEO;
		ImVec2 is = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, icon);
		ImVec2 mid((a.x + b.x) * .5f, a.y + v.size.y * .32f);
		dl->AddText(f, fsz(f), mid - is * .5f, c::faint, icon);
		if (emptyText) {
			ImFont* t = fonts.body;
			ImVec2 ts = t->CalcTextSizeA(fsz(t), FLT_MAX, 0, emptyText);
			dl->AddText(t, fsz(t), ImVec2(mid.x - ts.x * .5f, mid.y + is.y), c::dim, emptyText);
		}
	}
	ImGui::Dummy(ImVec2(size.x, v.size.y));
	return v;
}

void laneArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t) {
	dl->AddLine(a, b, col, t);
	ImVec2 d = b - a;
	float len = std::sqrt(d.x * d.x + d.y * d.y);
	if (len < 1) return;
	d = d / len;
	ImVec2 n(-d.y, d.x);
	float h = t * 3.2f;
	dl->AddTriangleFilled(b + d * h * .6f, b - d * h + n * h * .8f, b - d * h - n * h * .8f, col);
}

static void tag(ImDrawList* dl, ImVec2 at, const char* text, ImU32 fg, ImU32 bg, bool centre = false) {
	ImFont* f = fonts.caption;
	ImVec2 ts = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, text);
	ImVec2 pad(px(7), px(3));
	if (centre) at.x -= ts.x * .5f + pad.x;
	dl->AddRectFilled(at, at + ts + pad * 2, bg, px(6));
	dl->AddText(f, fsz(f), at + pad, fg, text);
}

namespace {

enum Handle {
	H_None, H_SpeedL, H_SpeedR, H_ObsL, H_ObsR, H_LaneR2L, H_LaneL2R,
	H_BoxL, H_BoxR, H_BoxT, H_BoxB, H_BoxTL, H_BoxTR, H_BoxBL, H_BoxBR, H_BoxMove
};

struct DragState {
	Handle active = H_None;
	ImVec2 grab;           // frame coords where the drag started
	Globals* target = nullptr;
	int startVals[12] = {};
};
DragState drag;

const char* describe(Handle h) {
	switch (h) {
	case H_SpeedL: return "Left speed line: where timing starts or ends";
	case H_SpeedR: return "Right speed line: where timing starts or ends";
	case H_ObsL: case H_ObsR: return "Obstruction edge (tree, pole): the tracker coasts through it";
	case H_LaneR2L: return "Right-to-left lane: the hubcap height of cars going that way";
	case H_LaneL2R: return "Left-to-right lane: the hubcap height of cars going that way";
	case H_BoxMove: return "Analysis box: drag to move";
	case H_None: return nullptr;
	default: return "Analysis box: drag to resize";
	}
}

}  // namespace

bool zoneOverlay(const View& v, Globals& g, const OverlayStyle& st, const char** hovered) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float bx = float(g.AnalysisBoxLeft), by = float(g.AnalysisBoxTop), bw = float(g.AnalysisBoxWidth), bh = float(g.AnalysisBoxHeight);
	ImVec2 b0 = v.at(bx, by), b1 = v.at(bx + bw, by + bh);
	float op = st.opacity;
	auto A = [&](ImU32 col, float a = 1) { return alpha(col, a * op); };

	// What's under the mouse, in priority order: thin lines first so they stay grabbable inside the box.
	Handle hot = H_None;
	ImVec2 m = ImGui::GetIO().MousePos;
	bool inFrame = st.editable && ImGui::IsWindowHovered() && m.x >= v.origin.x && m.y >= v.origin.y
		&& m.x <= v.origin.x + v.size.x && m.y <= v.origin.y + v.size.y;
	float tol = px(7);
	bool hasObs = g.obstruction[0] < g.obstruction[1];
	if (inFrame && drag.active == H_None) {
		auto nearX = [&](float fx) { return std::fabs(m.x - v.at(fx, 0).x) < tol && m.y > b0.y - tol && m.y < b1.y + tol; };
		auto nearY = [&](float fy) { return std::fabs(m.y - v.at(0, fy).y) < tol && m.x > b0.x - tol && m.x < b1.x + tol; };
		bool nearL = std::fabs(m.x - b0.x) < tol, nearR = std::fabs(m.x - b1.x) < tol;
		bool nearT = std::fabs(m.y - b0.y) < tol, nearB = std::fabs(m.y - b1.y) < tol;
		bool withinX = m.x > b0.x - tol && m.x < b1.x + tol, withinY = m.y > b0.y - tol && m.y < b1.y + tol;
		ImVec2 tabA = b0 - ImVec2(0, px(26)), tabB = b0 + ImVec2(px(132), 0);
		if (nearL && nearT) hot = H_BoxTL;
		else if (nearR && nearT) hot = H_BoxTR;
		else if (nearL && nearB) hot = H_BoxBL;
		else if (nearR && nearB) hot = H_BoxBR;
		else if (nearX(bx + g.speedLineLeft)) hot = H_SpeedL;
		else if (nearX(bx + g.speedLineRight)) hot = H_SpeedR;
		else if (hasObs && nearX(bx + g.obstruction[0])) hot = H_ObsL;
		else if (hasObs && nearX(bx + g.obstruction[1])) hot = H_ObsR;
		else if (nearY(by + g.R2LStreetY)) hot = H_LaneR2L;
		else if (nearY(by + g.L2RStreetY)) hot = H_LaneL2R;
		else if (nearL && withinY) hot = H_BoxL;
		else if (nearR && withinY) hot = H_BoxR;
		else if (nearT && withinX) hot = H_BoxT;
		else if (nearB && withinX) hot = H_BoxB;
		else if (m.x >= tabA.x && m.x <= tabB.x && m.y >= tabA.y && m.y <= tabB.y) hot = H_BoxMove;
	}
	Handle shown = drag.active != H_None ? drag.active : hot;
	if (hovered) *hovered = describe(shown);

	// Mouse cursor feedback.
	switch (shown) {
	case H_SpeedL: case H_SpeedR: case H_ObsL: case H_ObsR: case H_BoxL: case H_BoxR: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
	case H_LaneR2L: case H_LaneL2R: case H_BoxT: case H_BoxB: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
	case H_BoxTL: case H_BoxBR: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
	case H_BoxTR: case H_BoxBL: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
	case H_BoxMove: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
	default: break;
	}

	bool changed = false;
	if (st.editable) {
		if (hot != H_None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			drag.active = hot;
			drag.grab = v.frame(m);
			drag.target = &g;
			int vals[12] = { g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight, g.speedLineLeft, g.speedLineRight,
				g.obstruction[0], g.obstruction[1], g.R2LStreetY, g.L2RStreetY };
			std::copy(vals, vals + 12, drag.startVals);
		}
		if (drag.active != H_None && drag.target == &g) {
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag.active = H_None;
			else {
				ImVec2 f = v.frame(m);
				int dx = int(std::lround(f.x - drag.grab.x)), dy = int(std::lround(f.y - drag.grab.y));
				const int* s = drag.startVals;
				int L = s[0], T = s[1], W = s[2], H = s[3];
				int absSL = L + s[4], absSR = L + s[5], absO0 = L + s[6], absO1 = L + s[7], absR = T + s[8], absL2 = T + s[9];
				auto setBox = [&](int l, int t, int r, int b) {
					l = std::clamp(l, 0, 1279); t = std::clamp(t, 0, 719);
					r = std::clamp(r, l + 40, 1280); b = std::clamp(b, t + 30, 720);
					g.AnalysisBoxLeft = l; g.AnalysisBoxTop = t; g.AnalysisBoxWidth = r - l; g.AnalysisBoxHeight = b - t;
					// Keep the lines where they are on the picture while an edge moves past them.
					g.speedLineLeft = std::clamp(absSL - l, 0, g.AnalysisBoxWidth - 2);
					g.speedLineRight = std::clamp(absSR - l, g.speedLineLeft + 1, g.AnalysisBoxWidth);
					if (s[6] < s[7]) { g.obstruction[0] = std::clamp(absO0 - l, 0, g.AnalysisBoxWidth); g.obstruction[1] = std::clamp(absO1 - l, 0, g.AnalysisBoxWidth); }
					g.R2LStreetY = std::clamp(absR - t, 1, g.AnalysisBoxHeight);
					g.L2RStreetY = std::clamp(absL2 - t, 1, g.AnalysisBoxHeight);
				};
				switch (drag.active) {
				case H_SpeedL: g.speedLineLeft = std::clamp(s[4] + dx, 0, g.speedLineRight - 10); break;
				case H_SpeedR: g.speedLineRight = std::clamp(s[5] + dx, g.speedLineLeft + 10, g.AnalysisBoxWidth); break;
				case H_ObsL: g.obstruction[0] = std::clamp(s[6] + dx, 0, g.obstruction[1] - 4); break;
				case H_ObsR: g.obstruction[1] = std::clamp(s[7] + dx, g.obstruction[0] + 4, g.AnalysisBoxWidth); break;
				case H_LaneR2L: g.R2LStreetY = std::clamp(s[8] + dy, 1, g.AnalysisBoxHeight); break;
				case H_LaneL2R: g.L2RStreetY = std::clamp(s[9] + dy, 1, g.AnalysisBoxHeight); break;
				case H_BoxL: setBox(L + dx, T, L + W, T + H); break;
				case H_BoxR: setBox(L, T, L + W + dx, T + H); break;
				case H_BoxT: setBox(L, T + dy, L + W, T + H); break;
				case H_BoxB: setBox(L, T, L + W, T + H + dy); break;
				case H_BoxTL: setBox(L + dx, T + dy, L + W, T + H); break;
				case H_BoxTR: setBox(L, T + dy, L + W + dx, T + H); break;
				case H_BoxBL: setBox(L + dx, T, L + W, T + H + dy); break;
				case H_BoxBR: setBox(L, T, L + W + dx, T + H + dy); break;
				case H_BoxMove: {
					int nl = std::clamp(L + dx, 0, 1280 - W), nt = std::clamp(T + dy, 0, 720 - H);
					g.AnalysisBoxLeft = nl; g.AnalysisBoxTop = nt;
					break;
				}
				default: break;
				}
				changed = true;
			}
		}
	}

	// Recompute after edits so this frame draws the new geometry.
	b0 = v.at(float(g.AnalysisBoxLeft), float(g.AnalysisBoxTop));
	b1 = v.at(float(g.AnalysisBoxLeft + g.AnalysisBoxWidth), float(g.AnalysisBoxTop + g.AnalysisBoxHeight));
	const float X = float(g.AnalysisBoxLeft), Y = float(g.AnalysisBoxTop);

	// Dim everything the tracker ignores.
	if (st.dimOutside) {
		ImU32 shade = A(c::rgb(0x05070B), .55f);
		ImVec2 f0 = v.origin, f1 = v.origin + v.size;
		dl->AddRectFilled(f0, ImVec2(f1.x, b0.y), shade);
		dl->AddRectFilled(ImVec2(f0.x, b1.y), f1, shade);
		dl->AddRectFilled(ImVec2(f0.x, b0.y), ImVec2(b0.x, b1.y), shade);
		dl->AddRectFilled(ImVec2(b1.x, b0.y), ImVec2(f1.x, b1.y), shade);
	}

	// Speed zone: soft fill between the two lines.
	ImVec2 z0 = v.at(X + g.speedLineLeft, Y), z1 = v.at(X + g.speedLineRight, Y + g.AnalysisBoxHeight);
	dl->AddRectFilledMultiColor(z0, z1, A(c::accent, .10f), A(c::accent, .10f), A(c::accent, .02f), A(c::accent, .02f));

	// Obstruction band, hatched.
	if (g.obstruction[0] < g.obstruction[1]) {
		ImVec2 o0 = v.at(X + g.obstruction[0], Y), o1 = v.at(X + g.obstruction[1], Y + g.AnalysisBoxHeight);
		dl->AddRectFilled(o0, o1, A(c::obstruction, .10f));
		dl->PushClipRect(o0, o1, true);
		for (float k = o0.x - (o1.y - o0.y); k < o1.x; k += px(10))
			dl->AddLine(ImVec2(k, o1.y), ImVec2(k + (o1.y - o0.y), o0.y), A(c::obstruction, .35f), 1.f);
		dl->PopClipRect();
		bool hotO = shown == H_ObsL || shown == H_ObsR;
		dl->AddLine(ImVec2(o0.x, o0.y), ImVec2(o0.x, o1.y), A(c::obstruction, hotO ? 1 : .8f), hotO ? 3.f : 2.f);
		dl->AddLine(ImVec2(o1.x, o0.y), ImVec2(o1.x, o1.y), A(c::obstruction, hotO ? 1 : .8f), hotO ? 3.f : 2.f);
		tag(dl, ImVec2((o0.x + o1.x) * .5f, o1.y + px(6)), ICON_BAN "  obstruction", A(c::bg0), A(c::obstruction, .9f), true);
	}

	// Lanes: direction arrows at the hubcap lines; the wrong-way lane is called out on one-way streets.
	if (st.showLanes) {
		struct Lane { float y; bool l2r; Handle h; };
		for (Lane lane : { Lane{ Y + g.R2LStreetY, false, H_LaneR2L }, Lane{ Y + g.L2RStreetY, true, H_LaneL2R } }) {
			ImU32 col = lane.l2r ? c::l2r : c::r2l;
			bool wrong = g.isWrongWay(lane.l2r);
			bool hotL = shown == lane.h;
			ImVec2 a = v.at(X + 6, lane.y), b = v.at(X + g.AnalysisBoxWidth - 6, lane.y);
			// Dashed lane line.
			for (float x = a.x; x < b.x; x += px(16)) dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + px(9), b.x), a.y), A(col, hotL ? 1 : .85f), hotL ? 3.f : 2.f);
			float mid = (a.x + b.x) * .5f, half = std::min(px(60), (b.x - a.x) * .1f);
			ImVec2 from(lane.l2r ? mid - half : mid + half, a.y), to(lane.l2r ? mid + half : mid - half, a.y);
			laneArrow(dl, from, to, A(wrong ? c::wrong : col), px(4));
			std::string label = std::string(lane.l2r ? "L→R " : "R→L ") + (lane.l2r ? g.L2RDirection : g.R2LDirection);
			if (wrong) label += "  " ICON_WARN " wrong way";
			ImVec2 at(lane.l2r ? a.x + px(6) : b.x - px(6) - pillSize(label.c_str()).x, a.y - px(24));
			tag(dl, at, label.c_str(), A(c::rgb(0x0B0E14)), A(wrong ? c::wrong : col, .92f));
		}
	}

	// Speed lines, with which end is "start" for each direction.
	for (int i = 0; i < 2; i++) {
		float x = X + (i == 0 ? g.speedLineLeft : g.speedLineRight);
		bool hotS = shown == (i == 0 ? H_SpeedL : H_SpeedR);
		ImVec2 a = v.at(x, Y), b = v.at(x, Y + g.AnalysisBoxHeight);
		dl->AddLine(a, b, A(c::rgb(0xFFFFFF), hotS ? 1.f : .9f), hotS ? 3.5f : 2.5f);
		dl->AddCircleFilled(a, px(4), A(c::rgb(0xFFFFFF)));
		dl->AddCircleFilled(b, px(4), A(c::rgb(0xFFFFFF)));
	}
	{
		int widthPx = g.speedLineRight - g.speedLineLeft;
		std::string z = ICON_STOPWATCH "  speed zone  " + std::to_string(widthPx) + " px";
		tag(dl, ImVec2((z0.x + z1.x) * .5f, z1.y + px(6)), z.c_str(), A(c::text), A(c::bg1, .92f), true);
	}

	// Analysis box border + handles + move tab.
	bool hotBox = shown >= H_BoxL;
	dl->AddRect(b0, b1, A(c::accent2, hotBox ? 1.f : .85f), px(4), 0, hotBox ? 2.5f : 2.f);
	if (st.editable) {
		float hs = px(5);
		for (ImVec2 c0 : { b0, ImVec2(b1.x, b0.y), ImVec2(b0.x, b1.y), b1 }) {
			dl->AddRectFilled(c0 - ImVec2(hs, hs), c0 + ImVec2(hs, hs), c::bg0, px(2));
			dl->AddRect(c0 - ImVec2(hs, hs), c0 + ImVec2(hs, hs), A(c::accent2), px(2), 0, 2.f);
		}
		ImVec2 ta = b0 - ImVec2(0, px(26));
		const char* label = ICON_ARROWS_LR "  analysis box";
		ImVec2 ts = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, label);
		dl->AddRectFilled(ta, ta + ImVec2(ts.x + px(16), px(24)), A(c::accent2, shown == H_BoxMove ? 1.f : .85f), px(5), ImDrawFlags_RoundCornersTop);
		dl->AddText(fonts.caption, fsz(fonts.caption), ta + ImVec2(px(8), px(5)), c::bg0, label);
	}
	return changed;
}

void trackOverlay(const View& v, const Globals& g, const cv::Rect& ab, const std::vector<vst::TrackBox>& boxes, int limit, int egregious) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	std::vector<ImRect> tags;   // speed tags placed so far; overlapping vehicles stack their tags upwards
	for (const vst::TrackBox& t : boxes) {
		bool wrong = g.isWrongWay(t.leftToRight);
		ImU32 col = wrong ? c::wrong : t.speed > 0 ? speedColor(t.speed, limit, egregious) : (t.leftToRight ? c::l2r : c::r2l);
		ImVec2 a = v.at(float(ab.x + t.box.x), float(ab.y + t.box.y));
		ImVec2 b = v.at(float(ab.x + t.box.x + t.box.width), float(ab.y + t.box.y + t.box.height));
		dl->AddRectFilled(a, b, alpha(col, .12f), px(6));
		dl->AddRect(a, b, col, px(6), 0, 2.5f);
		// Front-bumper marker: the edge the tracker times.
		float fx = t.leftToRight ? b.x : a.x;
		dl->AddLine(ImVec2(fx, a.y), ImVec2(fx, b.y), alpha(c::rgb(0xFFFFFF), .9f), 3.f);
		std::string label = t.speed > 0 ? std::to_string(t.speed) + " mph" : std::string(t.leftToRight ? "→" : "←") + " tracking";
		if (wrong) label = ICON_WARN " WRONG WAY  " + label;
		ImFont* f = t.speed > 0 ? fonts.strong : fonts.caption;
		ImVec2 ts = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, label.c_str());
		ImVec2 size = ts + ImVec2(px(14), px(8));
		ImVec2 ta(std::clamp(a.x, v.origin.x, std::max(v.origin.x, v.origin.x + v.size.x - size.x)), a.y - ts.y - px(10));
		// Two cars passing each other put their tags on top of each other; lift this one above any it would cover.
		for (bool moved = true; moved;) {
			moved = false;
			for (const ImRect& r : tags)
				if (r.Overlaps(ImRect(ta, ta + size))) { ta.y = r.Min.y - size.y - px(4); moved = true; }
		}
		tags.emplace_back(ta, ta + size);
		dl->AddRectFilled(ta, ta + size, col, px(6));
		dl->AddText(f, fsz(f), ta + ImVec2(px(7), px(4)), c::rgb(0x0B0E14), label.c_str());
	}
}
