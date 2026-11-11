#pragma once

#include "texture.hpp"
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
// `tlut`: for a palette texture, the palette the draw uses (the replayed GX state's loadedTluts[obj.tlut]); it names the
// palette part of the key. Without one a palette texture is keyed with palette hash 0 (nullptr for non-palette textures).
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, const GXTlutObj_* tlut) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj, const GXTlutObj_* tlut) noexcept;
// Dev menu (F8): whether the replacement with this file name (no extension) has been drawn since start.
bool replacement_used(std::string_view name) noexcept;
// Dev menu (F8): forget them (new level); the next take_highlight_change returns true.
void reset_used() noexcept;
// Dev menu (F8): draw the texture with this replacement file name (no extension) solid magenta, whether or not it has a
// replacement ("" = off). take_highlight_change returns true once after
// it changed: the caller must drop its cached textures so find_replacement runs again.
void set_highlight(std::string_view name) noexcept;
bool take_highlight_change() noexcept;
// Dev menu (F8): whether this handle is the magenta highlight texture (never creates it).
bool is_highlight_handle(const TextureHandle& handle) noexcept;
// Dev menu (F8): texture log (gx.cpp, devmenu_texlog.csv): the scene its rows are tagged with, "" = off.
// texture_log_generation changes on every set, so the draw path re-reads the scene only then.
void set_texture_log_scene(std::string_view scene) noexcept;
std::string texture_log_scene() noexcept;
uint32_t texture_log_generation() noexcept;
// Dev tools texture pack switcher: <UserData>/texture_packs/<name>/ replaces the usual chain (the user's
// texture_replacements, mods, the built-in HD pack) while selected, and a texture it lacks keeps the game's original;
// "" = usual (no pack). set_texture_pack (any thread) only leaves a
// request, and asking for the pack in use again rescans it; false = not a plain folder name. take_texture_pack_change
// (renderer thread, before textures are resolved) applies it and returns true once after a change: the caller must drop
// its cached textures so find_replacement runs again.
bool set_texture_pack(std::string_view name) noexcept;
std::string texture_pack() noexcept;         // the pack asked for ("" = usual)
size_t texture_pack_entries() noexcept;      // replacements found in the applied pack
bool take_texture_pack_change() noexcept;
// Dev menu texture list: every texture resolved since the last reset_used (per texture key). Off until the dev menu
// turns it on (dev tools only); a copy is returned, and the generation changes whenever the list does.
struct TextureListEntry {
  std::string name;  // file name without extension (what a pack or a dump needs)
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;    // GXTexFmt
  bool inPack = false;    // the selected pack has a file for it
  bool inUsual = false;   // the usual chain (texture_replacements, mods, HD pack) has one
  bool packSelected = false;
};
void set_texture_list_enabled(bool on) noexcept;
uint32_t texture_list_generation() noexcept;
std::vector<TextureListEntry> texture_list() noexcept;
// Button prompts for the controller in use: `family` 0 xbox, 1 playstation, 2 nintendo, -1 the game's own GameCube
// icons; `buttons` = the pad button (controller_button_names.h config name) drawn for the GameCube A, B, X and Y
// buttons, "" = that button's usual one. take_button_icon_style_change returns true once after a change, so cached
// textures can be re-resolved (renderer thread).
struct ButtonIcons {
  int family = -1;
  std::array<std::string, 4> buttons;
  bool operator==(const ButtonIcons&) const = default;
};
void set_button_icons(const ButtonIcons& icons) noexcept;
// <UserData>/button_icons/<family> of the icons in use (UTF-8), "" when no controller is connected.
std::string button_icon_folder() noexcept;
bool take_button_icon_style_change() noexcept;
} // namespace aurora::gfx::texture_replacement
