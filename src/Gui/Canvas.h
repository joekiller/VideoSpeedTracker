// The video canvas: draws a frame letterboxed into the available space and overlays VST's geometry on it.
//
// All geometry is in VST's 1280x720 frame coordinates (what VST.cfg uses); View maps it to screen pixels.
// The zone editor lets every value that used to be typed into VST.cfg be dragged on the picture instead:
// analysis box (edges, corners, move tab), the two speed lines, the obstruction band and the two lane lines.
#pragma once
#include "Engine.h"
#include "Globals.h"
#include <imgui.h>
#include <vector>

struct View {
	ImVec2 origin{ 0, 0 };     // screen position of frame (0,0)
	float scale = 1;           // screen px per frame px
	ImVec2 size{ 0, 0 };       // on-screen size of the frame
	ImVec2 at(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
	ImVec2 frame(ImVec2 screen) const { return ImVec2((screen.x - origin.x) / scale, (screen.y - origin.y) / scale); }
};

// Draws the texture (or a placeholder) as large as fits in `size`, centred horizontally and top-aligned, and
// advances the cursor by the height actually used; returns the mapping.  Pass tex = nullptr
// to draw the empty-state panel with `emptyText`.
View drawFrame(void* tex, ImVec2 size, const char* emptyText = nullptr);

struct OverlayStyle {
	bool editable = false;
	bool showLanes = true;
	bool dimOutside = true;
	float opacity = 1.f;
};

// Draws the zones; when editable, handles dragging.  Returns true if g changed.  `hovered` gets a short
// description of what's under the mouse (for the status line), or stays empty.
bool zoneOverlay(const View& v, Globals& g, const OverlayStyle& style, const char** hovered = nullptr);

// Draws the tracker's live boxes with speed tags.
void trackOverlay(const View& v, const Globals& g, const cv::Rect& analysisBox, const std::vector<vst::TrackBox>& boxes,
	int speedLimit, int egregious);

// Arrow along a lane with a label, used by the overlay and the guide.
void laneArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness);
