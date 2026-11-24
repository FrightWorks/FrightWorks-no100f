#pragma once

#include "texture_convert.hpp"

#include <cstdint>
#include <optional>

// Sharp font ([video] sharp_font): the game builds its font sheets in memory from font.HIP (white letters on clear,
// 4-bit palette, 256x256) and draws its menu and caption lettering from them, so it goes soft at today's resolutions.
// Instead of a replacement file (that would be game art in the repo), each sheet is redrawn as it loads: its alpha is
// enlarged with a Lanczos filter and cut at the halfway line (the letter outline the original anti-aliasing implies),
// with a one-pixel soft edge. Ahead of the built-in HD pack, whose AI-upscaled font has lumpy edges.
namespace aurora::gfx::sharp_font {
constexpr uint32_t kScale = 6;

// Whether a palette texture is one of the font sheets: size, format and the hash of the palette range it uses
// (the same hash texture replacement names carry; the sheets' contents vary with the glyphs placed on them).
bool is_font_sheet(uint32_t width, uint32_t height, uint32_t format, uint64_t tlutHash) noexcept;

// An RGBA8 sheet (one level) redrawn kScale times larger, with a full mip chain.
std::optional<ConvertedTexture> sharpen(const ConvertedTexture& sheet) noexcept;
} // namespace aurora::gfx::sharp_font
