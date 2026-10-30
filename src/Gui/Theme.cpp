#include "Theme.h"
#include "Icons.h"
#include <imgui_internal.h>
#include <cstdarg>
#include <cstdio>

extern const unsigned char font_inter_regular[];
extern const unsigned font_inter_regular_size;
extern const unsigned char font_inter_semibold[];
extern const unsigned font_inter_semibold_size;
extern const unsigned char font_inter_bold[];
extern const unsigned font_inter_bold_size;
extern const unsigned char font_fa_solid[];
extern const unsigned font_fa_solid_size;

namespace theme {

Fonts fonts;
static float gScale = 1.f;
float scale() { return gScale; }

ImVec4 v4(ImU32 col, float a) {
	ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
	if (a >= 0) v.w = a;
	return v;
}

ImU32 alpha(ImU32 col, float a) {
	return (col & ~IM_COL32_A_MASK) | (ImU32(a * 255.f + .5f) << IM_COL32_A_SHIFT);
}

ImU32 speedColor(int mph, int limit, int egregious) {
	if (mph >= egregious) return c::bad;
	if (mph > limit) return c::warn;
	return c::ok;
}

// The atlas holds Latin text plus the arrows, dashes and math signs the UI uses (← → ≤ × · …), and every
// Font Awesome glyph merged into the text fonts so icons can sit inline in labels.
static const ImWchar* textRanges() {
	static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0x2030, 0x205E, 0x2190, 0x21FF, 0x2212, 0x2265, 0x25A0, 0x25FF, 0 };
	return ranges;
}

static float gRaster = 1.f;

static ImFont* addFont(const unsigned char* data, unsigned size, float px, bool withIcons, float iconScale = .9f) {
	px *= gRaster;
	ImGuiIO& io = ImGui::GetIO();
	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;   // data lives in the executable
	cfg.OversampleH = 2;
	cfg.OversampleV = 1;
	ImFont* f = io.Fonts->AddFontFromMemoryTTF((void*)data, int(size), px, &cfg, textRanges());
	if (withIcons) {
		static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
		ImFontConfig ic;
		ic.FontDataOwnedByAtlas = false;
		ic.MergeMode = true;
		ic.PixelSnapH = true;
		ic.GlyphMinAdvanceX = px * 1.15f;   // fixed-width icons line up in lists and menus
		ic.GlyphOffset = ImVec2(0, px * .06f);
		io.Fonts->AddFontFromMemoryTTF((void*)font_fa_solid, int(font_fa_solid_size), px * iconScale, &ic, iconRanges);
	}
	return f;
}

