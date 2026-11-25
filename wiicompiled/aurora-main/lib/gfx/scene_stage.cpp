#include "scene_stage.hpp"

#include "bloom.hpp"
#include "../gx/gx.hpp"
#include "../internal.hpp"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace aurora::gfx::scene_stage {
namespace {
using webgpu::g_device;
using webgpu::g_queue;
Module Log("aurora::gfx::scene_stage");

// ----- Tuning (world units are the game's; Scooby is about 1.3 tall)
// How far around a surface other surfaces darken it.
constexpr float kAoRadius = 0.5f;
// How dark a fully crowded spot gets (before the strength setting).
constexpr float kAoIntensity = 0.9f;
// Ignores occluders this close to the surface's own plane, per unit of distance from the camera (flat surfaces must
// not shade themselves through depth-buffer precision).
constexpr float kAoBias = 0.002f;
// Ignores occluders less than about this steep above the surface (sine of the angle): the game's low-poly rocks and
// walls meet at shallow angles, which would otherwise darken every polygon edge.
constexpr float kAoAngleBias = 0.26f;
// The occlusion fades out between these distances from the camera: far away it is small and noisy.
constexpr float kAoFadeStart = 40.f;
constexpr float kAoFadeEnd = 80.f;
// And it fades in between these: things right in front of the camera (tree trunks, posts the camera passes) cover
// much of the picture, and their shading came out blotchy; Scooby is about 4 units away.
constexpr float kAoNearFadeStart = 1.f;
constexpr float kAoNearFadeEnd = 2.5f;
// The longest sampling radius, as a share of the picture's height (close up, the radius covers a lot of screen).
constexpr float kAoMaxRadiusOfHeight = 0.05f;
// Cartoon ink outlines: line width as a share of the picture's height per level (Thin, Bold), how dark the ink is,
// and the distances over which the lines fade out (far away they would crowd into clutter).
constexpr float kInkWidthOfHeight[] = {1.f / 540.f, 1.f / 320.f};
constexpr float kInkDarkness[] = {0.85f, 0.9f};
constexpr float kInkFadeStart = 35.f;
constexpr float kInkFadeEnd = 80.f;
// The depth view's distance scale: a surface this far from the camera shows at 1/e brightness.
constexpr float kDepthViewDistance = 8.f;

std::atomic_int g_viewMode{0};
std::unordered_set<HashType> g_solidSurfaces;
std::atomic<float> g_aoStrength{0.f};
std::atomic_int g_inkLevel{0};
Camera g_camera;

// ----- Relighting experiment (NO100F_RELIGHT=<file>). The file, one item per line ('#' comments), world space:
//   ambient R G B
//   moon DX DY DZ R G B          (direction toward the moon)
//   light X Y Z RADIUS R G B
//   scale RADIUS_SCALE POINT_INTENSITY
//   far NEAR FAR                 (the light fades out from NEAR to FAR units away: distant backdrops stay as painted)
//   shadow RADIUS SIZE BIAS      (moon shadows within RADIUS units round the view, SIZE texels; RADIUS 0 = none)
//   unlit KEY                    (a texture drawn as the game draws it: the sky band, painted backdrops)
constexpr size_t kMaxRelightLights = 32;
struct RelightLight {
  float pos[3];
  float radius;
  float color[3];
};
struct RelightScene {
  bool loaded = false;
  float ambient[3] = {0.3f, 0.3f, 0.35f};
  bool hasMoon = false;
  float moonDir[3] = {0.f, 1.f, 0.f};
  float moonColor[3] = {0.5f, 0.55f, 0.7f};
  float radiusScale = 1.f;
  float pointIntensity = 1.f;
  float fadeNear = 1e6f, fadeFar = 2e6f;
  float shadowRadius = 40.f;
  uint32_t shadowSize = 2048;
  float shadowBias = 0.15f;  // units toward the moon
  std::vector<RelightLight> lights;
  std::unordered_set<std::string> unlit;
};

RelightScene load_relight_scene() {
    RelightScene s;
    const char* path = std::getenv("NO100F_RELIGHT");
    if (path == nullptr || *path == '\0') {
      return s;
    }
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
      Log.warn("Relight: can't open {}", path);
      return s;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr) {
      char word[16] = {};
      float v[7] = {};
      const int n = std::sscanf(line, "%15s %f %f %f %f %f %f %f", word, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                                &v[6]);
      if (n < 1 || word[0] == '#') continue;
      const std::string_view w(word);
      char key[128] = {};
      if (w == "unlit" && std::sscanf(line, "%15s %127s", word, key) == 2) {
        s.unlit.emplace(key);
        continue;
      }
      if (w == "ambient" && n >= 4) {
        std::copy(v, v + 3, s.ambient);
      } else if (w == "moon" && n >= 7) {
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 0.f) {
          for (int i = 0; i < 3; ++i) s.moonDir[i] = v[i] / len;
          std::copy(v + 3, v + 6, s.moonColor);
          s.hasMoon = true;
        }
      } else if (w == "light" && n >= 8 && s.lights.size() < kMaxRelightLights) {
        s.lights.push_back({{v[0], v[1], v[2]}, v[3], {v[4], v[5], v[6]}});
      } else if (w == "scale" && n >= 3) {
        s.radiusScale = v[0];
        s.pointIntensity = v[1];
      } else if (w == "shadow" && n >= 2) {
        s.shadowRadius = std::max(v[0], 0.f);
        if (n >= 3) s.shadowSize = static_cast<uint32_t>(std::clamp(v[1], 256.f, 8192.f));
        if (n >= 4) s.shadowBias = v[2];
      } else if (w == "far" && n >= 3 && v[1] > v[0]) {
        s.fadeNear = v[0];
        s.fadeFar = v[1];
      }
    }
    std::fclose(f);
    s.loaded = true;
    Log.info("Relight: {} lights, moon {}, {} unlit textures, from {}", s.lights.size(), s.hasMoon ? "on" : "off",
             s.unlit.size(), path);
    return s;
}

