#include "sharp_font.hpp"

#include "texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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

struct FlatIcon {
  uint64_t textureHash;
  uint32_t width, height, format;
};
// mnu4.HIP (pause menu and map), as texture replacement names hash them. Left out on purpose: SD_tag_3 (CMPR, its
// block noise makes notched outlines) and the lit skull key (shaded art: the regions make its shading blotchy).
constexpr FlatIcon kFlatIcons[] = {
    {0xa104a782f714805cull, 64, 64, GX_TF_RGBA8},     // arrow_6
    {0xde7d27125da89477ull, 32, 32, GX_TF_RGB5A3},    // token_icon
    {0xc7b2874323776f99ull, 64, 64, GX_TF_RGBA8},     // paws_map, paws_corner, SD_tag_map
    {0x6c7b9aa8fc556590ull, 64, 64, GX_TF_RGBA8},     // mnu4_skull_key_off
    {0x39cd51fd1e019d38ull, 128, 128, GX_TF_RGB5A3},  // SD_tag_2
    {0x9fbf142021336544ull, 64, 64, GX_TF_RGB5A3},    // warp_icon
    {0x1242d7d3a67c53f4ull, 128, 128, GX_TF_RGBA8},   // scooby_head_BG
    {0x726acd31ba66c040ull, 64, 64, GX_TF_RGBA8},     // roomD_* (map rooms not visited yet)
    {0x06f6da7456368aeaull, 64, 64, GX_TF_RGBA8},     // roomL_br
    {0x6e6515f85ec9f05bull, 64, 64, GX_TF_RGBA8},     // roomL_lv
    {0x89e2687e192701c0ull, 64, 64, GX_TF_RGBA8},     // roomL_a
    {0x4be9a8f9a5caf452ull, 64, 64, GX_TF_RGBA8},     // roomL_b
    {0x7a99421e88811f44ull, 64, 64, GX_TF_RGBA8},     // roomL_g
    {0x9acab582adfcf7c0ull, 64, 64, GX_TF_RGBA8},     // roomL_k
    {0x4a4d81219cae706cull, 64, 64, GX_TF_RGBA8},     // roomL_l
    {0x0a7152c2bfd6ec74ull, 64, 64, GX_TF_RGBA8},     // roomL_o
    {0x73a7e2c61ca27591ull, 64, 64, GX_TF_RGBA8},     // roomL_p
    {0x411afc7ca12a99beull, 64, 64, GX_TF_RGBA8},     // roomL_q
    {0x769d6830a9ed19c8ull, 64, 64, GX_TF_RGBA8},     // roomL_r
    {0x594de5766382e35eull, 64, 64, GX_TF_RGBA8},     // roomL_y
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

// A w x h float field enlarged kScale times with a separable Lanczos-3 filter: rows first, then columns.
std::vector<float> enlarge(const float* src, uint32_t w, uint32_t h, const std::vector<Taps>& tx,
                           const std::vector<Taps>& ty) {
  const uint32_t W = w * kScale;
  const uint32_t H = h * kScale;
  std::vector<float> rows(static_cast<size_t>(h) * W);
  for (uint32_t y = 0; y < h; ++y) {
    const float* line = src + static_cast<size_t>(y) * w;
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
  return field;
}

// Central-difference gradient length of a W x H field (edges clamped), at least 1e-4.
float gradient_at(const std::vector<float>& f, uint32_t W, uint32_t H, uint32_t x, uint32_t y) noexcept {
  const auto at = [&](int xx, int yy) {
    xx = std::clamp(xx, 0, static_cast<int>(W) - 1);
    yy = std::clamp(yy, 0, static_cast<int>(H) - 1);
    return f[static_cast<size_t>(yy) * W + xx];
  };
  const int ix = static_cast<int>(x);
  const int iy = static_cast<int>(y);
  const float gx = 0.5f * (at(ix + 1, iy) - at(ix - 1, iy));
  const float gy = 0.5f * (at(ix, iy + 1) - at(ix, iy - 1));
  return std::max(std::sqrt(gx * gx + gy * gy), 1e-4f);
}

uint32_t full_mip_count(uint32_t W, uint32_t H) noexcept {
  uint32_t mips = 1;
  while ((std::max(W, H) >> mips) > 0) ++mips;
  return mips;
}

// Fills levels 1.. of an RGBA8 mip chain from level 0: 2x2 boxes, averaged with alpha weighting so clear texels
// don't darken the edges.
void build_mips(uint8_t* level, uint32_t W, uint32_t H, uint32_t mips) noexcept {
  uint32_t pw = W, ph = H;
  for (uint32_t mip = 1; mip < mips; ++mip) {
    const uint32_t mw = std::max(pw >> 1, 1u);
    const uint32_t mh = std::max(ph >> 1, 1u);
    const uint8_t* prev = level;
    uint8_t* next = level + static_cast<size_t>(pw) * ph * 4;
    for (uint32_t y = 0; y < mh; ++y) {
      for (uint32_t x = 0; x < mw; ++x) {
        std::array<uint32_t, 3> premul{};
        std::array<uint32_t, 3> plain{};
        uint32_t alphaSum = 0;
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(x * 2 + dx, pw - 1);
            const uint32_t sy = std::min(y * 2 + dy, ph - 1);
            const uint8_t* p = prev + (static_cast<size_t>(sy) * pw + sx) * 4;
            for (int c = 0; c < 3; ++c) {
              premul[c] += p[c] * p[3];
              plain[c] += p[c];
            }
            alphaSum += p[3];
          }
        }
        uint8_t* px = next + (static_cast<size_t>(y) * mw + x) * 4;
        for (int c = 0; c < 3; ++c) {
          px[c] = static_cast<uint8_t>(alphaSum > 0 ? premul[c] / alphaSum : plain[c] / 4);
        }
        px[3] = static_cast<uint8_t>((alphaSum + 2) / 4);
      }
    }
    level = next;
    pw = mw;
    ph = mh;
  }
}

