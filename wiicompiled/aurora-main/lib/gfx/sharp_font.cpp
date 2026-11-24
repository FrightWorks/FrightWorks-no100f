#include "sharp_font.hpp"

#include "texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace aurora::gfx::sharp_font {
namespace {
// Palette range hashes (XXH64 of the palette entries the sheet's indices use, as in texture replacement names) of the
// sheets the game builds from font.HIP. Seen in game: the lettering sheet (menus, PRESS START, captions) and the plain
// one (sans-serif and accented letters).
constexpr uint64_t kFontPalettes[] = {
    0x0ee17e70f46e5e50ull,
    0x838f511007e15637ull,
};

constexpr double kPi = 3.14159265358979323846;

double lanczos3(double x) noexcept {
  x = std::abs(x);
  if (x < 1e-8) return 1.0;
  if (x >= 3.0) return 0.0;
  const double px = kPi * x;
  return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

struct Taps {
  std::array<uint32_t, 6> index;
  std::array<float, 6> weight;
};

// Lanczos taps for each sample of an n-sample line enlarged `scale` times (sample i sits at source position
// (i + 0.5) / scale - 0.5); the edges repeat the outermost source sample.
std::vector<Taps> make_taps(uint32_t n, uint32_t scale) {
  std::vector<Taps> taps(static_cast<size_t>(n) * scale);
  for (size_t i = 0; i < taps.size(); ++i) {
    const double u = (static_cast<double>(i) + 0.5) / scale - 0.5;
    const int first = static_cast<int>(std::floor(u)) - 2;
    double sum = 0.0;
    std::array<double, 6> w{};
    for (int k = 0; k < 6; ++k) {
      w[k] = lanczos3(u - (first + k));
      sum += w[k];
      taps[i].index[k] = static_cast<uint32_t>(std::clamp(first + k, 0, static_cast<int>(n) - 1));
    }
    for (int k = 0; k < 6; ++k) taps[i].weight[k] = static_cast<float>(w[k] / sum);
  }
  return taps;
}
} // namespace

bool is_font_sheet(uint32_t width, uint32_t height, uint32_t format, uint64_t tlutHash) noexcept {
  return width == 256 && height == 256 && format == GX_TF_C4 &&
         std::find(std::begin(kFontPalettes), std::end(kFontPalettes), tlutHash) != std::end(kFontPalettes);
}

std::optional<ConvertedTexture> sharpen(const ConvertedTexture& sheet) noexcept {
  const uint32_t w = sheet.width;
  const uint32_t h = sheet.height;
  if (sheet.format != wgpu::TextureFormat::RGBA8Unorm || w == 0 || h == 0 || w > 1024 || h > 1024 ||
      sheet.data.size() < static_cast<size_t>(w) * h * 4) {
    return std::nullopt;
  }
  const uint32_t W = w * kScale;
  const uint32_t H = h * kScale;
  const uint8_t* src = sheet.data.data();

  // The letters' colour (white on the game's sheets) for the parts that were clear in the original.
  std::vector<float> alpha(static_cast<size_t>(w) * h);
  std::array<double, 3> colourSum{};
  double colourCount = 0.0;
  for (size_t i = 0; i < alpha.size(); ++i) {
    alpha[i] = src[i * 4 + 3] / 255.0f;
    if (src[i * 4 + 3] >= 128) {
      for (int c = 0; c < 3; ++c) colourSum[c] += src[i * 4 + c];
      colourCount += 1.0;
    }
  }
  std::array<uint8_t, 3> fill{255, 255, 255};
  if (colourCount > 0.0) {
    for (int c = 0; c < 3; ++c) fill[c] = static_cast<uint8_t>(std::lround(colourSum[c] / colourCount));
  }

  // Alpha enlarged with a separable Lanczos-3 filter: rows first, then columns.
  const auto tx = make_taps(w, kScale);
  const auto ty = make_taps(h, kScale);
  std::vector<float> rows(static_cast<size_t>(h) * W);
  for (uint32_t y = 0; y < h; ++y) {
    const float* line = alpha.data() + static_cast<size_t>(y) * w;
    float* out = rows.data() + static_cast<size_t>(y) * W;
    for (uint32_t x = 0; x < W; ++x) {
      const auto& t = tx[x];
      float v = 0.0f;
      for (int k = 0; k < 6; ++k) v += line[t.index[k]] * t.weight[k];
      out[x] = v;
    }
  }
  std::vector<float> field(static_cast<size_t>(W) * H);
  for (uint32_t y = 0; y < H; ++y) {
    const auto& t = ty[y];
    float* out = field.data() + static_cast<size_t>(y) * W;
    for (int k = 0; k < 6; ++k) {
      const float* in = rows.data() + static_cast<size_t>(t.index[k]) * W;
      const float wk = t.weight[k];
      for (uint32_t x = 0; x < W; ++x) out[x] += in[x] * wk;
    }
  }

  uint32_t mips = 1;
  while ((std::max(W, H) >> mips) > 0) ++mips;
  ByteBuffer data{static_cast<size_t>(calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, W, H, mips))};
  uint8_t* level = data.data();