// The light file as last loaded. A reload (dev menu "relight reload") replaces it; the old one is kept (never freed),
// so a reader on another thread keeps a valid scene.
std::atomic<const RelightScene*> g_relightScene{nullptr};
std::atomic_bool g_relightOn{true};
std::atomic_uint g_relightGeneration{1};

const RelightScene& relight_scene() {
  const RelightScene* scene = g_relightScene.load(std::memory_order_acquire);
  if (scene == nullptr) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    scene = g_relightScene.load(std::memory_order_acquire);
    if (scene == nullptr) {
      scene = new RelightScene(load_relight_scene());
      g_relightScene.store(scene, std::memory_order_release);
    }
  }
  return *scene;
}

struct RelightParams {
  std::array<float, 4> moonDir;   // view space, toward the moon; w = 1 when there is a moon
  std::array<float, 4> moonColor;
  std::array<float, 4> ambient;
  std::array<float, 4> info;      // light count, fade near, fade far
  std::array<std::array<float, 4>, 4> shadow; // view space to shadow clip space (rows)
  std::array<float, 4> shadowInfo; // on, size, normal offset (units), bias (depth)
  std::array<std::array<float, 4>, kMaxRelightLights * 2> lights; // (view pos, radius), (colour, 0)
};

// View matrix votes: this frame's solid perspective draws' position matrices.
struct MatrixVote {
  std::array<float, 12> m;
  uint32_t n;
};
std::array<MatrixVote, 16> g_votes;
size_t g_voteCount = 0;
// This frame's shadow uniform copies (their projections' offsets in the staging buffer).
std::vector<size_t> g_shadowOffsets;
// Half the depth range of the shadow map, in units along the moon's direction.
constexpr float kShadowDepth = 200.f;

ShadowTargets g_shadowTargets;
std::array<wgpu::Texture, 5> g_shadowTextures;
uint32_t g_shadowSamples = 0;

// The moon's shadow camera for a view matrix `v` (rows of a 3x4, view = R * world + t): an orthographic box `radius`
// round a point ahead of the camera, snapped to the map's texels (so it doesn't shimmer as the camera moves), depth
// reversed (nearer the moon = larger). Returned as view space to shadow clip space.
std::array<float, 16> shadow_matrix(const std::array<float, 12>& v, const float* d, float radius, uint32_t size) {
  const float R[3][3] = {{v[0], v[1], v[2]}, {v[4], v[5], v[6]}, {v[8], v[9], v[10]}};
  const float t[3] = {v[3], v[7], v[11]};
  float cam[3], fwd[3];
  for (int i = 0; i < 3; ++i) {
    cam[i] = -(R[0][i] * t[0] + R[1][i] * t[1] + R[2][i] * t[2]);
    fwd[i] = -R[2][i];
  }
  const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  float upRef[3] = {0.f, 1.f, 0.f};
  if (std::abs(d[1]) > 0.99f) {
    upRef[1] = 0.f;
    upRef[2] = 1.f;
  }
  float up[3], right[3];
  const float k = dot(upRef, d);
  for (int i = 0; i < 3; ++i) up[i] = upRef[i] - d[i] * k;
  const float ul = std::sqrt(dot(up, up));
  for (float& c : up) c /= ul;
  // right = up x d, so right x up = d: the same handedness as a camera (triangle winding, culling, unchanged).
  right[0] = up[1] * d[2] - up[2] * d[1];
  right[1] = up[2] * d[0] - up[0] * d[2];
  right[2] = up[0] * d[1] - up[1] * d[0];
  // Centre: ahead of the camera along its horizontal forward.
  float flat[3] = {fwd[0], 0.f, fwd[2]};
  const float fl = std::sqrt(dot(flat, flat));
  float centre[3];
  for (int i = 0; i < 3; ++i) centre[i] = cam[i] + (fl > 1e-4f ? flat[i] / fl : 0.f) * radius * 0.5f;
  const float texel = 2.f * radius / static_cast<float>(size);
  const float cx = std::round(dot(right, centre) / texel) * texel;
  const float cy = std::round(dot(up, centre) / texel) * texel;
  const float cz = dot(d, centre);
  // World to shadow clip (rows of a 3x4; the fourth row is 0 0 0 1).
  const float W[3][4] = {
      {right[0] / radius, right[1] / radius, right[2] / radius, -cx / radius},
      {up[0] / radius, up[1] / radius, up[2] / radius, -cy / radius},
      {d[0] / (2.f * kShadowDepth), d[1] / (2.f * kShadowDepth), d[2] / (2.f * kShadowDepth),
       (kShadowDepth - cz) / (2.f * kShadowDepth)},
  };
  // Times view to world = [R^T | cam].
  std::array<float, 16> m{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) m[r * 4 + c] = W[r][0] * R[c][0] + W[r][1] * R[c][1] + W[r][2] * R[c][2];
    m[r * 4 + 3] = W[r][0] * cam[0] + W[r][1] * cam[1] + W[r][2] * cam[2] + W[r][3];
  }
  m[15] = 1.f;
  return m;
}

