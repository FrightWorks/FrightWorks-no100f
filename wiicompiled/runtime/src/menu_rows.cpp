#include "menu_rows.h"

#include "overlay_theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace MenuRows {
namespace {

using namespace OverlayTheme;

struct State {
    bool input = false;
    int selected = 0;
    int next = 0;    // rows drawn so far this frame
    int count = 0;   // rows drawn last frame
    int adjust = 0;      // left / right this frame, at the menu's pace (choices, toggles)
    int adjustFast = 0;  // the same at the slider pace
    bool activate = false;
    bool second = false;
    bool scroll = false;  // bring the selected row into view
    bool pad = false;
    bool lastRowSelected = false;
    Kind selectedKind = Kind::None;
    std::string title;
    std::string text;
    // A section title right above the next row: scrolled into view with it.
    float sectionTop = 0.0f;
    int sectionRow = -1;
} s;

// Keys the list reads itself while it has input (ImGui's navigation must not act on them too).
constexpr ImGuiKey kOwnedKeys[] = {
    ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
    ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
    ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
    ImGuiKey_GamepadL1, ImGuiKey_GamepadR1, ImGuiKey_GamepadStart,
    ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
    ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_Q, ImGuiKey_E, ImGuiKey_PageUp, ImGuiKey_PageDown,
};

constexpr ImGuiInputFlags kMoveRepeat = ImGuiInputFlags_Repeat | ImGuiInputFlags_RepeatRateNavMove;
constexpr ImGuiInputFlags kTweakRepeat = ImGuiInputFlags_Repeat | ImGuiInputFlags_RepeatRateNavTweak;

// The keys' owner: fixed, so it doesn't depend on the window Begin runs in.
const ImGuiID kOwner = ImHashStr("FrightWorksMenuRows");

bool KeyPressed(ImGuiKey key, ImGuiInputFlags flags) { return ImGui::IsKeyPressed(key, flags, kOwner); }

// Hands the keys back (to a prompt's ImGui navigation, the dev menu, a text field).
void ReleaseKeys() {
    for (const ImGuiKey key : kOwnedKeys) {
        if (ImGui::GetKeyOwner(key) == kOwner) ImGui::SetKeyOwner(key, ImGuiKeyOwner_NoOwner);
    }
}

int Direction(ImGuiKey less, ImGuiKey lessStick, ImGuiKey lessKey, ImGuiKey more, ImGuiKey moreStick, ImGuiKey moreKey,
              ImGuiInputFlags flags) {
    const bool down = KeyPressed(less, flags) || KeyPressed(lessStick, flags) || KeyPressed(lessKey, flags);
    const bool up = KeyPressed(more, flags) || KeyPressed(moreStick, flags) || KeyPressed(moreKey, flags);
    return (up ? 1 : 0) - (down ? 1 : 0);
}

float RowHeight() { return std::round(Row()->FontSize + Px(26)); }

// The value column on the right of a row.
float ValueWidth(float rowWidth) { return std::clamp(rowWidth * 0.46f, Px(210), Px(380)); }

struct RowFrame {
    ImGuiID id = 0;
    ImRect bb;
    ImRect value;
    bool selected = false;
    bool hovered = false;
    bool held = false;
    bool clicked = false;   // released over the row (a click)
    bool activate = false;  // A / Enter / a click
    int adjust = 0;
    bool disabled = false;
};

std::string Visible(const char* label) { return std::string(label, ImGui::FindRenderedTextEnd(label)); }

// Lays out, numbers and draws the background and name of a row. False when it is out of view (nothing to draw), but
// the row still counts and still takes its input.
bool BeginRow(const char* label, Kind kind, RowFrame& r, ImGuiButtonFlags flags = 0, bool allowOverlap = false) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const int index = s.next++;
    r.selected = index == s.selected;
    r.disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    s.lastRowSelected = r.selected;
    if (window->SkipItems) return false;
    r.id = window->GetID(label);
    const ImVec2 pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    r.bb = ImRect(pos, ImVec2(pos.x + width, pos.y + RowHeight()));
    const float valueWidth = ValueWidth(width);
    r.value = ImRect(ImVec2(r.bb.Max.x - Px(16) - valueWidth, r.bb.Min.y), ImVec2(r.bb.Max.x - Px(16), r.bb.Max.y));
    ImGui::ItemSize(r.bb.GetSize());
    if (r.selected && s.scroll) {
        ImRect target = r.bb;
        if (s.sectionRow == index) target.Min.y = std::min(target.Min.y, s.sectionTop);
        if (index == 0) target.Min.y = window->DC.CursorStartPos.y - window->WindowPadding.y;  // the first row: the very top
        ImGui::ScrollToRectEx(window, target, ImGuiScrollFlags_KeepVisibleEdgeY);
        s.scroll = false;
    }
    if (allowOverlap) ImGui::SetNextItemAllowOverlap();
    const bool visible = ImGui::ItemAdd(r.bb, r.id);
    if (visible) {
        r.clicked = ImGui::ButtonBehavior(r.bb, r.id, &r.hovered, &r.held, flags);
        // A greyed-out row takes no clicks, but pointing at it still selects it, so the details say why it's grey.
        if (r.disabled) r.hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r.bb.Min, r.bb.Max);
        // Pointing at a row selects it (only when the mouse moves: a scroll under a still mouse doesn't).
        const ImGuiIO& io = ImGui::GetIO();
        if (r.hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || r.clicked || r.held)) {
            s.selected = index;
            r.selected = true;
            s.lastRowSelected = true;
            s.pad = false;
        }
    }
    if (r.selected) {
        s.title = Visible(label);
        s.text.clear();
        s.selectedKind = kind;
    }
    r.activate = !r.disabled && ((r.selected && s.activate) || r.clicked);
    r.adjust = r.selected && !r.disabled ? s.adjust : 0;
    if (!visible) return false;

    ImDrawList* list = window->DrawList;
    const float rounding = Px(10);
    if (r.selected) {
        list->AddRectFilled(r.bb.Min, r.bb.Max, UiCol(Colour::Purple, 0.42f), rounding);
        list->AddRect(r.bb.Min, r.bb.Max, UiCol(Colour::Lime, 0.6f), rounding, 0, Px(1.5f));
        list->AddRectFilled(ImVec2(r.bb.Min.x + Px(5), r.bb.Min.y + Px(10)), ImVec2(r.bb.Min.x + Px(9), r.bb.Max.y - Px(10)),
                            UiCol(Colour::Lime), Px(2));
    } else if (r.hovered) {
        list->AddRectFilled(r.bb.Min, r.bb.Max, UiCol(Colour::Purple, 0.2f), rounding);
    }
    ImFont* font = Row();
    const std::string name = Visible(label);
    const float textY = std::round((r.bb.Min.y + r.bb.Max.y - font->FontSize) * 0.5f);
    const ImVec2 textMin(r.bb.Min.x + Px(22), textY);
    const ImVec2 textMax(r.value.Min.x - Px(12), textY + font->FontSize);
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Col(r.selected ? Colour::Cream : Colour::CreamDim)));
    ImGui::RenderTextEllipsis(list, textMin, textMax, textMax.x, textMax.x, name.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    return true;
}