  // Level 0: the outline is where the enlarged alpha crosses one half; the edge is softened over one output pixel
  // (distance to the outline = (v - 0.5) / gradient). Colour: the source's own, premultiplied bilinear, else `fill`.
  const auto at = [&](int x, int y) {
    x = std::clamp(x, 0, static_cast<int>(W) - 1);
    y = std::clamp(y, 0, static_cast<int>(H) - 1);
    return field[static_cast<size_t>(y) * W + x];
  };
  for (uint32_t y = 0; y < H; ++y) {
    const float sy = std::clamp((y + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(h - 1));
    const uint32_t y0 = static_cast<uint32_t>(sy);
    const uint32_t y1 = std::min(y0 + 1, h - 1);
    const float fy = sy - y0;
    for (uint32_t x = 0; x < W; ++x) {
      const float v = at(x, y);
      const float gx = 0.5f * (at(x + 1, y) - at(x - 1, y));
      const float gy = 0.5f * (at(x, y + 1) - at(x, y - 1));
      const float grad = std::max(std::sqrt(gx * gx + gy * gy), 1e-4f);
      const float a = std::clamp(0.5f + (v - 0.5f) / grad, 0.0f, 1.0f);

      const float sx = std::clamp((x + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(w - 1));
      const uint32_t x0 = static_cast<uint32_t>(sx);
      const uint32_t x1 = std::min(x0 + 1, w - 1);
      const float fx = sx - x0;
      const std::array<std::pair<size_t, float>, 4> corners{{
          {(static_cast<size_t>(y0) * w + x0) * 4, (1 - fx) * (1 - fy)},
          {(static_cast<size_t>(y0) * w + x1) * 4, fx * (1 - fy)},
          {(static_cast<size_t>(y1) * w + x0) * 4, (1 - fx) * fy},
          {(static_cast<size_t>(y1) * w + x1) * 4, fx * fy},
      }};
      std::array<float, 3> premul{};
      float weight = 0.0f;
      for (const auto& [o, cw] : corners) {
        const float ca = src[o + 3] * cw;
        for (int c = 0; c < 3; ++c) premul[c] += src[o + c] * ca;
        weight += ca;
      }
      uint8_t* px = level + (static_cast<size_t>(y) * W + x) * 4;
      for (int c = 0; c < 3; ++c) {
        px[c] = weight > 1.0f ? static_cast<uint8_t>(std::clamp(std::lround(premul[c] / weight), 0l, 255l)) : fill[c];
      }
      px[3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  }

  // Mips: 2x2 boxes, averaged with alpha weighting so clear texels don't darken the edges.
  uint32_t pw = W, ph = H;
  for (uint32_t mip = 1; mip < mips; ++mip) {
    const uint32_t mw = std::max(pw >> 1, 1u);
    const uint32_t mh = std::max(ph >> 1, 1u);
    const uint8_t* prev = level;
    uint8_t* next = level + static_cast<size_t>(pw) * ph * 4;
    for (uint32_t y = 0; y < mh; ++y) {
      for (uint32_t x = 0; x < mw; ++x) {
        std::array<uint32_t, 3> premul{};
        uint32_t alphaSum = 0;
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(x * 2 + dx, pw - 1);
            const uint32_t sy = std::min(y * 2 + dy, ph - 1);
            const uint8_t* p = prev + (static_cast<size_t>(sy) * pw + sx) * 4;
            for (int c = 0; c < 3; ++c) premul[c] += p[c] * p[3];
            alphaSum += p[3];
          }
        }
        uint8_t* px = next + (static_cast<size_t>(y) * mw + x) * 4;
        for (int c = 0; c < 3; ++c) px[c] = alphaSum > 0 ? static_cast<uint8_t>(premul[c] / alphaSum) : fill[c];
        px[3] = static_cast<uint8_t>((alphaSum + 2) / 4);
      }
    }
    level = next;
    pw = mw;
    ph = mh;
  }

  return ConvertedTexture{
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = W,
      .height = H,
      .mips = mips,
      .data = std::move(data),
  };
}
} // namespace aurora::gfx::sharp_font
