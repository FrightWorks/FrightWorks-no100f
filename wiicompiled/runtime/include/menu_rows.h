#pragma once

#include <imgui.h>

#include <functional>
#include <string>

// The F10 menu's settings list, made for a controller: one full-width row per setting, its name on the left and its
// value on the right. Up / down (D-pad, left stick, arrow keys, or pointing with the mouse) pick a row, left / right
// change its value, and A (Enter, Space or a click) toggles or opens it. The list keeps its own selection instead of
// ImGui's navigation, whose left stick only scrolls. The details panel shows the selected row's name and its
// Describe text.
namespace MenuRows {

enum class Kind { None, Choice, Toggle, Slider, Action, Binding, Text };

// Each frame, inside the list's child window, before the first row. `input`: the list takes the controller and the
// keys this frame (false while a prompt, a popup or a text field has them).
void Begin(bool input);
// After the last row: keeps the selection on a row and scrolls it into view.
void End();
// Selects row `row` (a page change: 0) and scrolls it into view.
void Select(int row);
int Selected();

// A press of a key the menu owns while the list has input (the pad's buttons and D-pad, the left stick, the arrows,
// Enter, Space, Q / E, Page Up / Down): ImGui's navigation doesn't see them then.
bool Pressed(ImGuiKey key, bool repeat = false);
// Gives the pad and keys back (the menu closed): ImGui navigation sees them again.
void Release();
// The last input came from a controller, so the prompts show its buttons (else the keys).
bool PadInput();
void SetPadInput(bool pad);

// A heading above a group of rows.
void Section(const char* title);
// Dim explanation text in the list (not a row).
void Note(const char* text);
// A name and a value that can't be changed (not a row: the selection skips it).
void Info(const char* label, const char* value);

// One of `count` values, left / right (or A) to step through them; `name(-1)` is shown when *index is -1 (a value
// that is none of them). Items `enabled` turns down are skipped.
bool Choice(const char* label, int* index, int count, const std::function<const char*(int)>& name,
            const std::function<bool(int)>& enabled = {});
bool Choice(const char* label, int* index, const char* const* items, int count);
// A bar from min to max, `step` per press of left / right; the mouse drags it.
bool Slider(const char* label, float* value, float min, float max, float step, const char* format);
bool SliderInt(const char* label, int* value, int min, int max, int step, const char* format);
// On / off: A flips it, left turns it off, right on.
bool Toggle(const char* label, bool* value);
// Opens or does something (A or a click). `value` is shown on the right (a key's name, a status).
bool Action(const char* label, const char* value = nullptr);
// A binding: its input and an optional second one. Returns 1 to change the first (A, or a click on it), 2 for the
// second (X, or a click on it or on "+ Second"), else 0.
int Binding(const char* label, const char* primary, const char* secondary, bool allowSecond);
// A line of text typed with the keyboard: A or a click starts typing. True when Enter commits it.
bool TextField(const char* label, std::string& text);

// The details panel's text for the row just drawn, shown while it is selected.
void Describe(const char* text);
// The row just drawn is the selected one.
bool RowSelected();

// The selected row this frame (after the list is drawn): its kind, name and description.
Kind SelectedKind();
const std::string& DetailTitle();
const std::string& DetailText();

} // namespace MenuRows