void CentredText(ImDrawList* list, ImFont* font, const ImRect& box, ImU32 colour, const char* text, float dy = 0.0f) {
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
    const float maxWidth = box.GetWidth();
    if (size.x <= maxWidth) {
        list->AddText(font, font->FontSize,
                      ImVec2(std::round(box.Min.x + (maxWidth - size.x) * 0.5f), std::round((box.Min.y + box.Max.y - size.y) * 0.5f + dy)),
                      colour, text);
        return;
    }
    // Too long: from the left, cut with an ellipsis.
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(colour));
    const float y = std::round((box.Min.y + box.Max.y - size.y) * 0.5f + dy);
    ImGui::RenderTextEllipsis(list, ImVec2(box.Min.x, y), ImVec2(box.Max.x, y + size.y), box.Max.x, box.Max.x, text, nullptr, &size);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Arrow(ImDrawList* list, ImVec2 centre, float size, bool right, ImU32 colour) {
    const float h = size * 0.5f;
    const float w = size * 0.32f;
    if (right) {
        list->AddTriangleFilled(ImVec2(centre.x - w, centre.y - h), ImVec2(centre.x + w, centre.y), ImVec2(centre.x - w, centre.y + h), colour);
    } else {
        list->AddTriangleFilled(ImVec2(centre.x + w, centre.y - h), ImVec2(centre.x - w, centre.y), ImVec2(centre.x + w, centre.y + h), colour);
    }
}

