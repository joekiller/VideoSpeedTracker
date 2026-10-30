// Look and feel for VST Studio: palette, fonts, ImGui style and the handful of custom widgets the pages share
// (cards, pills, stat tiles, segmented controls).  Everything is drawn by ImGui, so the app looks the same on
// Linux, Windows and macOS.
#pragma once
#include <imgui.h>
#include <string>

namespace theme {

// Palette (sRGB, packed as ImU32 via col()).  The direction colours match the engine's own overlay: purple for
// left-to-right traffic and orange for right-to-left.
namespace c {
constexpr ImU32 rgb(unsigned hex, unsigned a = 255) {
	return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, a);
}
inline const ImU32 bg0 = rgb(0x0B0E14);       // window background
inline const ImU32 bg1 = rgb(0x111620);       // side rail, header
inline const ImU32 bg2 = rgb(0x171D2A);       // cards
inline const ImU32 bg3 = rgb(0x1F2738);       // inputs, hovered cards
inline const ImU32 border = rgb(0x273246);
inline const ImU32 text = rgb(0xE7EBF3);
inline const ImU32 dim = rgb(0x8B95A8);
inline const ImU32 faint = rgb(0x5A6478);
inline const ImU32 accent = rgb(0x4C8DFF);
inline const ImU32 accent2 = rgb(0x22D3EE);
inline const ImU32 ok = rgb(0x22C55E);
inline const ImU32 warn = rgb(0xF5A524);
inline const ImU32 bad = rgb(0xEF4444);
inline const ImU32 wrong = rgb(0xF43F5E);
inline const ImU32 l2r = rgb(0xB06CFF);
inline const ImU32 r2l = rgb(0xFF8A3D);
inline const ImU32 obstruction = rgb(0xFACC15);
}  // namespace c

ImVec4 v4(ImU32 col, float alpha = -1.f);
ImU32 alpha(ImU32 col, float a);

struct Fonts {
	ImFont* body = nullptr;      // Inter Regular + icons
	ImFont* strong = nullptr;    // Inter SemiBold + icons
	ImFont* small = nullptr;     // Inter Regular, smaller
	ImFont* title = nullptr;     // Inter SemiBold, page titles
	ImFont* stat = nullptr;      // Inter Bold, big numbers
	ImFont* hero = nullptr;      // Inter Bold, the speed readout
	ImFont* icons = nullptr;     // Font Awesome only, large
};
extern Fonts fonts;

// Loads the embedded fonts and applies the style.  uiScale sizes everything (Windows/Linux HiDPI); raster
// renders the font atlas at a higher resolution than the UI coordinates (macOS Retina, where ImGui works in
// points and the framebuffer has 2 pixels per point).  Call before the first frame.
void setup(float uiScale, float raster = 1.f);
float scale();
inline float px(float v) { return v * scale(); }
// A font's on-screen size (its atlas size divided by the Retina raster factor); use with draw-list text calls.
inline float fsz(const ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }

// Colour for a speed, using the same thresholds as the tracker's own overlay.
ImU32 speedColor(int mph, int limit, int egregious);

// --- widgets ---------------------------------------------------------------------------------------------
// Rounded card with an optional icon + title row; pair every beginCard() with endCard().
bool beginCard(const char* id, const char* icon = nullptr, const char* title = nullptr, ImVec2 size = ImVec2(0, 0), bool scroll = false);
void endCard();
// Small rounded label, e.g. a "WRONG WAY" badge.  Returns its size.
ImVec2 pill(const char* text, ImU32 fg, ImU32 bg, ImFont* font = nullptr);
ImVec2 pillSize(const char* text, ImFont* font = nullptr);
// Primary (filled accent) and ghost (outlined) buttons with a leading icon.
bool primaryButton(const char* label, ImVec2 size = ImVec2(0, 0), ImU32 color = c::accent);
bool ghostButton(const char* label, ImVec2 size = ImVec2(0, 0));
bool iconButton(const char* icon, const char* tooltip, bool active = false, float size = 0);
// Segmented control; returns true when the selection changed.
bool segmented(const char* id, int* value, const char* const labels[], int count, float width = 0);
// Big number tile: caption, value, optional sub-caption, accent colour.
void statTile(const char* icon, const char* caption, const std::string& value, const char* sub, ImU32 color, ImVec2 size);
void sectionLabel(const char* text);
void helpMarker(const char* text);
void dimText(const char* fmt, ...);
// Label on the left, widget after it at a fixed column, for inspector forms.
void formLabel(const char* label, float column);

}  // namespace theme