// Every pass's parameters.
struct Params {
  Mat4x4<float> inverseProjection; // clip -> view, used as vec4f(ndc, 1.0) * inv_proj like the GX shader's proj
  Mat4x4<float> projection;
  std::array<float, 4> viewport; // left, top, width, height (EFB pixels)
  std::array<float, 4> depthMap; // stored depth = x + ndc z * y
  std::array<float, 4> sizes;    // EFB width, height; occlusion target width, height
  std::array<float, 4> ao;       // radius, intensity, bias, strength
  std::array<float, 4> ao2;      // angle bias, near fade start, near fade end, unused
  std::array<float, 4> misc; // view mode, longest radius share of height (depth view: its distance), fade start, end
  std::array<float, 4> ink;  // line width (EFB pixels), darkness, unused, fade start
  std::array<float, 4> ink2; // fade end, unused
};
static_assert(sizeof(Params) == 256);

constexpr const char* kStageWgsl = R"""(
struct Params {
    inv_proj: mat4x4f,
    proj: mat4x4f,
    viewport: vec4f,
    depth_map: vec4f,
    sizes: vec4f,
    ao: vec4f,
    ao2: vec4f,
    misc: vec4f,
    ink: vec4f,
    ink2: vec4f,
};
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var depth_tex: EfbDepth;
@group(0) @binding(2) var ao_tex: texture_2d<f32>;
struct Relight {
    moon_dir: vec4f,
    moon_color: vec4f,
    ambient: vec4f,
    info: vec4f,
    shadow: array<vec4f, 4>,
    shadow_info: vec4f,
    lights: array<vec4f, 64>,
};
@group(0) @binding(3) var<uniform> relight: Relight;
@group(0) @binding(4) var shadow_tex: EfbDepth;

// How much of the moon reaches view-space point `c` (normal `n`): 3x3 samples of the moon's depth map (depth
// reversed: an occluder is nearer the moon, so larger). Outside the map: lit.
fn moon_shadow(c: vec3f, n: vec3f) -> f32 {
    if (relight.shadow_info.x == 0.0) {
        return 1.0;
    }
    let p = vec4f(c + n * relight.shadow_info.z, 1.0);
    let s = vec3f(dot(relight.shadow[0], p), dot(relight.shadow[1], p), dot(relight.shadow[2], p));
    let size = relight.shadow_info.y;
    let uv = vec2f(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * size;
    if (any(uv < vec2f(0.0)) || any(uv >= vec2f(size)) || s.z <= 0.0 || s.z >= 1.0) {
        return 1.0;
    }
    // 3x3 texels of comparisons, each bilinearly filtered: 4x4 samples weighted by where the point falls in its texel,
    // so the edge moves smoothly instead of in texel steps.
    let last = vec2i(i32(size) - 1);
    let t = uv - 0.5;
    let base = vec2i(floor(t));
    let f = t - floor(t);
    var wx = array<f32, 4>(1.0 - f.x, 1.0, 1.0, f.x);
    var wy = array<f32, 4>(1.0 - f.y, 1.0, 1.0, f.y);
    var lit = 0.0;
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let d = textureLoad(shadow_tex, clamp(base + vec2i(i - 1, j - 1), vec2i(0), last), 0);
            lit += wx[i] * wy[j] * select(0.0, 1.0, d <= s.z + relight.shadow_info.w);
        }
    }
    return lit / 9.0;
}

const SAMPLES = 12;
const TURNS = 7.0;
const TAU = 6.2831853;

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
    return vec4f(p[i], 0.0, 1.0);
}

fn efb_depth(p: vec2i) -> f32 {
    let size = vec2i(params.sizes.xy);
    return textureLoad(depth_tex, clamp(p, vec2i(0), size - 1), 0);
}

// The view-space position of the surface at EFB pixel `p` (its centre), w = 1; w = 0 where nothing solid is drawn.
// Undoes the GX shader's projection (clip = vec4f(view, 1.0) * proj) and the viewport's depth range.
fn view_pos(p: vec2i) -> vec4f {
    let d = efb_depth(p);
    let ndc_z = (d - params.depth_map.x) / params.depth_map.y;
    if (d <= 0.0 || ndc_z <= 0.0) {
        return vec4f(0.0);
    }
    let uv = (vec2f(p) + 0.5 - params.viewport.xy) / params.viewport.zw;
    let v = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndc_z, 1.0) * params.inv_proj;
    return vec4f(v.xyz / v.w, 1.0);
}

// The surface's normal, from the neighbours on the side where the depth changes least (so silhouettes don't bend it),
// facing the camera.
fn normal_at(p: vec2i, c: vec3f) -> vec3f {
    let r = view_pos(p + vec2i(1, 0));
    let l = view_pos(p - vec2i(1, 0));
    let d = view_pos(p + vec2i(0, 1));
    let u = view_pos(p - vec2i(0, 1));
    let use_r = r.w > 0.0 && (l.w == 0.0 || abs(r.z - c.z) < abs(c.z - l.z));
    let use_d = d.w > 0.0 && (u.w == 0.0 || abs(d.z - c.z) < abs(c.z - u.z));
    let dx = select(select(vec3f(0.0), c - l.xyz, l.w > 0.0), r.xyz - c, use_r);
    let dy = select(select(vec3f(0.0), c - u.xyz, u.w > 0.0), d.xyz - c, use_d);
    var n = cross(dx, dy);
    if (dot(n, n) < 1e-12) {
        return normalize(-c);
    }
    n = normalize(n);
    return select(-n, n, dot(n, -c) > 0.0);
}

// Interleaved gradient noise: a different sampling spin per pixel, which the blur averages out.
fn noise(p: vec2f) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));
}

// The occlusion at an occlusion-target pixel (half the EFB's size): (occlusion, distance), 1 = none.
@fragment
fn fs_ao(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let p = vec2i(pos.xy) * 2;
    let c4 = view_pos(p);
    if (c4.w == 0.0) {
        return vec4f(1.0, 0.0, 0.0, 0.0);
    }
    let c = c4.xyz;
    let dist = abs(c.z);
    if (dist >= params.misc.w) {
        return vec4f(1.0, dist, 0.0, 0.0);
    }
    let n = normal_at(p, c);
    let radius = params.ao.x;
    let focal = abs(params.proj[1].y) * params.viewport.w * 0.5;
    let radius_px = min(radius * focal / dist, params.sizes.y * params.misc.y);
    if (radius_px < 2.0) {
        return vec4f(1.0, dist, 0.0, 0.0);
    }
    let spin = noise(pos.xy) * TAU;
    let r2 = radius * radius;
    var sum = 0.0;
    for (var i = 0; i < SAMPLES; i++) {
        let a = (f32(i) + 0.5) / f32(SAMPLES);
        let angle = a * TURNS * TAU + spin;
        let q4 = view_pos(p + vec2i(round(vec2f(cos(angle), sin(angle)) * a * radius_px)));
        if (q4.w == 0.0) {
            continue;
        }
        let v = q4.xyz - c;
        let vv = dot(v, v);
        let f = max(r2 - vv, 0.0);
        sum += f * f * f * max((dot(v, n) - params.ao2.x * sqrt(vv) - params.ao.z * dist) / (vv + 0.01 * r2), 0.0);
    }
    let ao = max(0.0, 1.0 - sum * params.ao.y * 5.0 / (r2 * r2 * r2 * f32(SAMPLES)));
    let fade = max(smoothstep(params.misc.z, params.misc.w, dist), 1.0 - smoothstep(params.ao2.y, params.ao2.z, dist));
    return vec4f(mix(ao, 1.0, fade), dist, 0.0, 0.0);
}

// A 9-tap Gaussian along `dir` that only mixes pixels at about the same distance (edges stay sharp).
fn blur(pos: vec4f, dir: vec2i) -> vec4f {
    let p = vec2i(pos.xy);
    let size = vec2i(params.sizes.zw);
    let centre = textureLoad(ao_tex, p, 0);
    if (centre.y == 0.0) {
        return centre;
    }
    var weights = array(0.1946, 0.1216, 0.0541, 0.0162);
    var sum = centre.x * 0.2270;
    var total = 0.2270;
    let tolerance = centre.y * 0.04 + 0.05;
    for (var i = 1; i <= 4; i++) {
        for (var s = -1; s <= 1; s += 2) {
            let t = textureLoad(ao_tex, clamp(p + dir * i * s, vec2i(0), size - 1), 0);
            let k = weights[i - 1] * max(0.0, 1.0 - abs(t.y - centre.y) / tolerance);
            sum += t.x * k;
            total += k;
        }
    }
    return vec4f(sum / total, centre.y, 0.0, 0.0);
}

@fragment
fn fs_blur_h(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return blur(pos, vec2i(1, 0));
}

@fragment
fn fs_blur_v(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return blur(pos, vec2i(0, 1));
}

// The occlusion at an EFB pixel: the four nearest occlusion texels, weighted by position and by how close their
// distance is to this pixel's (so a foreground edge does not take the background's occlusion, or the reverse).
fn ao_at(p: vec2i) -> f32 {
    let c4 = view_pos(p);
    if (c4.w == 0.0) {
        return 1.0;
    }
    let dist = abs(c4.z);
    let size = vec2i(params.sizes.zw);
    let h = (vec2f(p) + 0.5) * 0.5 - 0.5;
    let base = vec2i(floor(h));
    let f = h - floor(h);
    let tolerance = dist * 0.05 + 0.05;
    var sum = 0.0;
    var total = 0.0;
    for (var j = 0; j < 2; j++) {
        for (var i = 0; i < 2; i++) {
            let t = textureLoad(ao_tex, clamp(base + vec2i(i, j), vec2i(0), size - 1), 0);
            let k = select(1.0 - f.x, f.x, i == 1) * select(1.0 - f.y, f.y, j == 1) *
                    max(0.001, 1.0 - abs(t.y - dist) / tolerance);
            sum += t.x * k;
            total += k;
        }
    }
    return sum / total;
}

// Ambient occlusion: multiplied into the solid scene.
@fragment
fn fs_apply(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let k = mix(1.0, ao_at(vec2i(pos.xy)), params.ao.w);
    return vec4f(k, k, k, 1.0);
}

// Debug views.
@fragment
fn fs_ao_view(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let k = ao_at(vec2i(pos.xy));
    return vec4f(k, k, k, 1.0);
}

// Cartoon ink. A flat surface's inverse distance changes evenly across the picture (even seen at a grazing angle), so
// a kink in it, along any of four directions, is a silhouette; only the nearer side inks, so the line sits on the
// object. A pixel inks only when one side drops away, not both: in something thinner than two lines (grass, a stem, a
// tail's tip) both do, and it keeps its colour instead of turning black. Bold is the same test at a wider reach. It
// does not ink creases: that tore up small and curved things.
const INK_KINK_LOW = 0.06;
const INK_KINK_HIGH = 0.18;
fn ink_at(p: vec2i) -> f32 {
    let c4 = view_pos(p);
    if (c4.w == 0.0) {
        return 0.0;
    }
    let c = c4.xyz;
    let dist = abs(c.z);
    let r = max(1, i32(round(params.ink.x)));
    var dirs = array(vec2i(1, 0), vec2i(0, 1), vec2i(1, 1), vec2i(1, -1));
    var ink = 0.0;
    for (var i = 0; i < 4; i++) {
        let a4 = view_pos(p + dirs[i] * r);
        let b4 = view_pos(p - dirs[i] * r);
        // Nothing drawn counts as infinitely far.
        let inv_a = select(0.0, 1.0 / abs(a4.z), a4.w > 0.0);
        let inv_b = select(0.0, 1.0 / abs(b4.z), b4.w > 0.0);
        let inv_c = 1.0 / dist;
        let kink = abs(inv_a + inv_b - 2.0 * inv_c) / inv_c;
        let one_side = (inv_a < inv_c * 0.98) != (inv_b < inv_c * 0.98);
        ink = max(ink, select(0.0, smoothstep(INK_KINK_LOW, INK_KINK_HIGH, kink), one_side));
    }
    return ink * (1.0 - smoothstep(params.ink.w, params.ink2.x, dist));
}

// Multiplied into the solid scene.
@fragment
fn fs_ink(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let k = 1.0 - ink_at(vec2i(pos.xy)) * params.ink.y;
    return vec4f(k, k, k, 1.0);
}

// Debug view: the lines alone, black on white.
@fragment
fn fs_ink_view(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let k = 1.0 - ink_at(vec2i(pos.xy));
    return vec4f(k, k, k, 1.0);
}

// Relighting: the light falling on the solid scene's surface at EFB pixel `p` (1 = the texture as drawn), from the
// ambient, the moon and the point lights; where nothing solid is drawn, 1.
fn light_at(p: vec2i) -> vec3f {
    let c4 = view_pos(p);
    if (c4.w == 0.0) {
        return vec3f(1.0);
    }
    let c = c4.xyz;
    let n = normal_at(p, c);
    if (relight.info.w != 0.0) {
        return vec3f(moon_shadow(c, n) * 2.0);  // debug view 6: the moon's shadow alone
    }
    var light = relight.ambient.rgb;
    if (relight.moon_dir.w > 0.0) {
        let facing = max(dot(n, relight.moon_dir.xyz), 0.0);
        if (facing > 0.0) {
            light += relight.moon_color.rgb * facing * moon_shadow(c, n);
        }
    }
    let count = i32(relight.info.x);
    for (var i = 0; i < count; i++) {
        let lp = relight.lights[i * 2];
        let to_light = lp.xyz - c;
        let d = length(to_light);
        let f = clamp(1.0 - (d * d) / (lp.w * lp.w), 0.0, 1.0);
        light += relight.lights[i * 2 + 1].rgb * f * f * max(dot(n, to_light / max(d, 1e-4)), 0.0);
    }
    return mix(light, vec3f(1.0), smoothstep(relight.info.y, relight.info.z, length(c)));
}

// Multiplied into the solid scene at up to twice its brightness (the blend doubles: source x destination x 2).
@fragment
fn fs_relight(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return vec4f(clamp(light_at(vec2i(pos.xy)) * 0.5, vec3f(0.0), vec3f(1.0)), 1.0);
}

// Debug view: the light alone (white = the texture as drawn, brighter clips).
@fragment
fn fs_relight_view(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return vec4f(clamp(light_at(vec2i(pos.xy)) * 0.5, vec3f(0.0), vec3f(1.0)), 1.0);
}

@fragment
fn fs_tint(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    _ = params.misc;
    return vec4f(0.35, 0.55, 1.0, 1.0);
}

@fragment
fn fs_depth(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let c = view_pos(vec2i(pos.xy));
    if (c.w == 0.0) {
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }
    let g = exp(-abs(c.z) / params.misc.y);
    return vec4f(g, g, g, 1.0);
}
)""";