void setup(float s, float raster) {
	gScale = s;
	gRaster = raster;
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();
	io.FontGlobalScale = 1.f / raster;
	fonts.body = addFont(font_inter_regular, font_inter_regular_size, px(15.5f), true);
	fonts.strong = addFont(font_inter_semibold, font_inter_semibold_size, px(15.5f), true);
	fonts.caption = addFont(font_inter_regular, font_inter_regular_size, px(13.f), true);
	fonts.title = addFont(font_inter_semibold, font_inter_semibold_size, px(22.f), true);
	fonts.stat = addFont(font_inter_bold, font_inter_bold_size, px(30.f), false);
	fonts.hero = addFont(font_inter_bold, font_inter_bold_size, px(54.f), false);
	{
		static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
		ImFontConfig ic;
		ic.FontDataOwnedByAtlas = false;
		fonts.icons = io.Fonts->AddFontFromMemoryTTF((void*)font_fa_solid, int(font_fa_solid_size), px(30.f) * raster, &ic, iconRanges);
	}
	io.FontDefault = fonts.body;

	ImGuiStyle& st = ImGui::GetStyle();
	st = ImGuiStyle();
	st.WindowPadding = ImVec2(16, 16);
	st.FramePadding = ImVec2(10, 7);
	st.CellPadding = ImVec2(10, 7);
	st.ItemSpacing = ImVec2(10, 9);
	st.ItemInnerSpacing = ImVec2(8, 6);
	st.IndentSpacing = 18;
	st.ScrollbarSize = 10;
	st.GrabMinSize = 12;
	st.WindowBorderSize = 0;
	st.ChildBorderSize = 0;
	st.PopupBorderSize = 1;
	st.FrameBorderSize = 0;
	st.WindowRounding = 10;
	st.ChildRounding = 12;
	st.FrameRounding = 7;
	st.PopupRounding = 10;
	st.ScrollbarRounding = 8;
	st.GrabRounding = 6;
	st.TabRounding = 7;
	st.SeparatorTextBorderSize = 1;
	st.SelectableTextAlign = ImVec2(0, .5f);
	st.ScaleAllSizes(s);

	ImVec4* col = st.Colors;
	col[ImGuiCol_Text] = v4(c::text);
	col[ImGuiCol_TextDisabled] = v4(c::faint);
	col[ImGuiCol_WindowBg] = v4(c::bg0);
	col[ImGuiCol_ChildBg] = v4(c::bg2);
	col[ImGuiCol_PopupBg] = v4(c::bg2, .98f);
	col[ImGuiCol_Border] = v4(c::border);
	col[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
	col[ImGuiCol_FrameBg] = v4(c::bg3);
	col[ImGuiCol_FrameBgHovered] = v4(c::rgb(0x283249));
	col[ImGuiCol_FrameBgActive] = v4(c::rgb(0x2E3A55));
	col[ImGuiCol_TitleBg] = v4(c::bg1);
	col[ImGuiCol_TitleBgActive] = v4(c::bg1);
	col[ImGuiCol_MenuBarBg] = v4(c::bg1);
	col[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	col[ImGuiCol_ScrollbarGrab] = v4(c::border);
	col[ImGuiCol_ScrollbarGrabHovered] = v4(c::faint);
	col[ImGuiCol_ScrollbarGrabActive] = v4(c::dim);
	col[ImGuiCol_CheckMark] = v4(c::accent);
	col[ImGuiCol_SliderGrab] = v4(c::accent);
	col[ImGuiCol_SliderGrabActive] = v4(c::accent2);
	col[ImGuiCol_Button] = v4(c::bg3);
	col[ImGuiCol_ButtonHovered] = v4(c::rgb(0x2A3550));
	col[ImGuiCol_ButtonActive] = v4(c::rgb(0x33405F));
	col[ImGuiCol_Header] = v4(c::accent, .16f);
	col[ImGuiCol_HeaderHovered] = v4(c::accent, .22f);
	col[ImGuiCol_HeaderActive] = v4(c::accent, .30f);
	col[ImGuiCol_Separator] = v4(c::border);
	col[ImGuiCol_SeparatorHovered] = v4(c::accent);
	col[ImGuiCol_SeparatorActive] = v4(c::accent);
	col[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
	col[ImGuiCol_Tab] = v4(c::bg2);
	col[ImGuiCol_TabHovered] = v4(c::bg3);
	col[ImGuiCol_TabSelected] = v4(c::bg3);
	col[ImGuiCol_TableHeaderBg] = v4(c::bg1);
	col[ImGuiCol_TableBorderStrong] = v4(c::border);
	col[ImGuiCol_TableBorderLight] = v4(c::border, .5f);
	col[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
	col[ImGuiCol_TableRowBgAlt] = v4(c::rgb(0xFFFFFF), .018f);
	col[ImGuiCol_TextSelectedBg] = v4(c::accent, .35f);
	col[ImGuiCol_PlotHistogram] = v4(c::accent);
	col[ImGuiCol_PlotLines] = v4(c::accent2);
	col[ImGuiCol_NavCursor] = v4(c::accent);
	col[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, .55f);
}

bool beginCard(const char* id, const char* icon, const char* title, ImVec2 size, bool scroll) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(c::bg2));
	ImGuiWindowFlags flags = scroll ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGuiChildFlags cf = ImGuiChildFlags_AlwaysUseWindowPadding;
	if (size.y == 0) cf |= ImGuiChildFlags_AutoResizeY;
	bool open = ImGui::BeginChild(id, size, cf, flags);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	// Hairline border, drawn rather than using ChildBorderSize so it can be softer than other borders.
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p0 = ImGui::GetWindowPos(), p1 = p0 + ImGui::GetWindowSize();
	dl->AddRect(p0, p1, alpha(c::border, .8f), ImGui::GetStyle().ChildRounding, 0, 1.f);
	if (title) {
		ImGui::PushFont(fonts.strong);
		if (icon) {
			ImGui::PushStyleColor(ImGuiCol_Text, v4(c::accent));
			ImGui::TextUnformatted(icon);
			ImGui::PopStyleColor();
			ImGui::SameLine();
		}
		ImGui::TextUnformatted(title);
		ImGui::PopFont();
		ImGui::Dummy(ImVec2(0, px(2)));
	}
	return open;
}

void endCard() { ImGui::EndChild(); }

ImVec2 pillSize(const char* text, ImFont* font) {
	if (!font) font = fonts.caption;
	ImVec2 ts = font->CalcTextSizeA(fsz(font), FLT_MAX, 0, text);
	return ImVec2(ts.x + px(16), ts.y + px(6));
}

ImVec2 pill(const char* text, ImU32 fg, ImU32 bg, ImFont* font) {
	if (!font) font = fonts.caption;
	ImVec2 size = pillSize(text, font);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + size, bg, size.y * .5f);
	dl->AddText(font, fsz(font), p + ImVec2(px(8), px(3)), fg, text);
	ImGui::Dummy(size);
	return size;
}

static bool colouredButton(const char* label, ImVec2 size, ImU32 base, ImU32 textCol, bool outline) {
	ImGui::PushStyleColor(ImGuiCol_Button, outline ? v4(c::bg2) : v4(base));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, outline ? v4(c::bg3) : v4(alpha(base, .88f)));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, outline ? v4(c::rgb(0x2A3550)) : v4(alpha(base, .75f)));
	ImGui::PushStyleColor(ImGuiCol_Text, v4(textCol));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, outline ? 1.f : 0.f);
	ImGui::PushStyleColor(ImGuiCol_Border, v4(c::border));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(14), px(8)));
	ImGui::PushFont(fonts.strong);
	bool r = ImGui::Button(label, size);
	ImGui::PopFont();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(5);
	return r;
}

