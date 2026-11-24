#pragma once

#include "texture_convert.hpp"

#include <cstdint>
#include <optional>

// Sharp font and icons: textures redrawn at a high resolution as they load, from the game's own data (nothing is
// shipped; a replacement file would be game art in the repo).
//
// Font ([video] sharp_font): the game builds its font sheets in memory from font.HIP (white letters on clear, 4-bit
// palette, 256x256) and draws its menu and caption lettering from them, so it goes soft at today's resolutions. Each
// sheet's alpha is enlarged with a Lanczos filter and cut at the halfway line (the letter outline the original
// anti-aliasing implies), with a one-pixel soft edge. Ahead of the downloaded HD packs, whose AI-upscaled font has lumpy
// edges.
//
// Icons ([video] sharp_icons): flat-colour pause menu and map icons (room markers, X markers, tags, key, warp swirl).
// The icon's colours are grouped into a few flat regions (k-means); each texel is the best mix of two neighbouring
// region colours, so every region gets a sub-pixel outline that is enlarged and cut like the font; shading inside a
// region comes from that region's own texels. Only where no replacement file exists: the HD pack keeps its icons.
namespace aurora::gfx::sharp_font {
constexpr uint32_t kScale = 6;

// Whether a palette texture is one of the font sheets: size, format and the hash of the palette range it uses
// (the same hash texture replacement names carry; the sheets' contents vary with the glyphs placed on them).
bool is_font_sheet(uint32_t width, uint32_t height, uint32_t format, uint64_t tlutHash) noexcept;

// An RGBA8 sheet (one level) redrawn kScale times larger, with a full mip chain.
std::optional<ConvertedTexture> sharpen(const ConvertedTexture& sheet) noexcept;

// Cheap first test (size and format only), before the texture is hashed.
bool may_be_flat_icon(uint32_t width, uint32_t height, uint32_t format) noexcept;
// Whether a texture is one of the flat icons, by its texture hash (texture replacement names).
bool is_flat_icon(uint32_t width, uint32_t height, uint32_t format, uint64_t textureHash) noexcept;

// An RGBA8 icon (one level) redrawn kScale times larger with clean region outlines, with a full mip chain.
std::optional<ConvertedTexture> sharpen_flat(const ConvertedTexture& icon) noexcept;
} // namespace aurora::gfx::sharp_font