enum class Kind { Occlusion, BlurH, BlurV, Apply, AoView, Tint, Depth, Ink, InkView, Relight, RelightView, Count };

struct Pipelines {
  uint32_t samples = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  std::array<wgpu::RenderPipeline, static_cast<size_t>(Kind::Count)> pipelines;
};
Pipelines g_pipelines;
wgpu::Buffer g_params;
wgpu::Buffer g_relightParams;
// The occlusion at half the EFB's size, and the blur's other half.
std::array<webgpu::TextureWithSampler, 2> g_aoTargets;

constexpr wgpu::TextureFormat kAoFormat = wgpu::TextureFormat::RG16Float;

void build_pipelines(wgpu::TextureFormat format, uint32_t samples) {
  if (g_pipelines.samples == samples && g_pipelines.format == format) {
    return;
  }
  const std::string code = std::string(samples > 1 ? "alias EfbDepth = texture_depth_multisampled_2d;\n"
                                                   : "alias EfbDepth = texture_depth_2d;\n") +
                           kStageWgsl;
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Scene stage"};
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const wgpu::BlendState multiply{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Dst,
                .dstFactor = wgpu::BlendFactor::Zero},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  // source x destination x 2: the relighting can brighten up to twice what was drawn.
  const wgpu::BlendState multiply2x{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Dst,
                .dstFactor = wgpu::BlendFactor::Src},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  const auto make = [&](const char* entry, bool efb, bool multiplies, bool doubles = false) {
    // Over the EFB: its format and sample count, and never its alpha (the game blends with it).
    const wgpu::ColorTargetState target{
        .format = efb ? format : kAoFormat,
        .blend = doubles ? &multiply2x : multiplies ? &multiply : nullptr,
        .writeMask = efb ? wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue
                         : wgpu::ColorWriteMask::All,
    };
    const wgpu::FragmentState fragment{.module = module, .entryPoint = entry, .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = entry,
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = efb ? samples : 1},
        .fragment = &fragment,
    };
    return g_device.CreateRenderPipeline(&descriptor);
  };
  auto& p = g_pipelines.pipelines;
  p[static_cast<size_t>(Kind::Occlusion)] = make("fs_ao", false, false);
  p[static_cast<size_t>(Kind::BlurH)] = make("fs_blur_h", false, false);
  p[static_cast<size_t>(Kind::BlurV)] = make("fs_blur_v", false, false);
  p[static_cast<size_t>(Kind::Apply)] = make("fs_apply", true, true);
  p[static_cast<size_t>(Kind::AoView)] = make("fs_ao_view", true, false);
  p[static_cast<size_t>(Kind::Tint)] = make("fs_tint", true, true);
  p[static_cast<size_t>(Kind::Depth)] = make("fs_depth", true, false);
  p[static_cast<size_t>(Kind::Ink)] = make("fs_ink", true, true);
  p[static_cast<size_t>(Kind::InkView)] = make("fs_ink_view", true, false);
  p[static_cast<size_t>(Kind::Relight)] = make("fs_relight", true, true, true);
  p[static_cast<size_t>(Kind::RelightView)] = make("fs_relight_view", true, false);
  g_pipelines.samples = samples;
  g_pipelines.format = format;
}