bool primaryButton(const char* label, ImVec2 size, ImU32 color) { return colouredButton(label, size, color, c::rgb(0xFFFFFF), false); }
bool ghostButton(const char* label, ImVec2 size) { return colouredButton(label, size, c::bg2, c::text, true); }

bool iconButton(const char* icon, const char* tooltip, bool active, float size) {
	if (size <= 0) size = ImGui::GetFrameHeight();
	ImGui::PushStyleColor(ImGuiCol_Button, active ? v4(c::accent, .25f) : ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, active ? v4(c::accent) : v4(c::text));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
	bool r = ImGui::Button(icon, ImVec2(size, size));
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
	if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
	return r;
}

bool segmented(const char* id, int* value, const char* const labels[], int count, float width) {
	ImGui::PushID(id);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float h = ImGui::GetFrameHeight();
	if (width <= 0) width = ImGui::GetContentRegionAvail().x;
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, p + ImVec2(width, h), c::bg3, ImGui::GetStyle().FrameRounding);
	float w = width / count;
	bool changed = false;
	for (int i = 0; i < count; i++) {
		ImGui::SetCursorScreenPos(p + ImVec2(w * i, 0));
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("seg", ImVec2(w, h)) && *value != i) { *value = i; changed = true; }
		bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		ImVec2 a = p + ImVec2(w * i + 2, 2), b = p + ImVec2(w * (i + 1) - 2, h - 2);
		if (*value == i) dl->AddRectFilled(a, b, c::accent, ImGui::GetStyle().FrameRounding - 1);
		else if (hovered) dl->AddRectFilled(a, b, alpha(c::text, .06f), ImGui::GetStyle().FrameRounding - 1);
		ImFont* f = fonts.strong;
		ImVec2 ts = f->CalcTextSizeA(fsz(f), FLT_MAX, 0, labels[i]);
		dl->AddText(f, fsz(f), ImVec2((a.x + b.x - ts.x) * .5f, (a.y + b.y - ts.y) * .5f), *value == i ? c::rgb(0xFFFFFF) : c::dim, labels[i]);
	}
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(width, h));
	ImGui::PopID();
	return changed;
}

void statTile(const char* icon, const char* caption, const std::string& value, const char* sub, ImU32 color, ImVec2 size) {
	ImGui::PushID(caption);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float r = ImGui::GetStyle().ChildRounding;
	dl->AddRectFilled(p, p + size, c::bg2, r);
	dl->AddRect(p, p + size, alpha(c::border, .8f), r);
	// A soft glow of the tile's colour in the corner gives each metric its identity without shouting.
	dl->AddRectFilledMultiColor(p + ImVec2(1, 1), p + ImVec2(size.x * .6f, size.y - 1), alpha(color, .10f), alpha(color, 0), alpha(color, 0), alpha(color, .04f));
	float pad = px(14);
	ImFont* sf = fonts.caption;
	std::string head = std::string(icon) + "  " + caption;
	dl->AddText(sf, fsz(sf), p + ImVec2(pad, pad), alpha(color, .95f), head.c_str());
	ImFont* nf = fonts.stat;
	dl->AddText(nf, fsz(nf), p + ImVec2(pad, pad + fsz(sf) + px(4)), c::text, value.c_str());
	if (sub) dl->AddText(sf, fsz(sf), p + ImVec2(pad, size.y - pad - fsz(sf)), c::dim, sub);
	ImGui::Dummy(size);
	ImGui::PopID();
}

void sectionLabel(const char* text) {
	ImGui::Dummy(ImVec2(0, px(2)));
	ImGui::PushFont(fonts.caption);
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

void helpMarker(const char* text) {
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::faint));
	ImGui::TextUnformatted(ICON_INFO);
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(px(340));
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

void dimText(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextWrappedV(fmt, args);
	ImGui::PopStyleColor();
	va_end(args);
}

void formLabel(const char* label, float column) {
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, v4(c::dim));
	ImGui::TextUnformatted(label);
	ImGui::PopStyleColor();
	ImGui::SameLine(column);
	ImGui::SetNextItemWidth(-FLT_MIN);
}

}  // namespace theme
