#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace aurora::gfx {

// The part of a palette (TLUT) a texture actually uses: texture replacement names hash only these bytes, so two
// textures sharing one big palette get distinct names. Pure (no GX types) so a unit test can run it.
struct PaletteSlice {
  size_t offset = 0;  // bytes into the palette
  size_t size = 0;
};

// GXTexFmt values of the palette formats.
constexpr uint32_t kPaletteFormatC4 = 8;
constexpr uint32_t kPaletteFormatC8 = 9;
constexpr uint32_t kPaletteFormatC14X2 = 10;

// textureData/textureSize: the texture's base level, as the game has it (big-endian C14X2 indices); paletteBytes: the
// palette's size in bytes (entries * 2). Nothing when the format isn't a palette format, there is no data, or the
// indices run past the palette.
inline std::optional<PaletteSlice> referenced_palette_slice(uint32_t format, const uint8_t* textureData,
                                                            uint32_t textureSize, size_t paletteBytes) noexcept {
  if (textureData == nullptr || textureSize == 0) {
    return std::nullopt;
  }
  uint32_t minIndex = 0xffff;
  uint32_t maxIndex = 0;
  switch (format) {
  case kPaletteFormatC4:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t lowNibble = textureData[i] & 0xf;
      const uint32_t highNibble = textureData[i] >> 4;
      minIndex = std::min({minIndex, lowNibble, highNibble});
      maxIndex = std::max({maxIndex, lowNibble, highNibble});
    }
    break;
  case kPaletteFormatC8:
    for (uint32_t i = 0; i < textureSize; ++i) {
      minIndex = std::min<uint32_t>(minIndex, textureData[i]);
      maxIndex = std::max<uint32_t>(maxIndex, textureData[i]);
    }
    break;
  case kPaletteFormatC14X2:
    for (uint32_t i = 0; i + sizeof(uint16_t) <= textureSize; i += sizeof(uint16_t)) {
      const uint32_t index = ((uint32_t{textureData[i]} << 8) | textureData[i + 1]) & 0x3fff;
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  default:
    return std::nullopt;
  }
  if (minIndex > maxIndex) {
    return std::nullopt;
  }
  const PaletteSlice slice{2 * static_cast<size_t>(minIndex), 2 * (static_cast<size_t>(maxIndex) + 1 - minIndex)};
  if (slice.offset + slice.size > paletteBytes) {
    return std::nullopt;
  }
  return slice;
}

}  // namespace aurora::gfx