using Texel = std::array<float, 4>;  // premultiplied r, g, b (0-255) and alpha (0-255)

float distance2(const Texel& a, const Texel& b) noexcept {
  float d = 0.0f;
  for (int c = 0; c < 4; ++c) d += (a[c] - b[c]) * (a[c] - b[c]);
  return d;
}

// k-means with farthest-point seeding (deterministic): the first centre is the texel nearest the mean.
std::vector<Texel> kmeans(const std::vector<Texel>& X, size_t K, float& rms) {
  Texel mean{};
  for (const auto& x : X)
    for (int c = 0; c < 4; ++c) mean[c] += x[c] / static_cast<float>(X.size());
  std::vector<Texel> C;
  C.push_back(*std::min_element(X.begin(), X.end(), [&](const Texel& a, const Texel& b) {
    return distance2(a, mean) < distance2(b, mean);
  }));
  std::vector<float> nearest(X.size(), std::numeric_limits<float>::max());
  while (C.size() < K) {
    size_t far = 0;
    for (size_t i = 0; i < X.size(); ++i) {
      nearest[i] = std::min(nearest[i], distance2(X[i], C.back()));
      if (nearest[i] > nearest[far]) far = i;
    }
    C.push_back(X[far]);
  }
  std::vector<uint32_t> label(X.size());
  for (int iter = 0; iter < 25; ++iter) {
    for (size_t i = 0; i < X.size(); ++i) {
      uint32_t best = 0;
      for (uint32_t k = 1; k < C.size(); ++k)
        if (distance2(X[i], C[k]) < distance2(X[i], C[best])) best = k;
      label[i] = best;
    }
    std::vector<Texel> sum(K, Texel{});
    std::vector<uint32_t> count(K, 0);
    for (size_t i = 0; i < X.size(); ++i) {
      for (int c = 0; c < 4; ++c) sum[label[i]][c] += X[i][c];
      ++count[label[i]];
    }
    for (size_t k = 0; k < K; ++k)
      if (count[k] > 0)
        for (int c = 0; c < 4; ++c) C[k][c] = sum[k][c] / static_cast<float>(count[k]);
  }
  double err = 0.0;
  for (size_t i = 0; i < X.size(); ++i) err += distance2(X[i], C[label[i]]);
  rms = static_cast<float>(std::sqrt(err / static_cast<double>(X.size())));
  return C;
}
} // namespace