void prepare_ao_targets(uint32_t width, uint32_t height) {
  for (auto& target : g_aoTargets) {
    if (target.texture && target.size.width == width && target.size.height == height) {
      continue;
    }
    const wgpu::TextureDescriptor descriptor{
        .label = "Scene stage occlusion",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {width, height, 1},
        .format = kAoFormat,
    };
    target.texture = g_device.CreateTexture(&descriptor);
    target.view = target.texture.CreateView();
    target.size = descriptor.size;
    target.format = kAoFormat;
  }
}

// One full-screen triangle with `kind`'s pipeline into `target` (`size`). Only the bindings the pass reads are bound
// (the pipelines' layouts are derived from what each entry point uses).
void draw(const wgpu::CommandEncoder& encoder, Kind kind, const wgpu::TextureView& target, wgpu::Extent3D size,
          const wgpu::TextureView* depth, const wgpu::TextureView* ao) {
  const auto& pipeline = g_pipelines.pipelines[static_cast<size_t>(kind)];
  std::vector<wgpu::BindGroupEntry> entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = g_params, .size = sizeof(Params)},
  };
  if (depth != nullptr)
    entries.push_back(wgpu::BindGroupEntry{.binding = 1, .textureView = *depth});
  if (ao != nullptr)
    entries.push_back(wgpu::BindGroupEntry{.binding = 2, .textureView = *ao});
  if (kind == Kind::Relight || kind == Kind::RelightView) {
    entries.push_back(wgpu::BindGroupEntry{.binding = 3, .buffer = g_relightParams, .size = sizeof(RelightParams)});
    entries.push_back(wgpu::BindGroupEntry{.binding = 4, .textureView = g_shadowTargets.depth});
  }
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = pipeline.GetBindGroupLayout(0),
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const wgpu::RenderPassColorAttachment attachment{
      .view = target,
      .loadOp = wgpu::LoadOp::Load,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Scene stage",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  const auto pass = encoder.BeginRenderPass(&passDescriptor);
  pass.SetViewport(0.f, 0.f, static_cast<float>(size.width), static_cast<float>(size.height), 0.f, 1.f);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.Draw(3);
  pass.End();
}

