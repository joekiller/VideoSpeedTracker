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

void setLane(Globals& g, bool l2r, bool both, int left, int right) {
	left = std::clamp(left, 1, std::max(g.AnalysisBoxHeight, 1));
	right = std::clamp(right, 1, std::max(g.AnalysisBoxHeight, 1));
	int storedRight = right == left ? 0 : right;
	if (l2r || both) { g.L2RStreetY = left; g.L2RStreetYRight = storedRight; }
	if (!l2r || both) { g.R2LStreetY = left; g.R2LStreetYRight = storedRight; }
}

namespace {

enum Handle {
	H_None, H_SpeedL, H_SpeedR, H_ObsL, H_ObsR, H_LaneR2L, H_LaneL2R, H_LaneOne, H_Edge,
	H_BoxL, H_BoxR, H_BoxT, H_BoxB, H_BoxTL, H_BoxTR, H_BoxBL, H_BoxBR, H_BoxMove
};

// Which part of a lane line or the street edge is held: the whole line (moves up and down), or one end (tilts it about
// the other, so the line can follow a street that runs at an angle; Perspective.h).
enum End { E_Whole, E_Left, E_Right };

struct DragState {
	Handle active = H_None;
	End end = E_Whole;
	ImVec2 grab;           // frame coords where the drag started
	Globals* target = nullptr;
	int startVals[16] = {};
};
DragState drag;

// Lane-line end handles sit a little inside the analysis box, clear of its own edge handles.
float laneHandleInset(float boxW) { return std::clamp(boxW * .04f, 16.f, 60.f); }

const char* describe(Handle h, End e) {
	if (e != E_Whole && h == H_Edge) return "Street edge end: drag it onto the curb (or a lane stripe) at this side";
	if (e != E_Whole) return "Lane end: drag up or down so the line follows the street";
	switch (h) {
	case H_Edge: return "Street edge: a line along the curb, used to measure how the street recedes";
	case H_SpeedL: return "Left speed line: where timing starts or ends";
	case H_SpeedR: return "Right speed line: where timing starts or ends";
	case H_ObsL: case H_ObsR: return "Obstruction edge (tree, pole): the tracker coasts through it";
	case H_LaneR2L: return "Right-to-left lane: the hubcap height of cars going that way";
	case H_LaneL2R: return "Left-to-right lane: the hubcap height of cars going that way";
	case H_LaneOne: return "Lane: the hubcap height of cars on this one-way street (wrong-way drivers use it too)";
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
	// A one-way street has one lane: both directions' hubcap lines are the same row (a wrong-way car drives in that
	// same lane), so the editor shows and drags a single line that sets both.  The legal direction's value leads.
	const bool oneWay = g.LegalDirection == "L2R" || g.LegalDirection == "R2L";
	const bool legalL2R = g.LegalDirection == "L2R";
	const bool hasEdge = g.hasStreetEdge();
	const float inset = laneHandleInset(bw);
	End hotEnd = E_Whole;
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
		else {
			// Lane lines (at their angle) and the street edge: an end handle first, then anywhere along the line.
			struct Cand { Handle h; perspective::Line l; };
			std::vector<Cand> lines;
			if (oneWay) lines.push_back({ H_LaneOne, g.lane(legalL2R) });
			else { lines.push_back({ H_LaneR2L, g.lane(false) }); lines.push_back({ H_LaneL2R, g.lane(true) }); }
			if (hasEdge) lines.push_back({ H_Edge, { double(g.streetEdge[0]), double(g.streetEdge[1]), double(std::max(bw, 1.f)) } });
			for (const Cand& cd : lines) {
				for (End e : { E_Left, E_Right }) {
					float fx = e == E_Left ? inset : bw - inset;
					ImVec2 d = v.at(bx + fx, by + float(cd.l.at(fx))) - m;
					if (hot == H_None && d.x * d.x + d.y * d.y < tol * tol * 2.f) { hot = cd.h; hotEnd = e; }
				}
			}
			for (const Cand& cd : lines)
				if (hot == H_None && withinX && std::fabs(m.y - v.at(0, by + float(cd.l.at(v.frame(m).x - bx))).y) < tol) hot = cd.h;
		}
		if (hot != H_None) {}
		else if (nearL && withinY) hot = H_BoxL;
		else if (nearR && withinY) hot = H_BoxR;
		else if (nearT && withinX) hot = H_BoxT;
		else if (nearB && withinX) hot = H_BoxB;
		else if (m.x >= tabA.x && m.x <= tabB.x && m.y >= tabA.y && m.y <= tabB.y) hot = H_BoxMove;
	}
	Handle shown = drag.active != H_None ? drag.active : hot;
	End shownEnd = drag.active != H_None ? drag.end : hotEnd;
	if (hovered) *hovered = describe(shown, shownEnd);

	// Mouse cursor feedback.
	switch (shown) {
	case H_SpeedL: case H_SpeedR: case H_ObsL: case H_ObsR: case H_BoxL: case H_BoxR: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
	case H_LaneR2L: case H_LaneL2R: case H_LaneOne: case H_Edge: case H_BoxT: case H_BoxB: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
	case H_BoxTL: case H_BoxBR: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
	case H_BoxTR: case H_BoxBL: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
	case H_BoxMove: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
	default: break;
	}

	bool changed = false;
	if (st.editable) {
		if (hot != H_None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			drag.active = hot;
			drag.end = hotEnd;
			drag.grab = v.frame(m);
			drag.target = &g;
			// Lane lines by their effective right ends (a level line's stored right end is 0).
			int vals[16] = { g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight, g.speedLineLeft, g.speedLineRight,
				g.obstruction[0], g.obstruction[1], g.R2LStreetY, g.L2RStreetY, int(g.lane(false).yRight), int(g.lane(true).yRight),
				g.streetEdge[0], g.streetEdge[1] };
			std::copy(vals, vals + 16, drag.startVals);
		}
		if (drag.active != H_None && drag.target == &g) {
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag.active = H_None;
			else {
				ImVec2 f = v.frame(m);
				int dx = int(std::lround(f.x - drag.grab.x)), dy = int(std::lround(f.y - drag.grab.y));
				const int* s = drag.startVals;
				int L = s[0], T = s[1], W = s[2], H = s[3];
				int absSL = L + s[4], absSR = L + s[5], absO0 = L + s[6], absO1 = L + s[7], absR = T + s[8], absL2 = T + s[9];
				int absRr = T + s[10], absL2r = T + s[11], absE0 = T + s[12], absE1 = T + s[13];
				// A lane line or the street edge, from its starting ends: the whole line moves with the mouse, an end tilts it
				// about the other.  An end that comes within 2 px of level snaps level, so a level line is easy to keep.
				auto moved = [&](int left, int right, int& outL, int& outR) {
					outL = left + (drag.end != E_Right ? dy : 0);
					outR = right + (drag.end != E_Left ? dy : 0);
					if (drag.end != E_Whole && std::abs(outL - outR) <= 2) (drag.end == E_Left ? outL : outR) = drag.end == E_Left ? outR : outL;
				};
				auto setBox = [&](int l, int t, int r, int b) {
					l = std::clamp(l, 0, 1279); t = std::clamp(t, 0, 719);
					r = std::clamp(r, l + 40, 1280); b = std::clamp(b, t + 30, 720);
					g.AnalysisBoxLeft = l; g.AnalysisBoxTop = t; g.AnalysisBoxWidth = r - l; g.AnalysisBoxHeight = b - t;
					// Keep the lines where they are on the picture while an edge moves past them.
					g.speedLineLeft = std::clamp(absSL - l, 0, g.AnalysisBoxWidth - 2);
					g.speedLineRight = std::clamp(absSR - l, g.speedLineLeft + 1, g.AnalysisBoxWidth);
					if (s[6] < s[7]) { g.obstruction[0] = std::clamp(absO0 - l, 0, g.AnalysisBoxWidth); g.obstruction[1] = std::clamp(absO1 - l, 0, g.AnalysisBoxWidth); }
					setLane(g, false, false, absR - t, absRr - t);
					setLane(g, true, false, absL2 - t, absL2r - t);
					if (s[12] > 0 && s[13] > 0) {
						g.streetEdge[0] = std::clamp(absE0 - t, 1, g.AnalysisBoxHeight);
						g.streetEdge[1] = std::clamp(absE1 - t, 1, g.AnalysisBoxHeight);
					}
				};
				switch (drag.active) {
				case H_SpeedL: g.speedLineLeft = std::clamp(s[4] + dx, 0, g.speedLineRight - 10); break;
				case H_SpeedR: g.speedLineRight = std::clamp(s[5] + dx, g.speedLineLeft + 10, g.AnalysisBoxWidth); break;
				case H_ObsL: g.obstruction[0] = std::clamp(s[6] + dx, 0, g.obstruction[1] - 4); break;
				case H_ObsR: g.obstruction[1] = std::clamp(s[7] + dx, g.obstruction[0] + 4, g.AnalysisBoxWidth); break;
				case H_LaneR2L: case H_LaneL2R: case H_LaneOne: {
					bool l2r = drag.active == H_LaneL2R || (drag.active == H_LaneOne && legalL2R);
					int nl, nr;
					moved(l2r ? s[9] : s[8], l2r ? s[11] : s[10], nl, nr);
					setLane(g, l2r, drag.active == H_LaneOne, nl, nr);
					break;
				}
				case H_Edge: {
					int nl, nr;
					moved(s[12], s[13], nl, nr);
					g.streetEdge[0] = std::clamp(nl, 1, g.AnalysisBoxHeight);
					g.streetEdge[1] = std::clamp(nr, 1, g.AnalysisBoxHeight);
					break;
				}
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
	// A lane line's ends on screen (it may run at an angle), and its y on screen at a screen x.
	const float bwNow = float(g.AnalysisBoxWidth);
	auto ends = [&](const perspective::Line& l, ImVec2& a, ImVec2& b) {
		a = v.at(X + 6, Y + float(l.at(6)));
		b = v.at(X + bwNow - 6, Y + float(l.at(bwNow - 6)));
	};
	auto yAt = [&](ImVec2 a, ImVec2 b, float x) { return b.x == a.x ? a.y : a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x); };
	// Round grips at each end of a line while editing, the visible sign that an end can be dragged to tilt it.
	auto grips = [&](const perspective::Line& l, ImU32 col, Handle h) {
		if (!st.editable) return;
		float in = laneHandleInset(bwNow);
		for (End e : { E_Left, E_Right }) {
			float fx = e == E_Left ? in : bwNow - in;
			bool lit = shown == h && shownEnd == e;
			ImVec2 p = v.at(X + fx, Y + float(l.at(fx)));
			dl->AddCircleFilled(p, px(lit ? 7 : 5), A(c::rgb(0x0B0E14)));
			dl->AddCircle(p, px(lit ? 7 : 5), A(col), 0, lit ? 3.f : 2.f);
		}
	};

	// The street edge (advanced, Perspective.h): a thin dashed line along the curb.
	if (st.showLanes && g.hasStreetEdge()) {
		perspective::Line e{ double(g.streetEdge[0]), double(g.streetEdge[1]), double(std::max(bwNow, 1.f)) };
		ImVec2 a, b;
		ends(e, a, b);
		bool hotE = shown == H_Edge;
		ImU32 col = c::streetEdge;
		ImVec2 d = b - a;
		float len = std::sqrt(d.x * d.x + d.y * d.y);
		for (float t = 0; t < len; t += px(14)) {
			float t1 = std::min(t + px(8), len);
			dl->AddLine(a + d * (t / len), a + d * (t1 / len), A(col, hotE ? 1 : .85f), hotE ? 3.f : 2.f);
		}
		grips(e, col, H_Edge);
		tag(dl, ImVec2(a.x + px(6), a.y + px(8)), "street edge", A(c::rgb(0x0B0E14)), A(col, .92f));
	}

	if (st.showLanes && oneWay) {
		// One lane line in the legal direction's colour, with chevrons along it showing the flow.
		ImU32 col = legalL2R ? c::l2r : c::r2l;
		bool hotL = shown == H_LaneOne;
		ImVec2 a, b;
		ends(g.lane(legalL2R), a, b);
		dl->AddLine(a, b, A(c::rgb(0x0B0E14), .45f), hotL ? 6.f : 5.f);   // a dark casing so the line reads on bright asphalt
		dl->AddLine(a, b, A(col, hotL ? 1 : .9f), hotL ? 3.f : 2.f);
		// Chevrons every ~90 screen px pointing the legal way; the label sits above the line, clear of them.
		float span = b.x - a.x, step = std::max(px(70), span / std::max(1.f, std::floor(span / px(90))));
		float h = px(7), dir = legalL2R ? 1.f : -1.f;
		const float gripL = v.at(X + laneHandleInset(bwNow), 0).x, gripR = v.at(X + bwNow - laneHandleInset(bwNow), 0).x;
		for (float x = a.x + step * .5f; x < b.x - px(4); x += step) {
			if (st.editable && (std::fabs(x - gripL) < px(12) || std::fabs(x - gripR) < px(12))) continue;   // leave the end grips clear
			float y = yAt(a, b, x);
			ImVec2 tip(x + dir * h * .5f, y);
			dl->AddPolyline(std::vector<ImVec2>{ ImVec2(tip.x - dir * h, y - h), tip, ImVec2(tip.x - dir * h, y + h) }.data(), 3,
				A(col), ImDrawFlags_None, hotL ? 3.f : 2.5f);
		}
		grips(g.lane(legalL2R), col, H_LaneOne);
		std::string label = std::string(legalL2R ? "One-way  L→R " : "One-way  R→L ") + (legalL2R ? g.L2RDirection : g.R2LDirection);
		float lx = legalL2R ? a.x + px(6) : b.x - px(6) - pillSize(label.c_str()).x;
		ImVec2 at(lx, std::min(yAt(a, b, lx), yAt(a, b, lx + pillSize(label.c_str()).x)) - px(24));
		tag(dl, at, label.c_str(), A(c::rgb(0x0B0E14)), A(col, .92f));
	}
	else if (st.showLanes) {
		struct Lane { bool l2r; Handle h; };
		for (Lane lane : { Lane{ false, H_LaneR2L }, Lane{ true, H_LaneL2R } }) {
			ImU32 col = lane.l2r ? c::l2r : c::r2l;
			bool wrong = g.isWrongWay(lane.l2r);
			bool hotL = shown == lane.h;
			ImVec2 a, b;
			ends(g.lane(lane.l2r), a, b);
			// Dashed lane line, along its angle.
			for (float x = a.x; x < b.x; x += px(16)) {
				float x1 = std::min(x + px(9), b.x);
				dl->AddLine(ImVec2(x, yAt(a, b, x)), ImVec2(x1, yAt(a, b, x1)), A(col, hotL ? 1 : .85f), hotL ? 3.f : 2.f);
			}
			float mid = (a.x + b.x) * .5f, half = std::min(px(60), (b.x - a.x) * .1f);
			float fx = lane.l2r ? mid - half : mid + half, tx = lane.l2r ? mid + half : mid - half;
			laneArrow(dl, ImVec2(fx, yAt(a, b, fx)), ImVec2(tx, yAt(a, b, tx)), A(wrong ? c::wrong : col), px(4));
			grips(g.lane(lane.l2r), col, lane.h);
			std::string label = std::string(lane.l2r ? "L→R " : "R→L ") + (lane.l2r ? g.L2RDirection : g.R2LDirection);
			if (wrong) label += "  " ICON_WARN " wrong way";
			float lx = lane.l2r ? a.x + px(6) : b.x - px(6) - pillSize(label.c_str()).x;
			ImVec2 at(lx, std::min(yAt(a, b, lx), yAt(a, b, lx + pillSize(label.c_str()).x)) - px(24));
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

void ghostCars(const View& v, const Globals& g, const perspective::CameraSetup& cam, bool goingRight) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(v.origin, v.origin + v.size, true);   // a car half out of the picture is still worth seeing in part
	const ImU32 col = c::rgb(0x7DD3FC);
	const perspective::Line lane = g.lane(goingRight);
	for (int lineX : { g.speedLineLeft, g.speedLineRight }) {
		double u = g.AnalysisBoxLeft + lineX, anchor = g.AnalysisBoxTop + lane.at(lineX);
		perspective::Ghost gh = perspective::ghostCar(cam, 1280, 720, u, anchor, goingRight);
		if (!gh.ok) continue;
		ImVec2 p[8];
		for (int i = 0; i < 8; i++) p[i] = v.at(float(gh.corner[i].u), float(gh.corner[i].v));
		// Faces first (near side and roof, faint), then the twelve edges: corner index = front*4 + far*2 + top.
		ImVec2 side[4] = { p[0], p[1], p[5], p[4] }, roof[4] = { p[1], p[3], p[7], p[5] };
		dl->AddConvexPolyFilled(side, 4, alpha(col, .16f));
		dl->AddConvexPolyFilled(roof, 4, alpha(col, .10f));
		static const int edges[12][2] = { { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },     // along the street
			{ 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },                                  // across it
			{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 } };                                // up
		for (auto& e : edges) {
			bool hidden = (e[0] & 2) && (e[1] & 2) && !(e[0] & 1 || e[1] & 1);   // the far bottom edges sit behind the car
			dl->AddLine(p[e[0]], p[e[1]], alpha(col, hidden ? .35f : .95f), hidden ? 1.f : 2.f);
		}
		dl->AddCircleFilled(v.at(float(u), float(anchor)), 3.5f, col);   // the hubcap corner, on the lane line
	}
	dl->PopClipRect();
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

namespace { int levelDrag = -1; }   // which level-guide dot is being dragged, -1 = none

bool levelGuide(const View& v, ImVec2 dots[2], bool showDots) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// Reference rows across the whole picture: a level street runs parallel to them.
	ImVec2 f0 = v.origin, f1 = v.origin + v.size;
	for (int i = 1; i < 8; i++) {
		float y = f0.y + v.size.y * i / 8.f;
		dl->AddLine(ImVec2(f0.x, y), ImVec2(f1.x, y), alpha(c::rgb(0xFFFFFF), i == 4 ? .35f : .18f), 1.f);
	}
	if (!showDots) { levelDrag = -1; return false; }

	ImVec2 m = ImGui::GetIO().MousePos;
	bool inFrame = ImGui::IsWindowHovered() && m.x >= f0.x && m.y >= f0.y && m.x <= f1.x && m.y <= f1.y;
	int hot = -1;
	for (int i = 0; i < 2 && levelDrag < 0 && inFrame; i++) {
		ImVec2 d = v.at(dots[i].x, dots[i].y) - m;
		if (d.x * d.x + d.y * d.y < px(12) * px(12)) hot = i;
	}
	if (hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) levelDrag = hot;
	if (levelDrag >= 0) {
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) levelDrag = -1;
		else {
			ImVec2 f = v.frame(m);
			dots[levelDrag] = ImVec2(std::clamp(f.x, 0.f, 1280.f), std::clamp(f.y, 0.f, 720.f));
		}
	}
	if (hot >= 0 || levelDrag >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

	// The line through the two dots, extended across the picture so a small tilt is easy to see against the rows.
	ImVec2 a = v.at(dots[0].x, dots[0].y), b = v.at(dots[1].x, dots[1].y);
	if (std::fabs(b.x - a.x) > 1) {
		float k = (b.y - a.y) / (b.x - a.x);
		dl->AddLine(ImVec2(f0.x, a.y + (f0.x - a.x) * k), ImVec2(f1.x, a.y + (f1.x - a.x) * k), alpha(c::accent2, .45f), 1.5f);
	}
	dl->AddLine(a, b, c::accent2, 3.f);
	for (int i = 0; i < 2; i++) {
		ImVec2 p = i ? b : a;
		bool lit = hot == i || levelDrag == i;
		dl->AddCircleFilled(p, px(lit ? 9 : 7), c::rgb(0x0B0E14));
		dl->AddCircle(p, px(lit ? 9 : 7), c::accent2, 0, 2.5f);
	}
	tag(dl, ImVec2((a.x + b.x) * .5f, std::max(a.y, b.y) + px(10)), "drag the dots onto the curb", c::rgb(0x0B0E14), alpha(c::accent2, .92f), true);
	return levelDrag >= 0;
}

double levelCorrection(ImVec2 p, ImVec2 q) {
	if (q.x < p.x) std::swap(p, q);
	if (q.x - p.x < 1) return 0;
	return std::atan2(q.y - p.y, q.x - p.x) * 180.0 / 3.14159265358979323846;
}

ImVec2 levelTurn(ImVec2 p, double degrees) {
	double t = degrees * 3.14159265358979323846 / 180.0, cs = std::cos(t), sn = std::sin(t);
	double dx = p.x - 640.0, dy = p.y - 360.0;
	return ImVec2(float(640.0 + dx * cs + dy * sn), float(360.0 - dx * sn + dy * cs));
}

// ---- The parking lane (ParkingLane.h) ------------------------------------------------------------------------------------
//
// A line across the whole frame, kept apart from zoneOverlay's Globals because it is Studio's own setting and usually lies
// outside the analysis box.  The same gestures as a lane line: grab it anywhere to move it, grab an end grip to tilt it
// (an end within 2 px of level snaps level).  Its drag state is its own, so a drag started here never moves a zone.
namespace {
struct ParkDrag {
	int end = -1;            // -1 none, 0 left grip, 1 right grip, 2 the whole line
	ImVec2 grab;
	double start[2] = { 0, 0 };
	double* target = nullptr;
};
ParkDrag parkDrag;
constexpr float kParkGripX[2] = { 40.f, 1240.f };   // frame x of the end grips
}  // namespace

bool parkingLineOverlay(const View& v, double y[2], bool editable, const char** hovered, float opacity) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	auto rowAt = [&](float fx) { return float(y[0] + (y[1] - y[0]) * fx / 1280.0); };
	auto A = [&](ImU32 col, float a = 1) { return alpha(col, a * opacity); };
	ImVec2 m = ImGui::GetIO().MousePos;
	bool inFrame = editable && ImGui::IsWindowHovered() && m.x >= v.origin.x && m.y >= v.origin.y
		&& m.x <= v.origin.x + v.size.x && m.y <= v.origin.y + v.size.y;
	float tol = px(7);
	int hot = -1;
	if (inFrame && parkDrag.end < 0) {
		for (int e = 0; e < 2 && hot < 0; e++) {
			ImVec2 d = v.at(kParkGripX[e], rowAt(kParkGripX[e])) - m;
			if (d.x * d.x + d.y * d.y < tol * tol * 2.f) hot = e;
		}
		if (hot < 0 && std::fabs(m.y - v.at(0, rowAt(v.frame(m).x)).y) < tol) hot = 2;
	}
	bool changed = false;
	if (editable && hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		parkDrag.end = hot;
		parkDrag.grab = v.frame(m);
		parkDrag.start[0] = y[0];
		parkDrag.start[1] = y[1];
		parkDrag.target = y;
	}
	if (parkDrag.end >= 0 && parkDrag.target == y) {
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) parkDrag.end = -1;
		else {
			double dy = std::round(v.frame(m).y - parkDrag.grab.y);
			// The grips sit 40 px in from the edges (fractions a and b of the width), so tilting about the other grip moves
			// the stored ends (at x 0 and 1280) a little more than dy, and the far end a little the other way.
			double a = kParkGripX[0] / 1280.0, b = kParkGripX[1] / 1280.0;
			double l = parkDrag.start[0], r = parkDrag.start[1];
			if (parkDrag.end == 2) { l += dy; r += dy; }
			else if (parkDrag.end == 0) { l += dy * b / (b - a); r -= dy * (1 - b) / (b - a); }
			else { r += dy * (1 - a) / (b - a); l -= dy * a / (b - a); }
			if (parkDrag.end != 2 && std::fabs(l - r) <= 2) (parkDrag.end == 0 ? l : r) = parkDrag.end == 0 ? r : l;
			y[0] = std::clamp(std::round(l), 1.0, 719.0);
			y[1] = std::clamp(std::round(r), 1.0, 719.0);
			changed = true;
		}
	}
	int shown = parkDrag.end >= 0 && parkDrag.target == y ? parkDrag.end : hot;
	if (hovered && shown >= 0)
		*hovered = shown == 2 ? "Parking lane: drag it onto the parked cars' wheel centres" : "Parking lane end: drag up or down so the line follows the parked cars";
	if (shown >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

	// Dashed, with a dark casing so it reads on bright asphalt and car paint alike.
	ImVec2 a = v.at(0, rowAt(0)), b = v.at(1280, rowAt(1280));
	bool lit = shown >= 0;
	dl->AddLine(a, b, A(c::rgb(0x0B0E14), .35f), lit ? 6.f : 5.f);
	ImVec2 d = b - a;
	float len = std::sqrt(d.x * d.x + d.y * d.y);
	for (float t = 0; t < len; t += px(16)) {
		float t1 = std::min(t + px(10), len);
		dl->AddLine(a + d * (t / len), a + d * (t1 / len), A(c::parking, lit ? 1 : .9f), lit ? 3.f : 2.f);
	}
	if (editable)
		for (int e = 0; e < 2; e++) {
			ImVec2 p = v.at(kParkGripX[e], rowAt(kParkGripX[e]));
			bool on = shown == e;
			dl->AddCircleFilled(p, px(on ? 7 : 5), A(c::rgb(0x0B0E14)));
			dl->AddCircle(p, px(on ? 7 : 5), A(c::parking), 0, on ? 3.f : 2.f);
		}
	tag(dl, v.at(70, rowAt(70)) - ImVec2(0, px(26)), ICON_CAR "  parking lane", A(c::rgb(0x0B0E14)), A(c::parking, .92f));
	return changed;
}

bool parkingLineDragging() { return parkDrag.end >= 0; }

void wheelbaseSketch(ImDrawList* dl, ImVec2 at, ImVec2 size, bool wheels) {
	// A side-on car facing right, in the card's own colours: body, cabin, two wheels.  The marks to click are drawn in
	// the warning colour the picture uses for them; the span between them is dimensioned underneath.
	const ImU32 body = c::rgb(0x3A4660), glass = c::rgb(0x55647F), tyre = c::rgb(0x0B0E14), rim = c::rgb(0x8B95A8);
	float w = size.x, h = size.y;
	auto P = [&](float fx, float fy) { return ImVec2(at.x + fx * w, at.y + fy * h); };
	float wheelR = h * .16f, axleY = .62f;
	float frontX = .78f, rearX = .22f;   // wheel centres as fractions of the width
	// Body: a rounded slab with a cabin on top.
	dl->AddRectFilled(P(.04f, .34f), P(.97f, .62f), body, px(10));
	ImVec2 cabin[] = { P(.26f, .35f), P(.36f, .12f), P(.66f, .12f), P(.78f, .35f) };
	dl->AddConvexPolyFilled(cabin, 4, body);
	ImVec2 win[] = { P(.31f, .33f), P(.385f, .17f), P(.635f, .17f), P(.72f, .33f) };
	dl->AddConvexPolyFilled(win, 4, glass);
	dl->AddLine(P(.51f, .17f), P(.51f, .33f), body, px(3));
	for (float fx : { rearX, frontX }) {
		ImVec2 cpt = P(fx, axleY);
		dl->AddCircleFilled(cpt, wheelR, tyre);
		dl->AddCircleFilled(cpt, wheelR * .55f, rim);
		dl->AddCircleFilled(cpt, wheelR * .18f, tyre);
	}
	float dimY = .86f;
	auto mark = [&](ImVec2 p) {
		// The crosshair the user clicks: a ring and a cross in the marks' colour.
		dl->AddCircle(p, px(7), c::warn, 0, 2.5f);
		dl->AddLine(p - ImVec2(px(11), 0), p + ImVec2(px(11), 0), c::warn, 2.f);
		dl->AddLine(p - ImVec2(0, px(11)), p + ImVec2(0, px(11)), c::warn, 2.f);
	};
	float x0 = wheels ? rearX : .04f, x1 = wheels ? frontX : .97f;
	ImVec2 m0 = wheels ? P(rearX, axleY) : P(.04f, .5f), m1 = wheels ? P(frontX, axleY) : P(.97f, .5f);
	mark(m0);
	mark(m1);
	// Dimension line with end ticks and the name of the measurement.
	ImVec2 d0 = P(x0, dimY), d1 = P(x1, dimY);
	dl->AddLine(d0, d1, c::dim, 1.5f);
	for (ImVec2 e : { d0, d1 }) dl->AddLine(e - ImVec2(0, px(6)), e + ImVec2(0, px(6)), c::dim, 1.5f);
	for (ImVec2 e : { m0, m1 }) dl->AddLine(ImVec2(e.x, e.y + px(12)), ImVec2(e.x, d0.y - px(6)), alpha(c::dim, .6f), 1.f);
	const char* label = wheels ? "wheelbase" : "length";
	ImVec2 ts = fonts.caption->CalcTextSizeA(fsz(fonts.caption), FLT_MAX, 0, label);
	ImVec2 lp((d0.x + d1.x - ts.x) * .5f, d0.y - ts.y - px(2));
	dl->AddRectFilled(lp - ImVec2(px(4), 0), lp + ts + ImVec2(px(4), 0), c::bg2);
	dl->AddText(fonts.caption, fsz(fonts.caption), lp, c::text, label);
}
