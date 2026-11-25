#pragma once

#include "texture.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
// `tlut`: the palette a palette texture is drawn with (resolve_static_palette_texture); with it, the game's font
// sheets are redrawn sharp ([video] sharp_font, sharp_font.hpp) even when texture replacement is off.
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, const GXTlutObj_* tlut = nullptr) noexcept;
// `tlut`: as for find_replacement; a palette texture's palette hash comes from it when the TLUT mirror has none.
std::string build_texture_replacement_name(const GXTexObj_& obj, const GXTlutObj_* tlut = nullptr) noexcept;
// Dev menu (F8): a static texture was uploaded (gx.cpp, every texture resolve): returns its name (pack file name without
// the extension, as build_texture_replacement_name) and, while track_drawn is on, adds it to the drawn list. `handle`:
// what was uploaded for it.
std::string note_drawn_texture(const GXTexObj_& obj, const GXTlutObj_* tlut, const TextureHandle& handle) noexcept;
bool tracking_drawn() noexcept;
// Dev menu (F8): keep the list of textures drawn since the last reset_used (off by default: the dev menu turns it on).
void track_drawn(bool on) noexcept;
// Dev menu (F8): one texture drawn since the last reset_used. `pack`: drawn from an HD file (a texture pack, mod or the
// player's texture_replacements); `xbox`: from the player's Xbox textures; fileWidth/fileHeight: the size uploaded in
// its place (0 = the game's own); versions: how often the game rewrote it in place (the name is the newest).
struct DrawnTexture {
  std::string name;
  uint32_t width = 0, height = 0, format = 0, versions = 1, fileWidth = 0, fileHeight = 0;
  bool pack = false;
  bool xbox = false;
};
std::vector<DrawnTexture> drawn_textures() noexcept;
// Changes whenever the drawn list or its pack markers change.
uint32_t drawn_generation() noexcept;
// Dev menu (F8): forget what was drawn (new level); the next take_highlight_change returns true.
void reset_used() noexcept;
// Dev menu (F8): draw that texture solid magenta ("" = off), by its pack file name without the extension, with or
// without a replacement file. take_highlight_change returns true once after it changed: the caller must drop its
// cached textures so find_replacement runs again.
void set_highlight(std::string_view name) noexcept;
bool take_highlight_change() noexcept;
// Dev menu (F8): read the texture folders and packs again (any thread). take_reload, on the renderer's thread before
// textures are resolved, does it and returns true once: the caller must then drop its cached textures.
void request_reload() noexcept;
bool take_reload() noexcept;
// Dev menu (F8): whether this handle is the magenta highlight texture (never creates it).
bool is_highlight_handle(const TextureHandle& handle) noexcept;
// Dev menu (F8): texture log (gx.cpp, devmenu_texlog.csv): the scene its rows are tagged with, "" = off.
// texture_log_generation changes on every set, so the draw path re-reads the scene only then.
void set_texture_log_scene(std::string_view scene) noexcept;
std::string texture_log_scene() noexcept;
uint32_t texture_log_generation() noexcept;
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