// A value in a rounded box (bindings, statuses): returns its rectangle.
ImRect Pill(ImDrawList* list, ImVec2 min, ImVec2 max, const char* text, bool lit, bool ghost = false) {
    const ImRect box(min, max);
    const float rounding = box.GetHeight() * 0.5f;
    if (ghost) {
        list->AddRect(box.Min, box.Max, UiCol(Colour::CreamDim, 0.45f), rounding, 0, Px(1));
    } else {
        list->AddRectFilled(box.Min, box.Max, UiCol(Colour::Field, 0.9f), rounding);
        list->AddRect(box.Min, box.Max, UiCol(lit ? Colour::Lime : Colour::Purple, lit ? 0.8f : 0.7f), rounding, 0, Px(1.2f));
    }
    const ImRect inner(ImVec2(box.Min.x + Px(10), box.Min.y), ImVec2(box.Max.x - Px(10), box.Max.y));
    CentredText(list, Body(), inner, UiCol(ghost ? Colour::CreamDim : Colour::Cream, ghost ? 0.8f : 1.0f), text);
    return box;
}

} // namespace

void Begin(bool input) {
    ImGuiContext& g = *GImGui;
    s.input = input;
    s.next = 0;
    s.adjust = s.adjustFast = 0;
    s.activate = s.second = false;
    s.lastRowSelected = false;
    s.selectedKind = Kind::None;
    s.title.clear();
    s.text.clear();
    s.sectionRow = -1;
    if (!input) {
        ReleaseKeys();
        return;
    }
    for (const ImGuiKey key : kOwnedKeys) {
        // Not a key a finished rebind locked until it's released (its press was the binding).
        if (ImGui::GetKeyOwnerData(&g, key)->LockUntilRelease) continue;
        ImGui::SetKeyOwner(key, kOwner);
    }
    const int move = Direction(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_UpArrow, ImGuiKey_GamepadDpadDown,
                               ImGuiKey_GamepadLStickDown, ImGuiKey_DownArrow, kMoveRepeat);
    s.adjust = Direction(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadRight,
                         ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow, kMoveRepeat);
    s.adjustFast = Direction(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadRight,
                             ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow, kTweakRepeat);
    s.activate = KeyPressed(ImGuiKey_NavGamepadActivate, ImGuiInputFlags_None) || KeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None) ||
                 KeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None) || KeyPressed(ImGuiKey_Space, ImGuiInputFlags_None);
    s.second = KeyPressed(ImGuiKey_GamepadFaceLeft, ImGuiInputFlags_None);

    // Which prompts to show: the last thing used.
    for (int key = ImGuiKey_Gamepad_BEGIN; key < ImGuiKey_Gamepad_END; ++key) {
        if (key >= ImGuiKey_GamepadLStickLeft && key <= ImGuiKey_GamepadRStickDown) continue;  // a drifting stick isn't a use
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) s.pad = true;
    }
    if (ImGui::GetKeyData(ImGuiKey_GamepadLStickUp)->Down || ImGui::GetKeyData(ImGuiKey_GamepadLStickDown)->Down ||
        ImGui::GetKeyData(ImGuiKey_GamepadLStickLeft)->Down || ImGui::GetKeyData(ImGuiKey_GamepadLStickRight)->Down) {
        s.pad = true;
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_Gamepad_BEGIN; ++key) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) s.pad = false;
    }

    if (move != 0 && s.count > 0) {
        s.selected = std::clamp(s.selected + move, 0, s.count - 1);
        s.scroll = true;
    }
}

void End() {
    s.count = s.next;
    if (s.count > 0 && s.selected >= s.count) {
        s.selected = s.count - 1;
        s.scroll = true;
    }
}

void Select(int row) {
    s.selected = std::max(0, row);
    s.scroll = true;
}

int Selected() { return s.selected; }

void Release() {
    s.input = false;
    ReleaseKeys();
}

bool Pressed(ImGuiKey key, bool repeat) { return KeyPressed(key, repeat ? kMoveRepeat : ImGuiInputFlags_None); }

bool PadInput() { return s.pad; }
void SetPadInput(bool pad) { s.pad = pad; }

void Section(const char* title) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    const bool first = s.next == 0 && window->DC.CursorPos.y <= window->DC.CursorStartPos.y + 0.5f;
    if (!first) ImGui::Dummy(ImVec2(0, Px(10)));
    s.sectionTop = window->DC.CursorPos.y;
    s.sectionRow = s.next;
    ImGui::PushFont(Heading());
    const float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + Px(8));
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::Lime));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImDrawList* list = window->DrawList;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float right = window->WorkRect.Max.x;
    const float y = std::round((min.y + max.y) * 0.5f);
    list->AddLine(ImVec2(max.x + Px(12), y), ImVec2(right - Px(8), y), UiCol(Colour::Purple, 0.55f), Px(1));
}

