#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace aurora::webgpu {

struct alignas(16) HdrPresentationUniform {
  std::array<float, 4> paperRect{};
  std::array<float, 4> peakRect{};
  std::array<float, 4> parameters{};
  // Game-picture scRGB scale and highlight boost, independent of calibration patches.
  std::array<float, 4> picture{};
};
static_assert(sizeof(HdrPresentationUniform) == 64);
static_assert(offsetof(HdrPresentationUniform, picture) == 48);

// window::hdr_display_state(): -1 unknown, 0 HDR off, 1 HDR on. Only a display that reports HDR off keeps the SDR
// picture at startup; unknown (not Windows, mirrored outputs, a failed query) proceeds as it always has.
inline bool hdr_output_allowed(int displayState) noexcept { return displayState != 0; }

inline bool valid_hdr_brightness(float paper, float peak) noexcept {
  return std::isfinite(paper) && std::isfinite(peak) &&
         paper >= 80.f && paper <= 1000.f && peak >= 80.f && peak <= 10000.f;
}

inline HdrPresentationUniform hdr_presentation_uniform(float paper, float peak,
                                                      uint32_t width, uint32_t height) noexcept {
  HdrPresentationUniform uniform;
  uniform.parameters[2] = 1.f / std::max(width, 1u);
  uniform.parameters[3] = 1.f / std::max(height, 1u);
  uniform.picture[0] = paper / 80.f;
  uniform.picture[1] = std::max(peak, paper) / paper - 1.f;
  return uniform;
}

} // namespace aurora::webgpu
