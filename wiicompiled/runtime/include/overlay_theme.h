#pragma once

#include <imgui.h>

#include <aurora/aurora.h>

// The look of every in-game ImGui menu (F10 settings, F8 dev menu, prompts): the launcher's haunted-mansion colours,
// Roboto for text, Luckiest Guy for titles, rounded "board" panels and wooden "plank" buttons.
namespace OverlayTheme {

// AuroraConfig::imGuiInitCallback: loads the fonts (scaled for the display) and sets the style.
void Initialize(const AuroraWindowSize* size);

// The display scale the fonts and the style were built for (1 = 100 %).
float Scale();
// v pixels at that scale.
inline float Px(float v) { return v * Scale(); }

ImFont* Body();     // Roboto 17 px: the default font
ImFont* Small();    // Roboto 14 px: hints, footers
ImFont* Heading();  // Luckiest Guy 22 px: section titles, planks
ImFont* Title();    // Luckiest Guy 40 px: panel titles

enum class Colour { Ink, Cream, CreamDim, Lime, LimeDeep, Gold, Purple, PurpleDeep, BoardTop, BoardBottom, Field, Good, Warn };
ImU32 Col(Colour colour, float alpha = 1.0f);
ImVec4 Vec(Colour colour, float alpha = 1.0f);

// Text with an outline (the launcher's sign lettering).
void OutlinedText(ImDrawList* list, ImFont* font, ImVec2 pos, ImU32 fill, ImU32 outline, float thickness,
                  const char* text);
// The board behind a panel: rounded, purple gradient, ink edge, a thin lime rim. `alpha` below 1 lets what is behind
// show through the body (the F10 board fills the window, over the dimmed game).
void Board(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float alpha = 1.0f);

// A wooden plank button (tabs and big actions): lime when selected/primary, purple otherwise. Gamepad-navigable.
bool Plank(const char* label, bool selected, ImVec2 size);
// A plank that does something rather than choosing a page: dark, with its edge and lettering in `colour`.
bool ActionPlank(const char* label, Colour colour, ImVec2 size);
// A switch for a setting: the knob on the right = on. Returns true when clicked (and flips *value).
bool Toggle(const char* label, bool* value);
// A section title in Luckiest Guy with a lime rule under it.
void SectionTitle(const char* text);
// Dim, wrapped explanation text.
void Hint(const char* text);
// A settings row: the label in the left column, then the cursor at the control column with the next item's width
// set. Use "##id" labels on the control.
void RowLabel(const char* label, float labelWidth, float controlWidth);

} // namespace OverlayTheme
