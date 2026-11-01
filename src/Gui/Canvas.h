// The video canvas: draws a frame letterboxed into the available space and overlays VST's geometry on it.
//
// All geometry is in VST's 1280x720 frame coordinates (what VST.cfg uses); View maps it to screen pixels.
// The zone editor lets every value that used to be typed into VST.cfg be dragged on the picture instead:
// analysis box (edges, corners, move tab), the two speed lines, the obstruction band, the lane lines (whole, or by an end
// to tilt one along a street that runs at an angle) and the optional street edge (Perspective.h).
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

// The level tool (Mark your street, "Level the picture").  Draws reference rows across the frame and, with showDots,
// two draggable dots (frame coordinates) joined by a line the user lays along the curb or a lane stripe.  Returns true
// while a dot is being dragged.
bool levelGuide(const View& v, ImVec2 dots[2], bool showDots);
// The extra Globals::levelDegrees that makes the line p-q level: its slope angle in frame coordinates (y down), so a
// line running downhill to the right gives a positive (counter-clockwise) turn, as levelFrame() applies it.
double levelCorrection(ImVec2 p, ImVec2 q);
// Where frame point p lands after levelFrame() turns the picture a further `degrees` about its centre, so the dots can
// stay on the curb they were laid on.
ImVec2 levelTurn(ImVec2 p, double degrees);

// "Check it with a ghost car" (Mark your street, Lane card): a wireframe car of typical size (perspective::ghostCar),
// projected with the camera setup, its front bumper on each speed line and its hubcap on the lane line there, driving
// right (goingRight) or left.  When the setup is right it sits on a real car at the same spot; too big or small means the
// distance or lens is off, leaning wrong means the angle is.  Draws nothing where the setup can't place it.
void ghostCars(const View& v, const Globals& g, const perspective::CameraSetup& cam, bool goingRight);

// Draws the tracker's live boxes with speed tags.
void trackOverlay(const View& v, const Globals& g, const cv::Rect& analysisBox, const std::vector<vst::TrackBox>& boxes,
	int speedLimit, int egregious);

// Sets a lane line's two ends (rows relative to AnalysisBoxTop, at the box's left and right edges) for one direction, or
// both for a one-way street's single lane.  A line whose ends agree is stored level (right end 0, as VST.cfg always had
// it), so only a line that really runs at an angle writes L2RStreetYRight / R2LStreetYRight (Perspective.h).
void setLane(Globals& g, bool l2r, bool both, int left, int right);

// Arrow along a lane with a label, used by the overlay and the guide.
void laneArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness);