void Note(const char* text) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    const float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + Px(22));
    ImGui::PushTextWrapPos(window->WorkRect.Max.x - window->Pos.x - Px(16));
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::CreamDim, 0.85f));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void Info(const char* label, const char* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    const ImVec2 pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = std::round(Body()->FontSize + Px(16));
    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    ImDrawList* list = window->DrawList;
    ImFont* font = Body();
    const float y = std::round((bb.Min.y + bb.Max.y - font->FontSize) * 0.5f);
    const float valueWidth = ValueWidth(width);
    const float valueX = bb.Max.x - Px(16) - valueWidth;
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::CreamDim));
    ImGui::RenderTextEllipsis(list, ImVec2(bb.Min.x + Px(22), y), ImVec2(valueX - Px(12), y + font->FontSize), valueX - Px(12),
                              valueX - Px(12), label, nullptr, nullptr);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::Cream));
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, value);
    const float right = bb.Max.x - Px(16);
    const float x = std::max(valueX, right - size.x);
    ImGui::RenderTextEllipsis(list, ImVec2(x, y), ImVec2(right, y + font->FontSize), right, right, value, nullptr, &size);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool Choice(const char* label, int* index, int count, const std::function<const char*(int)>& name,
            const std::function<bool(int)>& enabled) {
    RowFrame r;
    const bool visible = BeginRow(label, Kind::Choice, r);
    const int before = *index;
    int i = before;
    const auto step = [&](int direction) {
        if (count <= 0) return;
        for (int n = 0; n < count; ++n) {
            i = ((i + direction) % count + count) % count;
            if (!enabled || enabled(i)) break;
        }
    };
    const float arrowZone = Px(34);
    if (r.adjust != 0) step(r.adjust);
    if (r.activate) {
        // A click on the left arrow goes back; anywhere else on the row (and A) goes forward.
        const bool back = r.clicked && ImGui::GetIO().MousePos.x < r.value.Min.x + arrowZone && ImGui::GetIO().MousePos.x >= r.value.Min.x;
        step(back ? -1 : 1);
    }
    const bool changed = i != before;
    if (changed) {
        *index = i;
        ImGui::MarkItemEdited(r.id);
    }
    if (!visible) return changed;

    ImDrawList* list = ImGui::GetCurrentWindow()->DrawList;
    const bool pips = count > 1 && count <= 10;
    const float arrowSize = Px(14);
    const float midY = std::round((r.value.Min.y + r.value.Max.y) * 0.5f - (pips ? Px(4) : 0.0f));
    const ImU32 arrow = UiCol(r.selected ? Colour::Lime : Colour::CreamDim, r.selected ? 1.0f : 0.45f);
    Arrow(list, ImVec2(r.value.Min.x + arrowZone * 0.5f, midY), arrowSize, false, arrow);
    Arrow(list, ImVec2(r.value.Max.x - arrowZone * 0.5f, midY), arrowSize, true, arrow);
    const ImRect text(ImVec2(r.value.Min.x + arrowZone, r.value.Min.y), ImVec2(r.value.Max.x - arrowZone, r.value.Max.y));
    CentredText(list, Row(), text, UiCol(Colour::Cream), name(*index), pips ? -Px(4) : 0.0f);
    if (pips) {
        const float gap = Px(11);
        const float y = r.value.Max.y - Px(9);
        float x = std::round((r.value.Min.x + r.value.Max.x) * 0.5f - gap * static_cast<float>(count - 1) * 0.5f);
        for (int n = 0; n < count; ++n, x += gap) {
            const bool on = n == *index;
            list->AddCircleFilled(ImVec2(x, y), on ? Px(3) : Px(2.2f),
                                  on ? UiCol(Colour::Lime) : UiCol(Colour::CreamDim, enabled && !enabled(n) ? 0.15f : 0.4f));
        }
    }
    return changed;
}

bool Choice(const char* label, int* index, const char* const* items, int count) {
    return Choice(label, index, count, [&](int i) { return i >= 0 && i < count ? items[i] : "Custom"; });
}