bool is_font_sheet(uint32_t width, uint32_t height, uint32_t format, uint64_t tlutHash) noexcept {
  return width == 256 && height == 256 && format == GX_TF_C4 &&
         std::find(std::begin(kFontPalettes), std::end(kFontPalettes), tlutHash) != std::end(kFontPalettes);
}

bool may_be_flat_icon(uint32_t width, uint32_t height, uint32_t format) noexcept {
  return std::any_of(std::begin(kFlatIcons), std::end(kFlatIcons), [&](const FlatIcon& icon) {
    return icon.width == width && icon.height == height && icon.format == format;
  });
}

bool is_flat_icon(uint32_t width, uint32_t height, uint32_t format, uint64_t textureHash) noexcept {
  return std::any_of(std::begin(kFlatIcons), std::end(kFlatIcons), [&](const FlatIcon& icon) {
    return icon.textureHash == textureHash && icon.width == width && icon.height == height && icon.format == format;
  });
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

  const auto field = enlarge(alpha.data(), w, h, make_taps(w, kScale), make_taps(h, kScale));

  const uint32_t mips = full_mip_count(W, H);
  ByteBuffer data{static_cast<size_t>(calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, W, H, mips))};
  uint8_t* level = data.data();

  // Level 0: the outline is where the enlarged alpha crosses one half; the edge is softened over one output pixel
  // (distance to the outline = (v - 0.5) / gradient). Colour: the source's own, premultiplied bilinear, else `fill`.
  for (uint32_t y = 0; y < H; ++y) {
    const float sy = std::clamp((y + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(h - 1));
    const uint32_t y0 = static_cast<uint32_t>(sy);
    const uint32_t y1 = std::min(y0 + 1, h - 1);
    const float fy = sy - y0;
    for (uint32_t x = 0; x < W; ++x) {
      const float v = field[static_cast<size_t>(y) * W + x];
      const float a = std::clamp(0.5f + (v - 0.5f) / gradient_at(field, W, H, x, y), 0.0f, 1.0f);

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
  build_mips(level, W, H, mips);

  return ConvertedTexture{
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = W,
      .height = H,
      .mips = mips,
      .data = std::move(data),
  };
}

std::optional<ConvertedTexture> sharpen_flat(const ConvertedTexture& icon) noexcept {
  const uint32_t w = icon.width;
  const uint32_t h = icon.height;
  if (icon.format != wgpu::TextureFormat::RGBA8Unorm || w < 2 || h < 2 || w > 256 || h > 256 ||
      icon.data.size() < static_cast<size_t>(w) * h * 4) {
    return std::nullopt;
  }
  const size_t n = static_cast<size_t>(w) * h;
  const uint8_t* src = icon.data.data();
  std::vector<Texel> P(n);
  for (size_t i = 0; i < n; ++i) {
    const float a = src[i * 4 + 3] / 255.0f;
    P[i] = {src[i * 4] * a, src[i * 4 + 1] * a, src[i * 4 + 2] * a, static_cast<float>(src[i * 4 + 3])};
  }

  // Regions: k-means over the flat texels (every channel within 20 of the four neighbours; edge texels are mixes),
  // with the fewest colours whose RMS error is under 10.
  std::vector<Texel> flat;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const Texel& p = P[static_cast<size_t>(y) * w + x];
      float diff = 0.0f;
      const std::array<std::pair<int, int>, 4> nb{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
      for (const auto& [dx, dy] : nb) {
        const int nx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
        const int ny = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
        const Texel& q = P[static_cast<size_t>(ny) * w + nx];
        for (int c = 0; c < 4; ++c) diff = std::max(diff, std::abs(p[c] - q[c]));
      }
      if (diff < 20.0f) flat.push_back(p);
    }
  }
  if (flat.size() <= 8) flat = P;
  std::vector<Texel> C;
  for (size_t K = 1; K <= 8; ++K) {
    float rms = 0.0f;
    C = kmeans(flat, K, rms);
    if (rms < 10.0f) break;
  }
  const size_t K = C.size();

  // Two regions may only mix when no third region's colour lies between them (clear | half-clear fill | outline:
  // the fill is not a mix of the other two).
  std::vector<uint8_t> neighbours(K * K, 1);
  for (size_t i = 0; i < K; ++i) {
    for (size_t j = 0; j < K; ++j) {
      const float L = std::max(distance2(C[i], C[j]), 1e-6f);
      for (size_t k = 0; k < K; ++k) {
        if (k == i || k == j) continue;
        float t = 0.0f;
        for (int c = 0; c < 4; ++c) t += (C[k][c] - C[j][c]) * (C[i][c] - C[j][c]);
        t /= L;
        Texel onLine;
        for (int c = 0; c < 4; ++c) onLine[c] = C[j][c] + t * (C[i][c] - C[j][c]);
        if (t > 0.1f && t < 0.9f && distance2(C[k], onLine) < 0.0625f * L) neighbours[i * K + j] = 0;
      }
    }
  }

  // Membership of each texel in each region: its nearest region, or the best mix of two neighbouring regions among
  // its three nearest when that fits better.
  std::vector<float> member(K * n, 0.0f);  // region-major: member[k * n + i]
  for (size_t i = 0; i < n; ++i) {
    std::array<std::pair<float, uint32_t>, 8> order{};
    for (uint32_t k = 0; k < K; ++k) order[k] = {distance2(P[i], C[k]), k};
    std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(K));
    member[order[0].second * n + i] = 1.0f;
    float best = order[0].first - 1.0f;
    const size_t near = std::min<size_t>(3, K);
    for (size_t a = 0; a < near; ++a) {
      for (size_t b = a + 1; b < near; ++b) {
        const uint32_t ki = order[a].second;
        const uint32_t kj = order[b].second;
        if (!neighbours[ki * K + kj]) continue;
        const float L = std::max(distance2(C[ki], C[kj]), 1e-6f);
        float t = 0.0f;
        for (int c = 0; c < 4; ++c) t += (P[i][c] - C[kj][c]) * (C[ki][c] - C[kj][c]);
        t = std::clamp(t / L, 0.0f, 1.0f);
        Texel mix;
        for (int c = 0; c < 4; ++c) mix[c] = C[kj][c] + t * (C[ki][c] - C[kj][c]);
        const float r = distance2(P[i], mix);
        if (r < best) {
          best = r;
          for (size_t k = 0; k < K; ++k) member[k * n + i] = 0.0f;
          member[ki * n + i] = t;
          member[kj * n + i] = 1.0f - t;
        }
      }
    }
  }

  const uint32_t W = w * kScale;
  const uint32_t H = h * kScale;
  const size_t N = static_cast<size_t>(W) * H;
  const auto tx = make_taps(w, kScale);
  const auto ty = make_taps(h, kScale);
  std::vector<std::vector<float>> M(K);
  for (size_t k = 0; k < K; ++k) M[k] = enlarge(member.data() + k * n, w, h, tx, ty);

  // The two strongest regions at each output texel, and the margin between them (its halfway line is their border).
  std::vector<uint8_t> first(N, 0), second(N, 0);
  std::vector<float> margin(N, 1.0f);
  for (size_t o = 0; o < N && K > 1; ++o) {
    uint32_t k1 = 0, k2 = 1;
    if (M[1][o] > M[0][o]) std::swap(k1, k2);
    for (uint32_t k = 2; k < K; ++k) {
      if (M[k][o] > M[k1][o]) {
        k2 = k1;
        k1 = k;
      } else if (M[k][o] > M[k2][o]) {
        k2 = k;
      }
    }
    first[o] = static_cast<uint8_t>(k1);
    second[o] = static_cast<uint8_t>(k2);
    margin[o] = M[k1][o] - M[k2][o];
  }

  // Shading inside a region: bilinear over that region's own nearly pure texels, else the region's mean colour.
  const auto regionColour = [&](uint32_t k, float sx, float sy) {
    const uint32_t x0 = static_cast<uint32_t>(sx);
    const uint32_t y0 = static_cast<uint32_t>(sy);
    const uint32_t x1 = std::min(x0 + 1, w - 1);
    const uint32_t y1 = std::min(y0 + 1, h - 1);
    const float fx = sx - x0;
    const float fy = sy - y0;
    const std::array<std::pair<size_t, float>, 4> corners{{
        {static_cast<size_t>(y0) * w + x0, (1 - fx) * (1 - fy)},
        {static_cast<size_t>(y0) * w + x1, fx * (1 - fy)},
        {static_cast<size_t>(y1) * w + x0, (1 - fx) * fy},
        {static_cast<size_t>(y1) * w + x1, fx * fy},
    }};
    Texel num{};
    float den = 0.0f;
    for (const auto& [i, bw] : corners) {
      const float wk = std::clamp((member[k * n + i] - 0.6f) / 0.4f, 0.0f, 1.0f) * bw;
      for (int c = 0; c < 4; ++c) num[c] += P[i][c] * wk;
      den += wk;
    }
    if (den <= 0.05f) return C[k];
    for (int c = 0; c < 4; ++c) num[c] /= den;
    return num;
  };

  const uint32_t mips = full_mip_count(W, H);
  ByteBuffer data{static_cast<size_t>(calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, W, H, mips))};
  uint8_t* level = data.data();
  for (uint32_t y = 0; y < H; ++y) {
    const float sy = std::clamp((y + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(h - 1));
    for (uint32_t x = 0; x < W; ++x) {
      const size_t o = static_cast<size_t>(y) * W + x;
      const float sx = std::clamp((x + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(w - 1));
      const Texel c1 = regionColour(first[o], sx, sy);
      const Texel c2 = K > 1 ? regionColour(second[o], sx, sy) : c1;
      Texel out = c1;
      if (K > 1) {
        const float t = std::clamp(0.5f + margin[o] / (2.0f * gradient_at(margin, W, H, x, y)), 0.5f, 1.0f);
        for (int c = 0; c < 4; ++c) out[c] = c1[c] * t + c2[c] * (1.0f - t);
      }
      uint8_t* px = level + o * 4;
      const float a = std::clamp(out[3], 0.0f, 255.0f);
      // Clear texels next to a shape take the shape's colour, so filtering never pulls in black at the edges.
      const Texel& tint = a >= 0.5f ? out : (c1[3] >= 0.5f ? c1 : c2);
      const float ta = std::max(tint[3], 1e-3f);
      for (int c = 0; c < 3; ++c) {
        px[c] = static_cast<uint8_t>(std::clamp(std::lround(tint[c] * 255.0f / ta), 0l, 255l));
      }
      px[3] = static_cast<uint8_t>(std::lround(a));
    }
  }
  build_mips(level, W, H, mips);

  return ConvertedTexture{
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = W,
      .height = H,
      .mips = mips,
      .data = std::move(data),
  };
}
} // namespace aurora::gfx::sharp_font