// The inverse of a 4x4 matrix, from its cofactors: inverse[r][c] = cofactor[c][r] / determinant (any layout: the inverse
// of the transpose is the transpose of the inverse); nothing if it has none.
std::optional<Mat4x4<float>> inverse(const Mat4x4<float>& matrix) {
  const std::array rows{&matrix.m0, &matrix.m1, &matrix.m2, &matrix.m3};
  const auto at = [&rows](size_t row, size_t column) { return rows[row]->m[column]; };
  // The three rows or columns left when one is taken out.
  const auto others = [](size_t skipped) {
    std::array<size_t, 3> left{};
    for (size_t i = 0, n = 0; i < 4; ++i) {
      if (i != skipped)
        left[n++] = i;
    }
    return left;
  };
  // The determinant of what is left without this row and column, expanded along its first row, signed by position.
  const auto cofactor = [&](size_t row, size_t column) {
    const auto r = others(row);
    const auto c = others(column);
    const auto lower2x2 = [&](size_t a, size_t b) {
      return at(r[1], c[a]) * at(r[2], c[b]) - at(r[1], c[b]) * at(r[2], c[a]);
    };
    const float minor = at(r[0], c[0]) * lower2x2(1, 2) - at(r[0], c[1]) * lower2x2(0, 2) + at(r[0], c[2]) * lower2x2(0, 1);
    return (row + column) % 2 == 0 ? minor : -minor;
  };
  float det = 0.f;
  for (size_t column = 0; column < 4; ++column)
    det += at(0, column) * cofactor(0, column);
  if (!std::isfinite(det) || std::fabs(det) < 1e-20f) {
    return std::nullopt;
  }
  Mat4x4<float> out{};
  const std::array outRows{&out.m0, &out.m1, &out.m2, &out.m3};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column)
      outRows[row]->m[column] = cofactor(column, row) / det;
  }
  return out;
}
} // namespace

bool relighting() noexcept { return g_relightOn.load(std::memory_order_relaxed) && relight_scene().loaded; }

int relight_state() noexcept {
  if (!relight_scene().loaded) return -1;
  return g_relightOn.load(std::memory_order_relaxed) ? 1 : 0;
}

void set_relight(int mode) noexcept {
  if (mode == 2) {
    g_relightScene.store(new RelightScene(load_relight_scene()), std::memory_order_release);
  } else {
    g_relightOn.store(mode != 0, std::memory_order_relaxed);
  }
  g_relightGeneration.fetch_add(1, std::memory_order_relaxed);
}

unsigned relight_generation() noexcept { return g_relightGeneration.load(std::memory_order_relaxed); }

bool is_unlit_texture(const std::string& key) noexcept {
  const auto& unlit = relight_scene().unlit;
  return !key.empty() && unlit.contains(key);
}

bool wanted() noexcept {
  return g_viewMode.load(std::memory_order_relaxed) != 0 || g_aoStrength.load(std::memory_order_relaxed) > 0.f ||
         g_inkLevel.load(std::memory_order_relaxed) != 0 || bloom::wanted() || relighting();
}