bool Slider(const char* label, float* value, float min, float max, float step, const char* format) {
    RowFrame r;
    const bool visible = BeginRow(label, Kind::Slider, r, ImGuiButtonFlags_PressedOnClick);
    const float before = *value;
    float v = before;
    const int keys = r.selected && !r.disabled ? s.adjustFast : 0;
    if (keys != 0) v = std::clamp(std::round((v + static_cast<float>(keys) * step) / step) * step, min, max);

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, v);
    ImFont* font = Row();
    char widest[64];
    std::snprintf(widest, sizeof(widest), format, max);
    const float numberWidth = std::max(font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, widest).x, Px(46));
    const ImRect track(ImVec2(r.value.Min.x + Px(8), r.value.Min.y), ImVec2(r.value.Max.x - numberWidth - Px(18), r.value.Max.y));
    // The mouse drags it from a press on the bar (A has nothing to do on a slider).
    if (r.held && !r.disabled && ImGui::GetIO().MouseClickedPos[0].x >= track.Min.x - Px(10) && track.GetWidth() > 0.0f) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - track.Min.x) / track.GetWidth(), 0.0f, 1.0f);
        v = std::clamp(std::round((min + t * (max - min)) / step) * step, min, max);
    }
    const bool changed = v != before;
    if (changed) {
        *value = v;
        ImGui::MarkItemEdited(r.id);
        std::snprintf(buffer, sizeof(buffer), format, v);
    }
    if (!visible) return changed;

    ImDrawList* list = ImGui::GetCurrentWindow()->DrawList;
    const float t = max > min ? (v - min) / (max - min) : 0.0f;
    const float y = std::round((r.bb.Min.y + r.bb.Max.y) * 0.5f);
    const float thickness = Px(6);
    list->AddRectFilled(ImVec2(track.Min.x, y - thickness * 0.5f), ImVec2(track.Max.x, y + thickness * 0.5f),
                        UiCol(Colour::Field), thickness * 0.5f);
    const float x = track.Min.x + t * track.GetWidth();
    list->AddRectFilled(ImVec2(track.Min.x, y - thickness * 0.5f), ImVec2(x, y + thickness * 0.5f),
                        UiCol(r.selected ? Colour::Lime : Colour::LimeDeep), thickness * 0.5f);
    list->AddCircleFilled(ImVec2(x, y), r.selected ? Px(9) : Px(7), UiCol(Colour::Cream));
    if (r.selected) list->AddCircle(ImVec2(x, y), Px(9), UiCol(Colour::Lime), 0, Px(2));
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, buffer);
    list->AddText(font, font->FontSize, ImVec2(std::round(r.value.Max.x - size.x), std::round(y - size.y * 0.5f)),
                  UiCol(Colour::Cream), buffer);
    return changed;
}

bool SliderInt(const char* label, int* value, int min, int max, int step, const char* format) {
    float v = static_cast<float>(*value);
    // The format takes an int: show it through a float-formatting copy.
    std::string floatFormat = format;
    const size_t d = floatFormat.find("%d");
    if (d != std::string::npos) floatFormat.replace(d, 2, "%.0f");
    if (!Slider(label, &v, static_cast<float>(min), static_cast<float>(max), static_cast<float>(step), floatFormat.c_str())) {
        return false;
    }
    const int rounded = static_cast<int>(std::lround(v));
    if (rounded == *value) return false;
    *value = rounded;
    return true;
}

bool Toggle(const char* label, bool* value) {
    RowFrame r;
    const bool visible = BeginRow(label, Kind::Toggle, r);
    const bool before = *value;
    if (r.adjust < 0) *value = false;
    if (r.adjust > 0) *value = true;
    if (r.activate) *value = !*value;
    const bool changed = *value != before;
    if (changed) ImGui::MarkItemEdited(r.id);
    if (!visible) return changed;

    ImDrawList* list = ImGui::GetCurrentWindow()->DrawList;
    const float height = Px(26);
    const float width = Px(50);
    const float y = std::round((r.bb.Min.y + r.bb.Max.y) * 0.5f);
    const ImVec2 a(r.value.Max.x - width, y - height * 0.5f);
    const ImVec2 b(r.value.Max.x, y + height * 0.5f);
    list->AddRectFilled(a, b, *value ? UiCol(Colour::LimeDeep) : UiCol(Colour::Field), height * 0.5f);
    list->AddRect(a, b, *value ? UiCol(Colour::Lime) : UiCol(Colour::Purple, 0.9f), height * 0.5f, 0, Px(1.5f));
    const float knob = height * 0.5f - Px(4);
    list->AddCircleFilled(ImVec2(*value ? b.x - height * 0.5f : a.x + height * 0.5f, y), knob,
                          *value ? UiCol(Colour::Cream) : UiCol(Colour::CreamDim, 0.85f));
    ImFont* font = Row();
    const char* text = *value ? "On" : "Off";
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
    list->AddText(font, font->FontSize, ImVec2(std::round(a.x - Px(14) - size.x), std::round(y - size.y * 0.5f)),
                  *value ? UiCol(Colour::Cream) : UiCol(Colour::CreamDim), text);
    return changed;
}

