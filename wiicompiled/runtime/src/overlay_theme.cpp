#include "overlay_theme.h"

#include <aurora/aurora.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <string>

// runtime/assets/fonts, embedded by runtime/cmake/embed_fonts.cmake.
extern const unsigned char kFont_LuckiestGuyRegular[];
extern const std::size_t kFont_LuckiestGuyRegularSize;
extern const unsigned char kFont_RobotoMedium[];
extern const std::size_t kFont_RobotoMediumSize;

namespace OverlayTheme {
namespace {

float g_scale = 1.0f;
ImFont* g_body = nullptr;
ImFont* g_small = nullptr;
ImFont* g_heading = nullptr;
ImFont* g_title = nullptr;
ImFont* g_row = nullptr;
ImFont* g_tab = nullptr;

// launcher/Theme.cs, so the game's menus and the launcher look like one thing.
constexpr ImU32 kColours[] = {
    IM_COL32(0x12, 0x0A, 0x1C, 255),  // Ink
    IM_COL32(0xF6, 0xE7, 0xB8, 255),  // Cream
    IM_COL32(0xC9, 0xBC, 0x98, 255),  // CreamDim
    IM_COL32(0xC8, 0xE6, 0x4A, 255),  // Lime
    IM_COL32(0x9C, 0xB8, 0x34, 255),  // LimeDeep
    IM_COL32(0xFF, 0xD4, 0x6B, 255),  // Gold
    IM_COL32(0x6E, 0x42, 0xAC, 255),  // Purple
    IM_COL32(0x46, 0x25, 0x7C, 255),  // PurpleDeep
    IM_COL32(0x2A, 0x19, 0x46, 255),  // BoardTop
    IM_COL32(0x1B, 0x10, 0x30, 255),  // BoardBottom
    IM_COL32(0x13, 0x0A, 0x24, 255),  // Field
    IM_COL32(0xA6, 0xE3, 0x7E, 255),  // Good
    IM_COL32(0xF4, 0xA8, 0x5E, 255),  // Warn
};

ImU32 Lighten(ImU32 c, float amount) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.x += (1.0f - v.x) * amount;
    v.y += (1.0f - v.y) * amount;
    v.z += (1.0f - v.z) * amount;
    return ImGui::ColorConvertFloat4ToU32(v);
}

ImFont* AddFont(const unsigned char* data, std::size_t size, float pixels) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // static data
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size),
                                                     std::round(pixels * g_scale), &config);
}

void SetStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(10, 6);
    style.CellPadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 22;
    style.ScrollbarSize = 14;
    style.GrabMinSize = 14;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 2;
    style.TabBarOverlineSize = 2;
    style.WindowRounding = 12;
    style.ChildRounding = 8;
    style.FrameRounding = 7;
    style.PopupRounding = 8;
    style.ScrollbarRounding = 9;
    style.GrabRounding = 6;
    style.TabRounding = 7;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.SeparatorTextBorderSize = 2;
    style.SeparatorTextPadding = ImVec2(14, 4);
    style.DisabledAlpha = 0.45f;

    ImVec4* c = style.Colors;
    const auto v = [](Colour colour, float alpha = 1.0f) { return Vec(colour, alpha); };
    c[ImGuiCol_Text] = v(Colour::Cream);
    c[ImGuiCol_TextDisabled] = v(Colour::CreamDim, 0.78f);
    c[ImGuiCol_WindowBg] = v(Colour::BoardBottom, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(Colour::BoardBottom, 0.98f);
    c[ImGuiCol_Border] = v(Colour::Purple, 0.55f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v(Colour::Field);
    c[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x2A, 0x1A, 0x48, 255));
    c[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x3A, 0x25, 0x60, 255));
    c[ImGuiCol_TitleBg] = v(Colour::Ink);
    c[ImGuiCol_TitleBgActive] = v(Colour::PurpleDeep);
    c[ImGuiCol_TitleBgCollapsed] = v(Colour::Ink, 0.8f);
    c[ImGuiCol_MenuBarBg] = v(Colour::BoardTop);
    c[ImGuiCol_ScrollbarBg] = v(Colour::Ink, 0.35f);
    c[ImGuiCol_ScrollbarGrab] = v(Colour::PurpleDeep);
    c[ImGuiCol_ScrollbarGrabHovered] = v(Colour::Purple);
    c[ImGuiCol_ScrollbarGrabActive] = v(Colour::Lime, 0.8f);
    c[ImGuiCol_CheckMark] = v(Colour::Lime);
    c[ImGuiCol_SliderGrab] = v(Colour::Lime);
    c[ImGuiCol_SliderGrabActive] = v(Colour::Gold);
    c[ImGuiCol_Button] = v(Colour::Purple);
    c[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(Lighten(Col(Colour::Purple), 0.18f));
    c[ImGuiCol_ButtonActive] = v(Colour::PurpleDeep);
    c[ImGuiCol_Header] = v(Colour::Purple, 0.55f);
    c[ImGuiCol_HeaderHovered] = v(Colour::Purple, 0.85f);
    c[ImGuiCol_HeaderActive] = v(Colour::LimeDeep, 0.85f);
    c[ImGuiCol_Separator] = v(Colour::PurpleDeep);
    c[ImGuiCol_SeparatorHovered] = v(Colour::Purple);
    c[ImGuiCol_SeparatorActive] = v(Colour::Lime);
    c[ImGuiCol_ResizeGrip] = v(Colour::Purple, 0.35f);
    c[ImGuiCol_ResizeGripHovered] = v(Colour::Purple, 0.8f);
    c[ImGuiCol_ResizeGripActive] = v(Colour::Lime);
    c[ImGuiCol_Tab] = v(Colour::PurpleDeep);
    c[ImGuiCol_TabHovered] = v(Colour::Purple);
    c[ImGuiCol_TabSelected] = v(Colour::Purple);
    c[ImGuiCol_TabSelectedOverline] = v(Colour::Lime);
    c[ImGuiCol_TabDimmed] = v(Colour::PurpleDeep, 0.8f);
    c[ImGuiCol_TabDimmedSelected] = v(Colour::Purple, 0.8f);
    c[ImGuiCol_TabDimmedSelectedOverline] = v(Colour::LimeDeep, 0.6f);
    c[ImGuiCol_PlotLines] = v(Colour::Lime);
    c[ImGuiCol_PlotLinesHovered] = v(Colour::Gold);
    c[ImGuiCol_PlotHistogram] = v(Colour::Lime);
    c[ImGuiCol_PlotHistogramHovered] = v(Colour::Gold);
    c[ImGuiCol_TableHeaderBg] = v(Colour::PurpleDeep);
    c[ImGuiCol_TableBorderStrong] = v(Colour::Purple, 0.6f);
    c[ImGuiCol_TableBorderLight] = v(Colour::PurpleDeep, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = v(Colour::Cream, 0.04f);
    c[ImGuiCol_TextLink] = v(Colour::Lime);
    c[ImGuiCol_TextSelectedBg] = v(Colour::Purple, 0.6f);
    c[ImGuiCol_DragDropTarget] = v(Colour::Gold);
    c[ImGuiCol_NavCursor] = v(Colour::Lime);
    c[ImGuiCol_NavWindowingHighlight] = v(Colour::Lime, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = v(Colour::Ink, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = v(Colour::Ink, 0.6f);
    style.ScaleAllSizes(g_scale);
}

} // namespace

void Initialize(const AuroraWindowSize* size) {
    g_scale = size != nullptr && size->scale > 0.0f ? std::clamp(size->scale, 1.0f, 3.0f) : 1.0f;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_body = AddFont(kFont_RobotoMedium, kFont_RobotoMediumSize, 17.0f);
    g_small = AddFont(kFont_RobotoMedium, kFont_RobotoMediumSize, 14.0f);
    g_heading = AddFont(kFont_LuckiestGuyRegular, kFont_LuckiestGuyRegularSize, 22.0f);
    g_title = AddFont(kFont_LuckiestGuyRegular, kFont_LuckiestGuyRegularSize, 40.0f);
    g_row = AddFont(kFont_RobotoMedium, kFont_RobotoMediumSize, 20.0f);
    g_tab = AddFont(kFont_LuckiestGuyRegular, kFont_LuckiestGuyRegularSize, 27.0f);
    io.FontDefault = g_body;
    SetStyle();
}

float Scale() { return g_scale; }
ImFont* Body() { return g_body; }
ImFont* Small() { return g_small != nullptr ? g_small : ImGui::GetFont(); }
ImFont* Heading() { return g_heading != nullptr ? g_heading : ImGui::GetFont(); }
ImFont* Title() { return g_title != nullptr ? g_title : ImGui::GetFont(); }
ImFont* Row() { return g_row != nullptr ? g_row : ImGui::GetFont(); }
ImFont* Tab() { return g_tab != nullptr ? g_tab : Heading(); }

ImU32 Col(Colour colour, float alpha) {
    const ImU32 c = kColours[static_cast<int>(colour)];
    const auto a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

ImVec4 Vec(Colour colour, float alpha) { return ImGui::ColorConvertU32ToFloat4(Col(colour, alpha)); }

ImU32 UiCol(Colour colour, float alpha) { return ImGui::GetColorU32(Vec(colour, alpha)); }

void Card(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    list->AddRectFilled(min, max, UiCol(Colour::BoardBottom, 0.92f * alpha), rounding);
    list->AddRect(min, max, UiCol(Colour::Purple, 0.55f * alpha), rounding, 0, Px(1));
}

bool FlatButton(const char* label, bool primary, ImVec2 size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiID id = window->GetID(label);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    ImDrawList* list = window->DrawList;
    const float rounding = size.y * 0.5f;
    if (primary) {
        list->AddRectFilled(bb.Min, bb.Max, UiCol(hovered ? Colour::Lime : Colour::LimeDeep), rounding);
    } else {
        list->AddRectFilled(bb.Min, bb.Max, UiCol(Colour::Purple, hovered ? 0.55f : 0.3f), rounding);
        list->AddRect(bb.Min, bb.Max, UiCol(Colour::Purple), rounding, 0, Px(1.5f));
    }
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImFont* font = Heading();
    const ImVec2 text = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label, end);
    list->AddText(font, font->FontSize,
                  ImVec2(std::round((bb.Min.x + bb.Max.x - text.x) * 0.5f), std::round((bb.Min.y + bb.Max.y - text.y) * 0.5f + Px(1))),
                  primary ? UiCol(Colour::Ink) : UiCol(Colour::Cream), label, end);
    ImGui::RenderNavCursor(bb, id);
    return pressed;
}

void OutlinedText(ImDrawList* list, ImFont* font, ImVec2 pos, ImU32 fill, ImU32 outline, float thickness,
                  const char* text) {
    const float size = font->FontSize;
    if (thickness > 0.0f) {
        constexpr int kSteps = 12;
        for (int i = 0; i < kSteps; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / kSteps;
            list->AddText(font, size, ImVec2(pos.x + std::cos(a) * thickness, pos.y + std::sin(a) * thickness), outline,
                          text);
        }
    }
    list->AddText(font, size, pos, fill, text);
}

void Board(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    // Drop shadow, then the gradient body, then the edges.
    for (int i = 3; i >= 1; --i) {
        const float grow = Px(4.0f * static_cast<float>(i));
        list->AddRectFilled(ImVec2(min.x - grow, min.y - grow + Px(6)), ImVec2(max.x + grow, max.y + grow + Px(6)),
                            Col(Colour::Ink, 0.12f), rounding + grow);
    }
    const int start = list->VtxBuffer.Size;
    list->AddRectFilled(min, max, Col(Colour::BoardTop, alpha), rounding);  // the gradient below keeps this alpha
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(list, start, list->VtxBuffer.Size, min, ImVec2(min.x, max.y),
                                                  Col(Colour::BoardTop), Col(Colour::BoardBottom));
    list->AddRect(min, max, Col(Colour::Ink), rounding, 0, Px(3));
    list->AddRect(ImVec2(min.x + Px(3), min.y + Px(3)), ImVec2(max.x - Px(3), max.y - Px(3)),
                  Col(Colour::Lime, 0.35f), std::max(0.0f, rounding - Px(3)), 0, Px(1));
}

namespace {
// `lettering` 0 = ink without an outline, for the light planks.
bool DrawPlank(const char* label, ImVec2 size, ImU32 top, ImU32 bottom, ImU32 edge, ImU32 lettering) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiID id = window->GetID(label);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;

    ImDrawList* list = window->DrawList;
    if (hovered && !disabled) {
        top = Lighten(top, 0.2f);
        bottom = Lighten(bottom, 0.2f);
    }
    ImVec2 a = bb.Min, b = bb.Max;
    if (held) {
        a.y += Px(1);
        b.y += Px(1);
    }
    const float rounding = Px(7);
    const int start = list->VtxBuffer.Size;
    list->AddRectFilled(a, b, top, rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(list, start, list->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, bottom);
    // Wood grain: two faint lines across the plank.
    for (int i = 1; i < 3; ++i) {
        const float y = a.y + (b.y - a.y) * static_cast<float>(i) / 3.0f;
        list->AddLine(ImVec2(a.x + Px(10), y), ImVec2(b.x - Px(10), y + Px(1)), Col(Colour::Ink, 0.16f), Px(1));
    }
    list->AddRect(a, b, edge, rounding, 0, Px(2));
    // Nails.
    const float nail = Px(2.2f);
    for (const float x : {a.x + Px(9), b.x - Px(9)})
        list->AddCircleFilled(ImVec2(x, (a.y + b.y) * 0.5f), nail, Col(Colour::Ink, 0.55f));

    const char* end = ImGui::FindRenderedTextEnd(label);
    ImFont* font = Heading();
    const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label, end);
    const ImVec2 textPos(std::round((a.x + b.x - textSize.x) * 0.5f), std::round((a.y + b.y - textSize.y) * 0.5f + Px(1)));
    std::string text(label, end);
    if (lettering == 0) {
        list->AddText(font, font->FontSize, textPos, Col(Colour::Ink), text.c_str());
    } else {
        OutlinedText(list, font, textPos, lettering, Col(Colour::Ink), Px(2), text.c_str());
    }
    ImGui::RenderNavCursor(bb, id);
    return pressed;
}
} // namespace

bool Plank(const char* label, bool selected, ImVec2 size) {
    return selected ? DrawPlank(label, size, Col(Colour::Lime), Col(Colour::LimeDeep), Col(Colour::Ink), 0)
                    : DrawPlank(label, size, Col(Colour::Purple), Col(Colour::PurpleDeep), Col(Colour::Ink), Col(Colour::Cream));
}

bool ActionPlank(const char* label, Colour colour, ImVec2 size) {
    return DrawPlank(label, size, Col(Colour::BoardTop), Col(Colour::Field), Col(colour), Col(colour));
}

bool Toggle(const char* label, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, end, true);
    const float height = ImGui::GetFrameHeight() * 0.8f;
    const float width = height * 1.9f;
    const ImVec2 pos = window->DC.CursorPos;
    const float frame = ImGui::GetFrameHeight();
    const ImRect bb(pos, ImVec2(pos.x + width + (labelSize.x > 0 ? style.ItemInnerSpacing.x + labelSize.x : 0.0f),
                                pos.y + frame));
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }
    const float t = *value ? 1.0f : 0.0f;
    ImDrawList* list = window->DrawList;
    const ImVec2 a(pos.x, pos.y + (frame - height) * 0.5f);
    const ImVec2 b(a.x + width, a.y + height);
    ImU32 track = *value ? Col(Colour::LimeDeep) : Col(Colour::Field);
    if (hovered) track = Lighten(track, 0.12f);
    list->AddRectFilled(a, b, track, height * 0.5f);
    list->AddRect(a, b, *value ? Col(Colour::Lime) : Col(Colour::Purple, 0.8f), height * 0.5f, 0, Px(1.5f));
    const float r = height * 0.5f - Px(3);
    const ImVec2 knob(a.x + height * 0.5f + t * (width - height), a.y + height * 0.5f);
    list->AddCircleFilled(knob, r, *value ? Col(Colour::Cream) : Col(Colour::CreamDim, 0.85f));
    ImGui::RenderNavCursor(ImRect(a, b), id);
    if (labelSize.x > 0.0f)
        ImGui::RenderText(ImVec2(b.x + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y), label, end);
    return pressed;
}

void SectionTitle(const char* text) {
    ImGui::Dummy(ImVec2(0, Px(4)));
    ImGui::PushFont(Heading());
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::Lime));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float y = max.y + Px(2);
    list->AddLine(ImVec2(min.x, y), ImVec2(right, y), Col(Colour::Purple, 0.7f), Px(2));
    list->AddLine(ImVec2(min.x, y), ImVec2(std::min(right, min.x + Px(60)), y), Col(Colour::Lime), Px(2));
    ImGui::Dummy(ImVec2(0, Px(4)));
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void RowLabel(const char* label, float labelWidth, float controlWidth) {
    // From where the row starts, not the window's edge: a page centred in a wide panel starts well inside it.
    const float rowStart = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(rowStart + labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
}

} // namespace OverlayTheme