void note_world_matrix(const float* matrix) noexcept {
  for (size_t i = 0; i < g_voteCount; ++i) {
    if (std::equal(matrix, matrix + 12, g_votes[i].m.begin())) {
      ++g_votes[i].n;
      return;
    }
  }
  if (g_voteCount < g_votes.size()) {
    auto& v = g_votes[g_voteCount++];
    std::copy(matrix, matrix + 12, v.m.begin());
    v.n = 1;
  }
}

void set_ink_outlines(int level) noexcept {
  g_inkLevel.store(level >= 0 && level <= 2 ? level : 0, std::memory_order_relaxed);
}
int ink_outlines() noexcept { return g_inkLevel.load(std::memory_order_relaxed); }

void begin_frame() noexcept {
  g_solidSurfaces.clear();
  g_voteCount = 0;
  g_shadowOffsets.clear();
}

void note_shadow_uniform(size_t offset) noexcept { g_shadowOffsets.push_back(offset); }

void write_shadow_uniforms(uint8_t* uniforms) noexcept {
  static constexpr std::array<float, 16> kNothing{};
  const auto& m = g_camera.hasShadow ? g_camera.shadow : kNothing;
  for (const size_t offset : g_shadowOffsets) std::memcpy(uniforms + offset, m.data(), sizeof(m));
}

void drop_shadow_uniforms() noexcept { g_shadowOffsets.clear(); }

const ShadowTargets& shadow_targets(uint32_t samples, bool motion) {
  const uint32_t size = relight_scene().shadowSize;
  if (g_shadowSamples != samples || g_shadowTargets.size != size) {
    g_shadowTextures = {};
    g_shadowTargets = {};
    g_shadowSamples = samples;
    g_shadowTargets.size = size;
  }
  const auto make = [&](size_t i, wgpu::TextureView& view, wgpu::TextureFormat format, bool sampled) {
    if (g_shadowTextures[i]) return;
    const wgpu::TextureDescriptor descriptor{
        .label = "Moon shadow map",
        .usage = wgpu::TextureUsage::RenderAttachment |
                 (sampled ? wgpu::TextureUsage::TextureBinding : wgpu::TextureUsage::None),
        .size = {size, size, 1},
        .format = format,
        .sampleCount = samples,
    };
    g_shadowTextures[i] = g_device.CreateTexture(&descriptor);
    view = g_shadowTextures[i].CreateView();
  };
  make(0, g_shadowTargets.depth, webgpu::g_graphicsConfig.depthFormat, true);
  make(1, g_shadowTargets.color, webgpu::g_graphicsConfig.surfaceConfiguration.format, false);
  if (motion) {
    make(2, g_shadowTargets.motion, wgpu::TextureFormat::RG16Float, false);
    make(3, g_shadowTargets.motionDepth, wgpu::TextureFormat::R32Float, false);
    make(4, g_shadowTargets.mark, wgpu::TextureFormat::R8Unorm, false);
  }
  return g_shadowTargets;
}
void note_solid_surface(HashType surface) noexcept { g_solidSurfaces.insert(surface); }
bool is_solid_surface(HashType surface) noexcept { return g_solidSurfaces.contains(surface); }

void set_ambient_occlusion(float strength) noexcept {
  g_aoStrength.store(std::isfinite(strength) ? std::clamp(strength, 0.f, 1.f) : 0.f, std::memory_order_relaxed);
}
float ambient_occlusion() noexcept { return g_aoStrength.load(std::memory_order_relaxed); }

void set_view(int mode) noexcept { g_viewMode.store(mode >= 0 && mode <= 6 ? mode : 0, std::memory_order_relaxed); }
int view() noexcept { return g_viewMode.load(std::memory_order_relaxed); }

void note_camera(const Mat4x4<float>& projection, const webgpu::Viewport& viewport) noexcept {
  g_camera.projection = projection;
  g_camera.viewport = viewport;
}
const Camera& camera() noexcept {
  const MatrixVote* best = nullptr;
  for (size_t i = 0; i < g_voteCount; ++i) {
    if (best == nullptr || g_votes[i].n > best->n) best = &g_votes[i];
  }
  g_camera.hasView = best != nullptr;
  if (best != nullptr) g_camera.view = best->m;
  const auto& scene = relight_scene();
  g_camera.hasShadow = g_camera.hasView && scene.hasMoon && scene.shadowRadius > 0.f;
  if (g_camera.hasShadow)
    g_camera.shadow = shadow_matrix(g_camera.view, scene.moonDir, scene.shadowRadius, scene.shadowSize);
  return g_camera;
}

void encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, const wgpu::TextureView& depth,
            wgpu::Extent3D size, uint32_t samples, const Camera& camera) {
  const int mode = view();
  const float strength = ambient_occlusion();
  const int ink = ink_outlines();
  const bool relight = relighting() && camera.hasView;
  if ((mode == 0 && strength <= 0.f && ink == 0 && !relight) || !color || !depth || size.width == 0 ||
      size.height == 0) {
    return;
  }
  // Without a camera that can be inverted (no perspective draw has written depth yet) no position can be worked out,
  // so nothing that reads the depth runs: it would ink or shade from nonsense. The tint view reads none.
  const auto inverseProjection = inverse(camera.projection);
  if (!inverseProjection && mode != 1) {
    return;
  }
  // The view shows the Bold lines when the setting is off.
  const int inkLevel = ink != 0 ? ink : 2;
  ZoneScoped;
  build_pipelines(webgpu::g_graphicsConfig.surfaceConfiguration.format, samples);
  if (!g_params) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Scene stage parameters",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(Params),
    };
    g_params = g_device.CreateBuffer(&descriptor);
  }
  const wgpu::Extent3D half{std::max(1u, (size.width + 1) / 2), std::max(1u, (size.height + 1) / 2), 1};
  const auto& vp = camera.viewport;
  // The viewport's depth range, as common.cpp applies it (reversed Z flips it).
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  const Params params{
      .inverseProjection = inverseProjection.value_or(Mat4x4<float>{}),
      .projection = camera.projection,
      .viewport = {vp.left, vp.top, std::max(vp.width, 1.f), std::max(vp.height, 1.f)},
      .depthMap = {minDepth, maxDepth - minDepth != 0.f ? maxDepth - minDepth : 1.f, 0.f, 0.f},
      .sizes = {static_cast<float>(size.width), static_cast<float>(size.height), static_cast<float>(half.width),
                static_cast<float>(half.height)},
      .ao = {kAoRadius, kAoIntensity, kAoBias, strength},
      .ao2 = {kAoAngleBias, kAoNearFadeStart, kAoNearFadeEnd, 0.f},
      .misc = {static_cast<float>(mode), mode == 2 ? kDepthViewDistance : kAoMaxRadiusOfHeight, kAoFadeStart,
               kAoFadeEnd},
      .ink = {std::max(1.f, static_cast<float>(size.height) * kInkWidthOfHeight[inkLevel - 1]),
              kInkDarkness[inkLevel - 1], 0.f, kInkFadeStart},
      .ink2 = {kInkFadeEnd, 0.f, 0.f, 0.f},
  };
  g_queue.WriteBuffer(g_params, 0, &params, sizeof(params));
  if (relight || mode == 5 || mode == 6) {
    const auto& scene = relight_scene();
    const auto& v = camera.view;  // rows: view = R * world + t
    const auto toView = [&](const float* p, float w, float* out) {
      for (int r = 0; r < 3; ++r) out[r] = v[r * 4] * p[0] + v[r * 4 + 1] * p[1] + v[r * 4 + 2] * p[2] + v[r * 4 + 3] * w;
    };
    RelightParams rp{};
    if (scene.hasMoon) {
      toView(scene.moonDir, 0.f, rp.moonDir.data());
      rp.moonDir[3] = 1.f;
    }
    std::copy(scene.moonColor, scene.moonColor + 3, rp.moonColor.begin());
    std::copy(scene.ambient, scene.ambient + 3, rp.ambient.begin());
    rp.info[0] = static_cast<float>(scene.lights.size());
    rp.info[1] = scene.fadeNear;
    rp.info[2] = scene.fadeFar;
    rp.info[3] = mode == 6 ? 1.f : 0.f;
    shadow_targets(samples, false);  // the depth map is bound even when the frame has no shadows
    if (camera.hasShadow) {
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) rp.shadow[r][c] = camera.shadow[r * 4 + c];
      const float size = static_cast<float>(scene.shadowSize);
      rp.shadowInfo = {1.f, size, 6.f * scene.shadowRadius / size, scene.shadowBias / (2.f * kShadowDepth)};
    }
    for (size_t i = 0; i < scene.lights.size(); ++i) {
      const auto& l = scene.lights[i];
      toView(l.pos, 1.f, rp.lights[i * 2].data());
      rp.lights[i * 2][3] = l.radius * scene.radiusScale;
      for (int c = 0; c < 3; ++c) rp.lights[i * 2 + 1][c] = l.color[c] * scene.pointIntensity;
    }
    if (!g_relightParams) {
      const wgpu::BufferDescriptor descriptor{
          .label = "Scene stage relighting",
          .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
          .size = sizeof(RelightParams),
      };
      g_relightParams = g_device.CreateBuffer(&descriptor);
    }
    g_queue.WriteBuffer(g_relightParams, 0, &rp, sizeof(rp));
    if (mode == 5 || mode == 6) {
      draw(encoder, Kind::RelightView, color, size, &depth, nullptr);
      return;
    }
    draw(encoder, Kind::Relight, color, size, &depth, nullptr);
  }
  if (mode == 1) {
    draw(encoder, Kind::Tint, color, size, nullptr, nullptr);
    return;
  }
  if (mode == 2) {
    draw(encoder, Kind::Depth, color, size, &depth, nullptr);
    return;
  }
  if (mode == 4) {
    draw(encoder, Kind::InkView, color, size, &depth, nullptr);
    return;
  }
  if (strength > 0.f || mode == 3) {
    prepare_ao_targets(half.width, half.height);
    draw(encoder, Kind::Occlusion, g_aoTargets[0].view, half, &depth, nullptr);
    draw(encoder, Kind::BlurH, g_aoTargets[1].view, half, nullptr, &g_aoTargets[0].view);
    draw(encoder, Kind::BlurV, g_aoTargets[0].view, half, nullptr, &g_aoTargets[1].view);
    draw(encoder, mode == 3 ? Kind::AoView : Kind::Apply, color, size, &depth, &g_aoTargets[0].view);
    if (mode == 3) {
      return;
    }
  }
  if (ink != 0) {
    draw(encoder, Kind::Ink, color, size, &depth, nullptr);
  }
}

void release_unused_targets() noexcept {
  if (g_aoTargets[0].texture && ambient_occlusion() <= 0.f && view() != 3) {
    g_aoTargets = {};
  }
}

bool holds_targets() noexcept { return static_cast<bool>(g_aoTargets[0].texture); }

void shutdown() noexcept {
  g_pipelines = {};
  g_params = {};
  g_relightParams = {};
  g_shadowTextures = {};
  g_shadowTargets = {};
  g_shadowSamples = 0;
  g_aoTargets = {};
}
} // namespace aurora::gfx::scene_stage