bool Action(const char* label, const char* value) {
    RowFrame r;
    const bool visible = BeginRow(label, Kind::Action, r);
    if (!visible) return r.activate;
    ImDrawList* list = ImGui::GetCurrentWindow()->DrawList;
    const float y = std::round((r.bb.Min.y + r.bb.Max.y) * 0.5f);
    const float chevron = Px(12);
    Arrow(list, ImVec2(r.value.Max.x - chevron * 0.5f, y), chevron, true,
          UiCol(r.selected ? Colour::Lime : Colour::CreamDim, r.selected ? 1.0f : 0.5f));
    if (value != nullptr && value[0] != '\0') {
        ImFont* font = Body();
        const float right = r.value.Max.x - chevron - Px(14);
        const float textWidth = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, value).x;
        const float width = std::min(textWidth + Px(28), right - r.value.Min.x);
        const float h = std::round(font->FontSize + Px(12));
        Pill(list, ImVec2(right - width, y - h * 0.5f), ImVec2(right, y + h * 0.5f), value, r.selected);
    }
    return r.activate;
}

int Binding(const char* label, const char* primary, const char* secondary, bool allowSecond) {
    RowFrame r;
    const bool visible = BeginRow(label, Kind::Binding, r);
    const bool hasSecond = secondary != nullptr && secondary[0] != '\0';
    const float gap = Px(10);
    const float half = (r.value.GetWidth() - gap) * 0.5f;
    const ImRect first(r.value.Min, ImVec2(r.value.Min.x + half, r.value.Max.y));
    const ImRect second(ImVec2(r.value.Max.x - half, r.value.Min.y), r.value.Max);
    int result = 0;
    if (r.activate) {
        result = 1;
        if (r.clicked && allowSecond && ImGui::GetIO().MousePos.x >= second.Min.x) result = 2;
    }
    if (r.selected && !r.disabled && allowSecond && s.second) result = 2;
    if (!visible) return result;

    ImDrawList* list = ImGui::GetCurrentWindow()->DrawList;
    ImFont* font = Body();
    const float h = std::round(font->FontSize + Px(12));
    const float y = std::round((r.bb.Min.y + r.bb.Max.y) * 0.5f);
    Pill(list, ImVec2(first.Min.x, y - h * 0.5f), ImVec2(first.Max.x, y + h * 0.5f), primary, r.selected);
    if (hasSecond) {
        Pill(list, ImVec2(second.Min.x, y - h * 0.5f), ImVec2(second.Max.x, y + h * 0.5f), secondary, false);
    } else if (allowSecond && (r.selected || r.hovered)) {
        Pill(list, ImVec2(second.Min.x, y - h * 0.5f), ImVec2(second.Max.x, y + h * 0.5f), "+ Second", false, true);
    }
    return result;
}

namespace {
int ResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}
} // namespace

bool TextField(const char* label, std::string& text) {
    RowFrame r;
    const bool visible = BeginRow(label, Kind::Text, r, 0, true);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!visible || window->SkipItems) return false;
    const ImVec2 after = window->DC.CursorPos;
    const float h = ImGui::GetFrameHeight();
    window->DC.CursorPos = ImVec2(r.value.Min.x, std::round((r.bb.Min.y + r.bb.Max.y - h) * 0.5f));
    ImGui::PushID(label);
    if (r.selected && s.activate && !r.disabled) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(r.value.GetWidth());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Vec(Colour::Field, 0.9f));
    const bool committed = ImGui::InputText("##text", text.data(), text.capacity() + 1,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                                            ResizeCallback, &text);
    ImGui::PopStyleColor();
    ImGui::PopID();
    window->DC.CursorPos = after;
    return committed;
}

void Describe(const char* text) {
    if (!s.lastRowSelected || text == nullptr || text[0] == '\0') return;
    if (!s.text.empty()) s.text += "\n\n";
    s.text += text;
}

bool RowSelected() { return s.lastRowSelected; }

Kind SelectedKind() { return s.selectedKind; }
const std::string& DetailTitle() { return s.title; }
const std::string& DetailText() { return s.text; }

} // namespace MenuRows
