#include "scene_stage.hpp"

#include "bloom.hpp"
#include "texture_replacement.hpp"
#include "../gx/gx.hpp"
#include "../internal.hpp"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
//   moonshadow STRENGTH          (how dark the moon's shadows are: 1 = all its light blocked, 0 = none)
//   lampshadow COUNT [SIZE [BIAS]] (the COUNT (up to 4, 0 = none) lamps nearest the view cast shadows, each from a
//                                 depth map of SIZE texels looking down from its globe; BIAS in units)
//   unlit KEY                    (a texture drawn as the game draws it: the sky band, painted backdrops)
//   hide KEY                     (a texture not drawn at all: the game's painted shadow decals)
//   opaque KEY                   (a texture drawn solid, never blended: trees the game fades near the camera)
//   emissive KEY STRENGTH [HALO [R G B]] (a texture that glows: lit as usual, then STRENGTH (0..1) of its own colour
//                                 added; with HALO (0..1), that share also makes a thin halo round it (needs the bloom
//                                 on); with R G B (0..1), both glow in that colour at the texture's brightness)
//   spin KEY                     (models with this texture turned about their own up axis by an angle from where they
//                                 stand: pickups the game spins in step start at different angles)
//   deblue S                     (0..1: how much blue the original textures lose under the new light)
//   fire X Y Z [SIZE [INTENSITY]] (a campfire on the ground there: flickering light, flame, embers, logs; SIZE = the
//                                 scorched patch's radius, default 0.6)
//   firemask 0|1                 (the flame's core into the depth buffer, default 1)
//   lamp X GROUND_Y Z            (a mercury-vapour post-top lamp standing there; its light 3.5 above the ground)
//   lampcolor R G B [RADIUS]     (every lamp's light; default 0.5 0.85 1.0, radius 5)
//   mountains LOW_DEG HIGH_DEG R G B (ridges round the sky's horizon between those heights; colour of the nearest)
//   move ID X Y Z [YAW]          (read by the runtime: an entity moved there, no100f_devmenu.cpp)
//   shift ID DX DY DZ            (read by the runtime: a trigger moved by an offset)
// Each level's file is reimagined/<level>.txt beside the game; all of Reimagined: .agents/reimagined.md.
//   sky ZR ZG ZB HR HG HB        (a night sky behind everything: zenith and horizon colours; hide the game's band)
//   moondisc RADIUS_DEG R G B    (the moon drawn in the sky at the moon's direction; radius 0 = none)
//   moontex KEY                  (the moon's picture: a texture name, looked up like the game's, so a pack can
//                                 replace it; the disc's colour multiplies it; none or missing: a drawn disc)
//   stars DENSITY                (0 to about 0.1)
constexpr size_t kMaxRelightLights = 32;
constexpr float kLampGlobeCentreForLight = 3.5f;  // a lamp's light: this far above its ground
struct RelightLight {
  float pos[3];
  float radius;
  float color[3];
  bool shadow = false;  // casts shadows from the solid scene's depth (the lamps' lights)
};
struct RelightScene {
  bool loaded = false;
  float ambient[3] = {0.3f, 0.3f, 0.35f};
  bool hasMoon = false;
  float moonDir[3] = {0.f, 1.f, 0.f};
  float moonColor[3] = {0.5f, 0.55f, 0.7f};
  float radiusScale = 1.f;
  float pointIntensity = 1.f;
  float deblue = 0.f;
  bool hasFire = false;
  float firePos[3] = {};
  float fireSize = 0.6f;
  bool fireMask = true;  // "firemask 0": no flame core in the depth buffer (for comparing)
  float fireIntensity = 1.f;
  bool hasMountains = false;
  float mountainLow = 2.f, mountainHigh = 7.f;
  float mountainColor[3] = {0.03f, 0.07f, 0.08f};  // "deblue S": 0..1, how much of the original textures' blue (positive Cb) goes
  float fadeNear = 1e6f, fadeFar = 2e6f;
  float shadowRadius = 40.f;
  uint32_t shadowSize = 2048;
  float shadowBias = 0.15f;  // units toward the moon
  std::vector<RelightLight> lights;
  std::unordered_set<std::string> unlit;
  std::unordered_set<std::string> hidden;
  std::unordered_set<std::string> opaque;
  std::unordered_map<std::string, std::array<float, 5>> emissive;  // "emissive KEY STRENGTH [HALO [R G B]]"
  std::unordered_set<std::string> spin;
  std::vector<std::array<float, 3>> lamps;    // "lamp X GROUND_Y Z"
  float lampColor[3] = {0.5f, 0.85f, 1.f};   // "lampcolor R G B [RADIUS]": the light each lamp adds at its globe
  float lampRadius = 5.f;
  float moonShadowStrength = 1.f;
  uint32_t lampShadowCount = kMaxLampShadows;
  uint32_t lampShadowSize = 1024;
  float lampShadowBias = 0.05f;
  bool hasSky = false;
  float skyZenith[3] = {0.02f, 0.035f, 0.07f};
  float skyHorizon[3] = {0.08f, 0.2f, 0.19f};
  float moonRadiusDeg = 2.0f;
  float moonDisc[3] = {0.93f, 0.93f, 0.86f};
  std::string moonTexture;
  float starDensity = 0.035f;
};

// The light file: the one set_relight_file gave (the runtime: this level's), else NO100F_REIMAGINED / NO100F_RELIGHT.
std::mutex g_relightPathMutex;
std::string g_relightPath;
bool g_relightPathSet = false;

RelightScene load_relight_scene() {
    RelightScene s;
    std::string chosen;
    {
      std::lock_guard lock(g_relightPathMutex);
      if (g_relightPathSet) chosen = g_relightPath;
    }
    if (chosen.empty()) {
      const char* env = std::getenv("NO100F_REIMAGINED");
      if (env == nullptr || *env == '\0') env = std::getenv("NO100F_RELIGHT");
      if (env != nullptr && !g_relightPathSet) chosen = env;
    }
    if (chosen.empty()) {
      return s;
    }
    const char* path = chosen.c_str();
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
      if (w == "hide" && std::sscanf(line, "%15s %127s", word, key) == 2) {
        s.hidden.emplace(key);
        continue;
      }
      float strength = 0.f;
      float halo = 0.f;
      float tint[3] = {0.f, 0.f, 0.f};
      if (w == "emissive" && std::sscanf(line, "%15s %127s %f %f %f %f %f", word, key, &strength, &halo, &tint[0],
                                         &tint[1], &tint[2]) >= 3) {
        s.emissive[key] = {std::clamp(strength, 0.f, 1.f), std::clamp(halo, 0.f, 1.f), std::clamp(tint[0], 0.f, 1.f),
                           std::clamp(tint[1], 0.f, 1.f), std::clamp(tint[2], 0.f, 1.f)};
        continue;
      }
      if (w == "moontex" && std::sscanf(line, "%15s %127s", word, key) == 2) {
        s.moonTexture = key;
        continue;
      }
      if (w == "spin" && std::sscanf(line, "%15s %127s", word, key) == 2) {
        s.spin.emplace(key);
      } else if (w == "opaque" && std::sscanf(line, "%15s %127s", word, key) == 2) {
        s.opaque.emplace(key);
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
      } else if (w == "fire" && n >= 4) {
        s.hasFire = true;
        std::copy(v, v + 3, s.firePos);
        if (n >= 5) s.fireSize = std::max(v[3], 0.05f);
        if (n >= 6) s.fireIntensity = std::max(v[4], 0.f);
      } else if (w == "lamp" && n >= 4) {
        s.lamps.push_back({v[0], v[1], v[2]});
      } else if (w == "moonshadow" && n >= 2) {
        s.moonShadowStrength = std::clamp(v[0], 0.f, 1.f);
      } else if (w == "lampshadow" && n >= 2) {
        s.lampShadowCount = static_cast<uint32_t>(std::clamp(v[0], 0.f, static_cast<float>(kMaxLampShadows)));
        if (n >= 3) s.lampShadowSize = static_cast<uint32_t>(std::clamp(v[1], 128.f, 4096.f));
        if (n >= 4) s.lampShadowBias = std::max(v[2], 0.f);
      } else if (w == "lampcolor" && n >= 4) {
        std::copy(v, v + 3, s.lampColor);
        if (n >= 5) s.lampRadius = std::max(v[3], 0.f);
      } else if (w == "firemask" && n >= 2) {
        s.fireMask = v[0] != 0.f;
      } else if (w == "mountains" && n >= 6) {
        s.hasMountains = true;
        s.mountainLow = v[0];
        s.mountainHigh = std::max(v[1], v[0]);
        std::copy(v + 2, v + 5, s.mountainColor);
      } else if (w == "deblue" && n >= 2) {
        s.deblue = std::clamp(v[0], 0.f, 1.f);
      } else if (w == "scale" && n >= 3) {
        s.radiusScale = v[0];
        s.pointIntensity = v[1];
      } else if (w == "shadow" && n >= 2) {
        s.shadowRadius = std::max(v[0], 0.f);
        if (n >= 3) s.shadowSize = static_cast<uint32_t>(std::clamp(v[1], 256.f, 8192.f));
        if (n >= 4) s.shadowBias = v[2];
      } else if (w == "sky" && n >= 7) {
        s.hasSky = true;
        std::copy(v, v + 3, s.skyZenith);
        std::copy(v + 3, v + 6, s.skyHorizon);
      } else if (w == "moondisc" && n >= 5) {
        s.moonRadiusDeg = std::max(v[0], 0.f);
        std::copy(v + 1, v + 4, s.moonDisc);
      } else if (w == "stars" && n >= 2) {
        s.starDensity = std::clamp(v[0], 0.f, 0.5f);
      } else if (w == "far" && n >= 3 && v[1] > v[0]) {
        s.fadeNear = v[0];
        s.fadeFar = v[1];
      }
    }
    std::fclose(f);
    for (const auto& l : s.lamps) {  // each lamp's light, in its globe
      if (s.lights.size() >= kMaxRelightLights - 1) break;  // the last slot is the campfire's
      s.lights.push_back({{l[0], l[1] + kLampGlobeCentreForLight, l[2]}, s.lampRadius,
                          {s.lampColor[0], s.lampColor[1], s.lampColor[2]}, true});
    }
    s.loaded = true;
    Log.info("Reimagined: {} lights, moon {}, {} unlit, {} hidden, {} opaque and {} emissive textures, sky {}, from {}",
             s.lights.size(), s.hasMoon ? "on" : "off", s.unlit.size(), s.hidden.size(), s.opaque.size(),
             s.emissive.size(), s.hasSky ? "on" : "off", path);
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
  std::array<float, 4> firePos;    // view space; w = 1 when there is a fire
  std::array<float, 4> fireInfo;   // size, unused, time (s), flicker
  std::array<float, 4> fireUp;     // world up in view space
  std::array<float, 4> fireEast;   // world +x in view space
  std::array<float, 4> extra;      // moon shadow strength, unused
  std::array<std::array<float, 4>, kMaxLampShadows * 4> lampShadow;  // each: view space to its map's clip (rows)
  std::array<float, 4> lampInfo;   // map size, near, bias (units), tan(half the view)
  std::array<std::array<float, 4>, kMaxRelightLights * 2> lights; // (view pos, radius), (colour, 0)
};

// View matrix votes: this frame's solid perspective draws' position matrices.
struct MatrixVote {
  std::array<float, 12> m;
  uint32_t n;
  float projX, projY;  // the projection's x and y scales it was drawn with
};
std::array<MatrixVote, 16> g_votes;
size_t g_voteCount = 0;
// This frame's shadow uniform copies (their projections' offsets in the staging buffer).
std::vector<size_t> g_shadowOffsets;
std::array<std::vector<size_t>, kMaxLampShadows> g_lampShadowOffsets;  // per lamp map, like g_shadowOffsets
LampShadowTargets g_lampShadowTargets;
std::array<wgpu::Texture, kMaxLampShadows + 4> g_lampShadowTextures;
uint32_t g_lampShadowSamples = 0;
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

// A lamp's shadow map: from `light` (world) straight down, a perspective view kLampShadowTanHalf wide each way, depth
// reversed as near / distance. Returned as view space to its clip space (rows of a 4x4). Screen right is world +x and
// screen up world -z, so right x up points back at the lamp, as in a camera (winding and culling unchanged).
std::array<float, 16> lamp_shadow_matrix(const std::array<float, 12>& v, const float* light) {
  const float R[3][3] = {{v[0], v[1], v[2]}, {v[4], v[5], v[6]}, {v[8], v[9], v[10]}};
  const float t[3] = {v[3], v[7], v[11]};
  float cam[3];
  for (int i = 0; i < 3; ++i) cam[i] = -(R[0][i] * t[0] + R[1][i] * t[1] + R[2][i] * t[2]);
  const float k = 1.f / kLampShadowTanHalf;
  const float W[4][4] = {
      {k, 0.f, 0.f, -light[0] * k},
      {0.f, 0.f, -k, light[2] * k},
      {0.f, 0.f, 0.f, kLampShadowNear},
      {0.f, -1.f, 0.f, light[1]},
  };
  std::array<float, 16> m{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) m[r * 4 + c] = W[r][0] * R[c][0] + W[r][1] * R[c][1] + W[r][2] * R[c][2];
    m[r * 4 + 3] = W[r][0] * cam[0] + W[r][1] * cam[1] + W[r][2] * cam[2] + W[r][3];
  }
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
    fire_pos: vec4f,
    fire_info: vec4f,
    fire_up: vec4f,
    fire_east: vec4f,
    extra: vec4f,
    lamp_shadow: array<vec4f, 16>,
    lamp_info: vec4f,
    lights: array<vec4f, 64>,
};
@group(0) @binding(3) var<uniform> relight: Relight;
@group(0) @binding(4) var shadow_tex: EfbDepth;
@group(0) @binding(5) var lamp_tex0: EfbDepth;
@group(0) @binding(6) var lamp_tex1: EfbDepth;
@group(0) @binding(7) var lamp_tex2: EfbDepth;
@group(0) @binding(8) var lamp_tex3: EfbDepth;

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

// The shadow maps of the nearest lamps (each looks straight down from its globe, a wide perspective; depth reversed,
// near / distance), one texture each: WGSL has no arrays of textures.
fn lamp_depth(k: i32, p: vec2i) -> f32 {
    var d = 0.0;
    switch k {
        case 0: { d = textureLoad(lamp_tex0, p, 0); }
        case 1: { d = textureLoad(lamp_tex1, p, 0); }
        case 2: { d = textureLoad(lamp_tex2, p, 0); }
        default: { d = textureLoad(lamp_tex3, p, 0); }
    }
    return d;
}

// How much of lamp shadow map `k`'s light reaches view-space point `c` (normal `n`), filtered like the moon's. Outside
// the map (above the lamp, or beyond its view): lit.
fn lamp_shadow(k: i32, c: vec3f, n: vec3f) -> f32 {
    let size = relight.lamp_info.x;
    let z_near = relight.lamp_info.y;
    let m0 = relight.lamp_shadow[k * 4];
    let m1 = relight.lamp_shadow[k * 4 + 1];
    let m2 = relight.lamp_shadow[k * 4 + 2];
    let m3 = relight.lamp_shadow[k * 4 + 3];
    let dist = dot(m3, vec4f(c, 1.0));
    if (dist <= z_near) {
        return 1.0;
    }
    // Pushed out along the normal by a texel and a half there (texels grow with the distance from the lamp).
    let texel = 2.0 * relight.lamp_info.w * dist / size;
    let p = vec4f(c + n * texel * 1.5, 1.0);
    let s = vec4f(dot(m0, p), dot(m1, p), dot(m2, p), dot(m3, p));
    if (s.w <= z_near) {
        return 1.0;
    }
    let ndc = s.xy / s.w;
    if (any(abs(ndc) >= vec2f(1.0))) {
        return 1.0;
    }
    let uv = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size;
    let last = vec2i(i32(size) - 1);
    let t = uv - 0.5;
    let base = vec2i(floor(t));
    let f = t - floor(t);
    var wx = array<f32, 4>(1.0 - f.x, 1.0, 1.0, f.x);
    var wy = array<f32, 4>(1.0 - f.y, 1.0, 1.0, f.y);
    let reach = s.w - relight.lamp_info.z;
    var lit = 0.0;
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let d = lamp_depth(k, clamp(base + vec2i(i - 1, j - 1), vec2i(0), last));
            // Stored near / distance; 0 = nothing there.
            let blocker = select(1e9, z_near / max(d, 1e-9), d > 0.0);
            lit += wx[i] * wy[j] * select(0.0, 1.0, blocker >= reach);
        }
    }
    return lit / 9.0;
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
            light += relight.moon_color.rgb * facing * mix(1.0, moon_shadow(c, n), relight.extra.x);
        }
    }
    let count = i32(relight.info.x);
    for (var i = 0; i < count; i++) {
        let lp = relight.lights[i * 2];
        let to_light = lp.xyz - c;
        let d = length(to_light);
        let f = clamp(1.0 - (d * d) / (lp.w * lp.w), 0.0, 1.0);
        let lambert = max(dot(n, to_light / max(d, 1e-4)), 0.0);
        if (f <= 0.0 || lambert <= 0.0) {
            continue;
        }
        let colour = relight.lights[i * 2 + 1];
        let blocked = select(1.0, lamp_shadow(i32(colour.w) - 1, c, n), colour.w > 0.5);
        light += colour.rgb * f * f * lambert * blocked;
    }
    if (relight.fire_pos.w > 0.0) {
        light *= fire_ground(c).x;
    }
    return mix(light, vec3f(1.0), smoothstep(relight.info.y, relight.info.z, length(c)));
}

// Reimagined campfire ("fire" in the light file): the scorched ground and its logs darken the light here; the flame and
// the embers are added by fs_fire.
fn fire_ground(c: vec3f) -> vec2f {  // x: light multiplier, y: ember glow
    let rel = c - relight.fire_pos.xyz;
    let up = relight.fire_up.xyz;
    let h = dot(rel, up);
    if (abs(h) > 0.4) {
        return vec2f(1.0, 0.0);
    }
    let e = relight.fire_east.xyz;
    let q = vec2f(dot(rel, e), dot(rel, cross(up, e)));
    let r = length(q);
    let s = relight.fire_info.x;  // size (the patch's radius)
    var m = mix(0.3, 1.0, smoothstep(0.3 * s, s, r));
    var glow = (1.0 - smoothstep(0.0, 0.45 * s, r)) * 0.8;
    // The logs' feet glow where they sit in the fire.
    if (h > 0.0 && h < 0.35 * s && r < 0.6 * s) {
        glow = max(glow, (1.0 - smoothstep(0.1 * s, 0.6 * s, r)) * (1.0 - h / (0.35 * s)));
    }
    return vec2f(m, glow);
}

fn fire_hash(p: vec2f) -> f32 {
    return fract(sin(dot(p, vec2f(127.1, 311.7))) * 43758.5453);
}
fn fire_noise(p: vec2f) -> f32 {
    let i = floor(p);
    let f = p - i;
    let u = f * f * (3.0 - 2.0 * f);
    return mix(mix(fire_hash(i), fire_hash(i + vec2f(1.0, 0.0)), u.x),
               mix(fire_hash(i + vec2f(0.0, 1.0)), fire_hash(i + vec2f(1.0, 1.0)), u.x), u.y);
}

// Added over the lit scene: the flame (a camera-facing sheet through the fire, hidden behind nearer surfaces), a soft
// glow round it and the embers on the ground.
// Where the camera ray `dir` (view space) meets the flame's sheet (upright through the fire, facing the camera): across
// and up it in fire sizes, and the distance; w = 0 when it misses.
fn flame_hit(dir: vec3f) -> vec4f {
    let base = relight.fire_pos.xyz;
    let up = relight.fire_up.xyz;
    let nrm = normalize(base - up * dot(base, up));
    let denom = dot(dir, nrm);
    if (abs(denom) <= 1e-4) {
        return vec4f(0.0);
    }
    let dist = dot(base, nrm) / denom;
    if (dist <= 0.0) {
        return vec4f(0.0);
    }
    let q = dir * dist - base;
    let right = normalize(cross(up, nrm));
    let s = relight.fire_info.x;
    return vec4f(dot(q, right) / s, dot(q, up) / s, dist, 1.0);
}

// The flame at (x, y) on its sheet: density (0 outside), how far up (0..1), distance from its middle line (0..1 inside).
// A teardrop: rounded at the bottom, widest a quarter of the way up, a point at the top, swaying and licking.
fn flame_shape(x: f32, y: f32) -> vec3f {
    let t = relight.fire_info.z;
    let foot = 0.25;
    let height = 2.5;
    if (y <= foot || y >= height) {
        return vec3f(0.0, 0.0, 1.0);
    }
    let v = clamp((y - foot) / (height - foot), 0.0, 1.0);
    let sway = (fire_noise(vec2f(y * 2.0 - t * 3.0, t * 0.7)) - 0.5) * 0.3 * v;
    let shape = smoothstep(0.0, 0.22, v) * pow(1.0 - v, 0.85);
    let width = 0.62 * shape * (0.88 + 0.24 * fire_noise(vec2f(t * 4.0, y * 3.0)));
    let lick = fire_noise(vec2f(x * 5.0, y * 4.0 - t * 5.0));
    let d = abs(x - sway) / max(width, 1e-3);
    return vec3f(smoothstep(0.0, 0.12, (1.0 - d) - v * 0.5 * (1.0 - lick)), v, d);
}

fn camera_ray(frag: vec2f) -> vec3f {
    let uv = (frag - params.viewport.xy) / params.viewport.zw;
    let r4 = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0) * params.inv_proj;
    return normalize(r4.xyz / r4.w);
}

// The flame's solid middle into the depth buffer (after its colour is added), so the game's late see-through draws
// behind it (Scooby's footprints) are hidden there while anything in front of it (Scooby) still draws over it.
@fragment
fn fs_fire_depth(@builtin(position) pos: vec4f) -> @builtin(frag_depth) f32 {
    let dir = camera_ray(pos.xy);
    let hit = flame_hit(dir);
    if (hit.w == 0.0) {
        discard;
    }
    if (flame_shape(hit.x, hit.y).x < 0.5) {
        discard;
    }
    let clip = vec4f(dir * hit.z, 1.0) * params.proj;
    return params.depth_map.x + clip.z / clip.w * params.depth_map.y;
}

@fragment
fn fs_fire(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let p = vec2i(pos.xy);
    let uv = (vec2f(p) + 0.5 - params.viewport.xy) / params.viewport.zw;
    let r4 = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0) * params.inv_proj;
    let dir = normalize(r4.xyz / r4.w);
    let c4 = view_pos(p);
    let scene_dist = select(1e9, length(c4.xyz), c4.w != 0.0);
    let base = relight.fire_pos.xyz;
    let up = relight.fire_up.xyz;
    let s = relight.fire_info.x;
    let t = relight.fire_info.z;
    let flick = relight.fire_info.w;
    var col = vec3f(0.0);

    // Embers on the ground (and glowing log ends).
    if (c4.w != 0.0) {
        let g = fire_ground(c4.xyz);
        if (g.y > 0.0) {
            let rel = c4.xyz - base;
            let e = relight.fire_east.xyz;
            let q = vec2f(dot(rel, e), dot(rel, cross(up, e))) / s;
            let n = fire_noise(q * 9.0 + vec2f(t * 0.7, -t * 0.5));
            col += vec3f(1.0, 0.32, 0.06) * g.y * (0.25 + 0.75 * n * n) * flick * 0.9;
        }
    }

    // The flame sheet: faces the camera, upright.
    let hit = flame_hit(dir);
    if (hit.w != 0.0 && hit.z < scene_dist + 0.05 * s) {
        let f = flame_shape(hit.x, hit.y);
        let v = f.y;
        let inner = smoothstep(0.62, 0.5, f.z) * smoothstep(0.7, 0.35, v);
        let flame = mix(mix(vec3f(0.95, 0.22, 0.03), vec3f(1.0, 0.5, 0.08), 1.0 - v), vec3f(1.0, 0.85, 0.3), inner);
        col += flame * f.x * flick * 0.95;
        // Glow round the flame.
        let gd = length(vec2f(hit.x, (hit.y - 0.9) * 0.7));
        col += vec3f(1.0, 0.45, 0.12) * exp(-gd * 2.2) * 0.12 * flick;
    }
    return vec4f(col, 1.0);
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

enum class Kind { Occlusion, BlurH, BlurV, Apply, AoView, Tint, Depth, Ink, InkView, Relight, RelightView, Fire, Count };

struct Pipelines {
  uint32_t samples = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  std::array<wgpu::RenderPipeline, static_cast<size_t>(Kind::Count)> pipelines;
  wgpu::RenderPipeline fireDepth;  // the flame's middle into the scene's depth (no colour)
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
  // Added light (the campfire's flame and embers).
  const wgpu::BlendState additive{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::One},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  const auto make = [&](const char* entry, bool efb, bool multiplies, bool doubles = false, bool adds = false) {
    // Over the EFB: its format and sample count, and never its alpha (the game blends with it).
    const wgpu::ColorTargetState target{
        .format = efb ? format : kAoFormat,
        .blend = adds ? &additive : doubles ? &multiply2x : multiplies ? &multiply : nullptr,
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
  p[static_cast<size_t>(Kind::Fire)] = make("fs_fire", true, false, false, true);
  {
    const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_fire_depth", .targetCount = 0};
    const wgpu::DepthStencilState depthStencil{
        .format = webgpu::g_graphicsConfig.depthFormat,
        .depthWriteEnabled = true,
        .depthCompare = gx::UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "fs_fire_depth",
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .depthStencil = &depthStencil,
        .multisample = {.count = samples},
        .fragment = &fragment,
    };
    g_pipelines.fireDepth = g_device.CreateRenderPipeline(&descriptor);
  }
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
  if (kind == Kind::Relight || kind == Kind::RelightView || kind == Kind::Fire) {
    entries.push_back(wgpu::BindGroupEntry{.binding = 3, .buffer = g_relightParams, .size = sizeof(RelightParams)});
  }
  if (kind == Kind::Relight || kind == Kind::RelightView) {  // the flame doesn't read the moon's shadow map
    entries.push_back(wgpu::BindGroupEntry{.binding = 4, .textureView = g_shadowTargets.depth});
    for (uint32_t k = 0; k < kMaxLampShadows; ++k)
      entries.push_back(wgpu::BindGroupEntry{.binding = 5 + k, .textureView = g_lampShadowTargets.depth[k]});
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

// The flame's middle into `depth` (the scene's depth buffer), tested against what is there.
void draw_fire_depth(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depth, wgpu::Extent3D size) {
  const auto& pipeline = g_pipelines.fireDepth;
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = g_params, .size = sizeof(Params)},
      wgpu::BindGroupEntry{.binding = 3, .buffer = g_relightParams, .size = sizeof(RelightParams)},
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = pipeline.GetBindGroupLayout(0), .entryCount = entries.size(), .entries = entries.data()};
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const wgpu::RenderPassDepthStencilAttachment depthAttachment{
      .view = depth,
      .depthLoadOp = wgpu::LoadOp::Load,
      .depthStoreOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Reimagined flame depth",
      .colorAttachmentCount = 0,
      .depthStencilAttachment = &depthAttachment,
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

void set_relight_file(const std::string& path) noexcept {
  {
    std::lock_guard lock(g_relightPathMutex);
    g_relightPath = path;
    g_relightPathSet = true;
  }
  g_relightScene.store(new RelightScene(load_relight_scene()), std::memory_order_release);
  g_relightGeneration.fetch_add(1, std::memory_order_relaxed);
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

uint8_t emissive_strength(const std::string& key) noexcept {
  if (key.empty()) return 0;
  const auto& emissive = relight_scene().emissive;
  const auto it = emissive.find(key);
  return it == emissive.end() ? 0 : static_cast<uint8_t>(std::lround(it->second[0] * 255.f));
}

uint8_t emissive_halo(const std::string& key) noexcept {
  if (key.empty()) return 0;
  const auto& emissive = relight_scene().emissive;
  const auto it = emissive.find(key);
  return it == emissive.end() ? 0 : static_cast<uint8_t>(std::lround(it->second[1] * 255.f));
}

std::array<uint8_t, 3> emissive_tint(const std::string& key) noexcept {
  if (key.empty()) return {};
  const auto& emissive = relight_scene().emissive;
  const auto it = emissive.find(key);
  if (it == emissive.end()) return {};
  return {static_cast<uint8_t>(std::lround(it->second[2] * 255.f)),
          static_cast<uint8_t>(std::lround(it->second[3] * 255.f)),
          static_cast<uint8_t>(std::lround(it->second[4] * 255.f))};
}

bool is_unlit_texture(const std::string& key) noexcept {
  const auto& unlit = relight_scene().unlit;
  return !key.empty() && unlit.contains(key);
}

constexpr char kSkyWgsl[] = R"WGSL(
struct Sky {
    rot0: vec4f,       // view direction to world: world.x = dot(rot0.xyz, v), ...
    rot1: vec4f,
    rot2: vec4f,
    proj: vec4f,       // 1/P00, 1/P11, P02, P12
    viewport: vec4f,   // left, top, width, height (EFB pixels)
    zenith: vec4f,
    horizon: vec4f,
    moon_dir: vec4f,   // toward the moon; w = angular radius (radians), 0 = no moon
    moon_color: vec4f, // the disc; w = star density
    mountains: vec4f,  // lowest, highest ridge (radians), on, seed
    mountain_color: vec4f,
};
@group(0) @binding(0) var<uniform> sky: Sky;
@group(0) @binding(1) var moon_tex: texture_2d<f32>;
@group(0) @binding(2) var moon_sampler: sampler;

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);
}

fn hash3(p: vec3f) -> f32 {
    return fract(sin(dot(p, vec3f(127.1, 311.7, 74.7))) * 43758.5453);
}
fn hash2(p: vec2f) -> f32 {
    return fract(sin(dot(p, vec2f(127.1, 311.7))) * 43758.5453);
}
fn noise2(p: vec2f) -> f32 {
    let i = floor(p);
    let f = p - i;
    let u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2f(1.0, 0.0)), u.x),
               mix(hash2(i + vec2f(0.0, 1.0)), hash2(i + vec2f(1.0, 1.0)), u.x), u.y);
}
fn far_height(az: f32, lo: f32, hi: f32, seed: f32) -> f32 {
    return lo + (hi - lo) * ridge(vec2f(cos(az), sin(az)) * 3.0 + vec2f(seed, 1.7));
}
fn near_height(az: f32, lo: f32, hi: f32, seed: f32) -> f32 {
    return lo * 0.6 + (hi - lo) * 0.55 * ridge(vec2f(cos(az), sin(az)) * 5.0 + vec2f(9.1, seed));
}
fn ridge(p: vec2f) -> f32 {  // 0..1, peaky
    let n = 0.55 * noise2(p) + 0.3 * noise2(p * 2.3 + 5.2) + 0.15 * noise2(p * 5.1 + 1.3);
    return clamp((1.0 - abs(n * 2.0 - 1.0)) * 1.25 - 0.2, 0.0, 1.0);
}
fn fbm2(p: vec2f) -> f32 {
    return 0.5 * noise2(p) + 0.25 * noise2(p * 2.03 + 1.7) + 0.125 * noise2(p * 4.01 + 3.1) + 0.0625 * noise2(p * 8.1);
}

@fragment
fn fs_sky(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let ndc = vec2f((pos.x - sky.viewport.x) / sky.viewport.z * 2.0 - 1.0,
                    1.0 - (pos.y - sky.viewport.y) / sky.viewport.w * 2.0);
    let v = vec3f((ndc.x + sky.proj.z) * sky.proj.x, (ndc.y + sky.proj.w) * sky.proj.y, -1.0);
    let d = normalize(vec3f(dot(sky.rot0.xyz, v), dot(sky.rot1.xyz, v), dot(sky.rot2.xyz, v)));
    let h = d.y;

    // Horizon to zenith.
    var col = mix(sky.horizon.rgb, sky.zenith.rgb, smoothstep(-0.02, 0.65, h));
    col *= select(1.0, 0.75, h < 0.0);

    // Stars: one possible star per cell of a fine grid on the sphere's directions.
    let density = sky.moon_color.w;
    let sp = d * 260.0;
    let cell = floor(sp);
    let pick = hash3(cell);
    if (pick > 1.0 - density) {
        let jitter = vec3f(hash3(cell + 1.3), hash3(cell + 7.1), hash3(cell + 3.7));
        let star = cell + 0.25 + 0.5 * jitter;
        let size = 0.18 + 0.22 * hash3(cell + 11.0);
        let bright = 0.35 + 0.65 * hash3(cell + 5.0);
        let s = (1.0 - smoothstep(0.0, size, length(sp - star))) * bright * smoothstep(0.03, 0.3, h);
        col += vec3f(0.85, 0.9, 1.0) * s;
    }


    // Mountains: two ranges of ridges round the horizon (the nearer lower and darker), over the stars, under the moon.
    if (sky.mountains.z > 0.0) {
        let az = atan2(d.x, d.z);
        let el = asin(clamp(h, -1.0, 1.0));
        let lo = sky.mountains.x;
        let hi = sky.mountains.y;
        let seed = sky.mountains.w;
        let far_ridge = far_height(az, lo, hi, seed);
        let near_ridge = near_height(az, lo, hi, seed);
        // Which way the slope faces against the moon's side of the sky: lit faces lighter, the others darker.
        let moon_az = atan2(sky.moon_dir.x, sky.moon_dir.z);
        let side = sign(sin(moon_az - az));
        // Faces: the slope of the ridge over a wide window, sampled a little to the side with height (the spine
        // running down from each peak wanders), so whole faces are lit or dark rather than thin stripes.
        let wob = (noise2(vec2f(az * 40.0, el * 60.0)) - 0.5) * 0.02;
        let fa = az + wob;
        let far_slope = far_height(fa + 0.02, lo, hi, seed) - far_height(fa - 0.02, lo, hi, seed);
        let near_slope = near_height(fa + 0.02, lo, hi, seed) - near_height(fa - 0.02, lo, hi, seed);
        let far_depth = clamp((far_ridge - el) / max(far_ridge, 1e-3), 0.0, 1.0);
        let near_depth = clamp((near_ridge - el) / max(near_ridge, 1e-3), 0.0, 1.0);
        let far_lit = mix(smoothstep(-0.004, 0.004, -far_slope * side), 0.5, far_depth * 0.7);
        let near_lit = mix(smoothstep(-0.004, 0.004, -near_slope * side), 0.5, near_depth * 0.7);
        // Gullies: faint streaks down the faces.
        let gully = noise2(vec2f(az * 90.0, el * 14.0)) * 0.6 + noise2(vec2f(az * 210.0, el * 30.0)) * 0.4;
        // The far range: shaded faces, gullies, snow above a jagged snowline, hazier toward its foot.
        var far_col = sky.mountain_color.rgb * mix(0.72, 1.2, far_lit) * (0.86 + 0.14 * gully);
        let snowline = lo + 0.8 * (hi - lo) + (noise2(vec2f(az * 30.0, 1.3)) - 0.5) * 0.25 * (hi - lo)
                     - (gully - 0.5) * 0.12 * (hi - lo);
        let snow = smoothstep(snowline - 0.0015, snowline + 0.0015, el);
        let snow_col = mix(sky.mountain_color.rgb, vec3f(0.44, 0.52, 0.6), 0.7) * mix(0.68, 1.1, far_lit);
        far_col = mix(far_col, snow_col, snow);
        far_col = mix(far_col, sky.horizon.rgb, 0.3 * (1.0 - smoothstep(0.0, far_ridge, el)));
        // Soft edges (a fraction of a degree).
        let far_cover = 1.0 - smoothstep(far_ridge - 0.004, far_ridge, el);
        col = mix(col, far_col, far_cover);
        let near_col = sky.mountain_color.rgb * 0.62 * mix(0.75, 1.2, near_lit) * (0.88 + 0.12 * gully);
        let near_cover = 1.0 - smoothstep(near_ridge - 0.004, near_ridge, el);
        col = mix(col, near_col, near_cover);
    }

    // The moon: a disc with soft limb darkening and darker patches, and its glow.
    let r = sky.moon_dir.w;
    if (r > 0.0) {
        let m = sky.moon_dir.xyz;
        let c = clamp(dot(d, m), -1.0, 1.0);
        let angle = acos(c);
        let halo = exp(-max(angle - r, 0.0) / (r * 2.0)) * 0.30 + exp(-angle / 0.30) * 0.07;
        col += sky.moon_color.rgb * vec3f(0.75, 0.82, 1.0) * halo;
        if (angle < r * 1.5) {
            let t1 = normalize(cross(select(vec3f(0.0, 1.0, 0.0), vec3f(1.0, 0.0, 0.0), abs(m.y) > 0.99), m));
            let t2 = cross(m, t1);
            let q = vec2f(dot(d, t1), dot(d, t2)) / sin(r);
            let limb = sqrt(max(1.0 - dot(q, q), 0.0));
            if (sky.zenith.w > 0.5) {
                // The moon's picture over the disc's square: texture up = sky up, right = the viewer's right.
                let uv = vec2f(0.5 - q.x * 0.5, 0.5 - q.y * 0.5);
                if (all(uv >= vec2f(0.0)) && all(uv <= vec2f(1.0))) {
                    let t = textureSampleLevel(moon_tex, moon_sampler, uv, 0.0);
                    col = mix(col, t.rgb * sky.moon_color.rgb, t.a);
                }
            } else {
                let patches = smoothstep(0.42, 0.66, fbm2(q * 2.6 + vec2f(4.0, 9.0)));
                let shade = (0.78 + 0.22 * limb) * (1.0 - 0.22 * patches);
                let disc = 1.0 - smoothstep(r * 0.94, r, angle);
                col = mix(col, sky.moon_color.rgb * shade, disc);
            }
        }
    }
    return vec4f(clamp(col, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";

struct SkyParams {
  std::array<std::array<float, 4>, 11> v;
};
struct SkyPipelines {
  wgpu::RenderPipeline pipeline;
  uint32_t samples = 0;
  bool motion = false;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};
SkyPipelines g_sky;
wgpu::Buffer g_skyParams;
wgpu::Sampler g_skySampler;
wgpu::Texture g_skyNoMoon;  // 1x1 stand-in when the moon has no picture
wgpu::TextureView g_skyNoMoonView;

bool is_hidden_texture(const std::string& key) noexcept {
  const auto& hidden = relight_scene().hidden;
  return !key.empty() && hidden.contains(key);
}

uint8_t relight_deblue() noexcept {
  return static_cast<uint8_t>(std::lround(relight_scene().deblue * 255.f));
}

bool is_spin_texture(const std::string& key) noexcept {
  const auto& spin = relight_scene().spin;
  return !key.empty() && spin.contains(key);
}

bool spin_angle(const float* m, float projX, float projY, float& angle) noexcept {
  // The world matrix most drawn with so far this frame (the static world comes before the pickups), else the camera's.
  const MatrixVote* best = nullptr;
  for (size_t i = 0; i < g_voteCount; ++i)
    if (best == nullptr || g_votes[i].n > best->n) best = &g_votes[i];
  const float* v = best != nullptr ? best->m.data() : (g_camera.hasView ? g_camera.view.data() : nullptr);
  if (v == nullptr) return false;
  // Only a model standing upright in the level, seen by its camera: its up axis (the model view's second column)
  // turned back to the world must be the world's up. A model drawn with a camera of its own (the HUD's snack counter)
  // isn't, and its "place in the world" would move with the level's camera: it keeps the game's own turn.
  const float up[3] = {m[1], m[5], m[9]};
  const float len = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
  if (len < 1e-6f || (v[1] * up[0] + v[5] * up[1] + v[9] * up[2]) / len < 0.995f) return false;
  if (best != nullptr && (std::abs(projX - best->projX) > 0.01f * std::abs(best->projX) ||
                          std::abs(projY - best->projY) > 0.01f * std::abs(best->projY)))
    return false;  // drawn with another camera's lens
  // world = R^T (view - T), rows of R at 0, 4, 8; T at 3, 7, 11. Only x and z: pickups bob up and down.
  const float d[3] = {m[3] - v[3], m[7] - v[7], m[11] - v[11]};
  const float wx = v[0] * d[0] + v[4] * d[1] + v[8] * d[2];
  const float wz = v[2] * d[0] + v[6] * d[1] + v[10] * d[2];
  uint32_t h = static_cast<uint32_t>(std::lround(wx * 20.f)) * 0x9E3779B1u ^
               static_cast<uint32_t>(std::lround(wz * 20.f)) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 13;
  angle = static_cast<float>(h & 0xFFFFu) / 65536.f * 6.2831853f;
  return true;
}

bool is_opaque_texture(const std::string& key) noexcept {
  const auto& opaque = relight_scene().opaque;
  return !key.empty() && opaque.contains(key);
}

bool wanted() noexcept {
  return g_viewMode.load(std::memory_order_relaxed) != 0 || g_aoStrength.load(std::memory_order_relaxed) > 0.f ||
         g_inkLevel.load(std::memory_order_relaxed) != 0 || bloom::wanted() || relighting();
}

void note_world_matrix(const float* matrix, float projX, float projY) noexcept {
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
    v.projX = projX;
    v.projY = projY;
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
  for (auto& o : g_lampShadowOffsets) o.clear();
}

void note_shadow_uniform(size_t offset) noexcept { g_shadowOffsets.push_back(offset); }

uint32_t lamp_shadow_slots() noexcept {
  if (!relighting()) return 0;
  const auto& scene = relight_scene();
  return scene.lamps.empty() ? 0 : std::min<uint32_t>(scene.lampShadowCount, scene.lamps.size());
}

void note_lamp_shadow_uniform(uint32_t slot, size_t offset) noexcept {
  if (slot < kMaxLampShadows) g_lampShadowOffsets[slot].push_back(offset);
}

void write_shadow_uniforms(uint8_t* uniforms) noexcept {
  static constexpr std::array<float, 16> kNothing{};
  const auto& m = g_camera.hasShadow ? g_camera.shadow : kNothing;
  for (const size_t offset : g_shadowOffsets) std::memcpy(uniforms + offset, m.data(), sizeof(m));
  for (uint32_t k = 0; k < kMaxLampShadows; ++k) {
    const auto& l = k < g_camera.lampShadowCount ? g_camera.lampShadow[k] : kNothing;
    for (const size_t offset : g_lampShadowOffsets[k]) std::memcpy(uniforms + offset, l.data(), sizeof(l));
  }
}

void drop_shadow_uniforms() noexcept {
  g_shadowOffsets.clear();
  for (auto& o : g_lampShadowOffsets) o.clear();
}


// ---- Reimagined campfire props: logs in a teepee and a ring of rocks round the light file's fire, real geometry drawn
// into the scene's pass after the sky (depth written, so the game's draws hide them and the stage lights them).
constexpr char kPropsWgsl[] = R"WGSL(
struct Props {
    view0: vec4f,
    view1: vec4f,
    view2: vec4f,
    proj: mat4x4f,
};
@group(0) @binding(0) var<uniform> props: Props;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) color: vec3f,
};

@vertex
fn vs_props(@location(0) p: vec3f, @location(1) color: vec3f) -> VsOut {
    let w = vec4f(p, 1.0);
    let mv = vec3f(dot(props.view0, w), dot(props.view1, w), dot(props.view2, w));
    var out: VsOut;
    out.pos = vec4f(mv, 1.0) * props.proj;
    out.color = color;
    return out;
}

@fragment
fn fs_props(in: VsOut) -> @location(0) vec4f {
    return vec4f(in.color, 1.0);
}
)WGSL";

struct PropVertex {
  float pos[3];
  float color[3];
};
struct PropParams {
  std::array<std::array<float, 4>, 3> view;
  Mat4x4<float> projection;
};

struct PropState {
  wgpu::RenderPipeline pipeline;
  uint32_t samples = 0;
  bool motion = false;
  wgpu::Buffer vertices;
  uint32_t vertexCount = 0;
  wgpu::Buffer params;
  const RelightScene* scene = nullptr;
};
PropState g_props;

float prop_hash(float x) {
  const float s = std::sin(x * 12.9898f) * 43758.5453f;
  return s - std::floor(s);
}

using V3 = std::array<float, 3>;
V3 v_add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 v_sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 v_mul(V3 a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float v_dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 v_cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 v_norm(V3 a) {
  const float l = std::sqrt(v_dot(a, a));
  return l > 0.f ? v_mul(a, 1.f / l) : a;
}

void push_tri(std::vector<PropVertex>& out, V3 a, V3 b, V3 c, V3 ca, V3 cb, V3 cc) {
  out.push_back({{a[0], a[1], a[2]}, {ca[0], ca[1], ca[2]}});
  out.push_back({{b[0], b[1], b[2]}, {cb[0], cb[1], cb[2]}});
  out.push_back({{c[0], c[1], c[2]}, {cc[0], cc[1], cc[2]}});
}

// A log from `a` to `b`: bark round the side (a little uneven), cut ends with light wood and a darker rim.
void push_log(std::vector<PropVertex>& out, V3 a, V3 b, float radius, float seed) {
  constexpr int kSides = 10;
  const V3 axis = v_norm(v_sub(b, a));
  const V3 t1 = v_norm(v_cross(std::abs(axis[1]) < 0.9f ? V3{0.f, 1.f, 0.f} : V3{1.f, 0.f, 0.f}, axis));
  const V3 t2 = v_cross(axis, t1);
  const V3 wood{0.66f, 0.47f, 0.26f}, rim{0.36f, 0.22f, 0.11f};
  std::array<V3, kSides> ra, rb;
  std::array<V3, kSides> bark;
  for (int i = 0; i < kSides; ++i) {
    const float ang = 6.2831853f * i / kSides;
    const float r = radius * (0.9f + 0.2f * prop_hash(seed * 7.f + i));
    const V3 off = v_add(v_mul(t1, std::cos(ang) * r), v_mul(t2, std::sin(ang) * r));
    ra[i] = v_add(a, off);
    rb[i] = v_add(b, off);
    const float k = 0.8f + 0.35f * prop_hash(seed * 3.f + i * 1.7f);
    bark[i] = {0.32f * k, 0.19f * k, 0.1f * k};
  }
  for (int i = 0; i < kSides; ++i) {
    const int j = (i + 1) % kSides;
    push_tri(out, ra[i], ra[j], rb[j], bark[i], bark[j], bark[j]);
    push_tri(out, ra[i], rb[j], rb[i], bark[i], bark[j], bark[i]);
    push_tri(out, a, ra[j], ra[i], wood, rim, rim);
    push_tri(out, b, rb[i], rb[j], wood, rim, rim);
  }
}

// A rounded rock: a subdivided octahedron, lumpy, squashed, sitting partly in the ground.
void push_rock(std::vector<PropVertex>& out, V3 centre, float size, float seed) {
  std::vector<std::array<V3, 3>> tris{
      {V3{1, 0, 0}, V3{0, 1, 0}, V3{0, 0, 1}},    {V3{0, 0, 1}, V3{0, 1, 0}, V3{-1, 0, 0}},
      {V3{-1, 0, 0}, V3{0, 1, 0}, V3{0, 0, -1}},  {V3{0, 0, -1}, V3{0, 1, 0}, V3{1, 0, 0}},
      {V3{1, 0, 0}, V3{0, 0, 1}, V3{0, -1, 0}},   {V3{0, 0, 1}, V3{-1, 0, 0}, V3{0, -1, 0}},
      {V3{-1, 0, 0}, V3{0, 0, -1}, V3{0, -1, 0}}, {V3{0, 0, -1}, V3{1, 0, 0}, V3{0, -1, 0}},
  };
  for (int level = 0; level < 2; ++level) {
    std::vector<std::array<V3, 3>> next;
    for (const auto& t : tris) {
      const V3 ab = v_norm(v_add(t[0], t[1])), bc = v_norm(v_add(t[1], t[2])), ca = v_norm(v_add(t[2], t[0]));
      next.push_back({t[0], ab, ca});
      next.push_back({ab, t[1], bc});
      next.push_back({ca, bc, t[2]});
      next.push_back({ab, bc, ca});
    }
    tris = std::move(next);
  }
  const float tone = 0.2f + 0.07f * prop_hash(seed * 5.f);
  const float yaw = prop_hash(seed * 11.f) * 6.2831853f;
  const auto place = [&](V3 n, V3& colour) {
    const float bump = 1.f + 0.16f * (prop_hash(n[0] * 3.1f + n[1] * 5.7f + n[2] * 2.3f + seed) - 0.5f) +
                       0.1f * std::sin(n[0] * 4.f + seed) * std::cos(n[2] * 3.f);
    V3 p{n[0] * 1.15f * bump, n[1] * 0.68f * bump, n[2] * 0.95f * bump};
    p = {p[0] * std::cos(yaw) - p[2] * std::sin(yaw), p[1], p[0] * std::sin(yaw) + p[2] * std::cos(yaw)};
    const float shade = 0.85f + 0.25f * (n[1] * 0.5f + 0.5f);
    colour = {tone * shade, tone * shade * 0.95f, tone * shade * 0.88f};
    return v_add(centre, v_mul(p, size));
  };
  for (const auto& t : tris) {
    V3 c0, c1, c2;
    const V3 p0 = place(t[0], c0), p1 = place(t[1], c1), p2 = place(t[2], c2);
    push_tri(out, p0, p1, p2, c0, c1, c2);
  }
}

std::vector<PropVertex> campfire_mesh(const RelightScene& scene) {
  std::vector<PropVertex> out;
  const V3 base{scene.firePos[0], scene.firePos[1], scene.firePos[2]};
  const float s = scene.fireSize;
  // A teepee of five logs leaning in, their tops crossing just past the middle.
  constexpr int kLogs = 5;
  for (int i = 0; i < kLogs; ++i) {
    const float ang = 6.2831853f * i / kLogs + 0.3f;
    const V3 dir{std::cos(ang), 0.f, std::sin(ang)};
    const V3 foot = v_add(base, v_add(v_mul(dir, 0.68f * s), V3{0.f, 0.05f * s, 0.f}));
    const V3 top = v_add(base, v_add(v_mul(dir, -0.16f * s), V3{0.f, 1.05f * s, 0.f}));
    push_log(out, foot, top, 0.15f * s, static_cast<float>(i) + 1.f);
  }
  // Two short logs lying at the foot.
  push_log(out, v_add(base, V3{-0.85f * s, 0.12f * s, 0.4f * s}), v_add(base, V3{-0.05f * s, 0.12f * s, 0.9f * s}),
           0.13f * s, 9.f);
  push_log(out, v_add(base, V3{0.62f * s, 0.12f * s, -0.72f * s}), v_add(base, V3{0.95f * s, 0.12f * s, 0.18f * s}),
           0.125f * s, 13.f);
  // A ring of rocks.
  constexpr int kRocks = 11;
  for (int i = 0; i < kRocks; ++i) {
    const float ang = 6.2831853f * i / kRocks + 0.15f * prop_hash(i * 2.3f);
    const float size = (0.27f + 0.1f * prop_hash(i * 4.1f)) * s;
    const float ring = 1.3f * s + 0.06f * s * prop_hash(i * 6.7f);
    push_rock(out, v_add(base, V3{std::cos(ang) * ring, size * 0.25f, std::sin(ang) * ring}), size,
              static_cast<float>(i) * 1.37f + 0.5f);
  }
  return out;
}


// ---- Reimagined lamps: 1960s mercury-vapour post-tops ("lamp X GROUND_Y Z"). A fluted cast pole on a plinth, a
// fitter, a ribbed acorn globe and a cap with a finial, 4 units tall. The metal is drawn with the props (lit by the
// stage); the globe glows: drawn after the relighting, unlit, and again into the bloom's light target.
constexpr float kLampGlobeBottom = 3.18f;
constexpr float kLampGlobeTop = 3.92f;

float lamp_globe_radius(float t) {  // t 0..1 up the globe: narrow at the fitter, widest 40% up, a point at the top
  return 0.21f * std::pow(std::max(std::sin(3.14159265f * (0.18f + 0.8f * t)), 0.f), 0.7f);
}

// A surface of revolution round `base`'s vertical: `profile` (height, radius) rings from bottom to top, `sides` around;
// `flute` > 0 alternates the radius (fluting). Flat-coloured (the stage lights it).
void push_lathe(std::vector<PropVertex>& out, V3 base, const std::vector<std::array<float, 2>>& profile, int sides,
                V3 colour, float flute = 0.f) {
  const auto at = [&](size_t ring, int side) {
    const float a = 6.2831853f * side / sides;
    const float r = profile[ring][1] * (flute > 0.f && (side & 1) ? 1.f - flute : 1.f);
    return v_add(base, V3{std::cos(a) * r, profile[ring][0], std::sin(a) * r});
  };
  for (size_t ring = 0; ring + 1 < profile.size(); ++ring) {
    for (int side = 0; side < sides; ++side) {
      const int next = (side + 1) % sides;
      const V3 a = at(ring, side), b = at(ring, next), c = at(ring + 1, next), d = at(ring + 1, side);
      push_tri(out, a, b, c, colour, colour, colour);
      push_tri(out, a, c, d, colour, colour, colour);
    }
  }
}

void push_lamp_metal(std::vector<PropVertex>& out, V3 g) {
  const V3 iron{0.07f, 0.1f, 0.09f};    // painted cast iron, dark green-black
  const V3 trim{0.11f, 0.14f, 0.12f};
  // Plinth: an octagonal base stepping in to the pole.
  push_lathe(out, g, {{0.f, 0.17f}, {0.24f, 0.17f}, {0.28f, 0.15f}, {0.36f, 0.15f}, {0.42f, 0.11f}, {0.48f, 0.1f}},
             8, iron);
  // Fluted pole, tapering.
  push_lathe(out, g, {{0.48f, 0.085f}, {3.0f, 0.058f}}, 16, iron, 0.12f);
  // A ring halfway up and the collar at the top.
  push_lathe(out, g, {{1.6f, 0.07f}, {1.64f, 0.085f}, {1.7f, 0.085f}, {1.74f, 0.07f}}, 12, trim);
  push_lathe(out, g, {{2.98f, 0.065f}, {3.02f, 0.09f}, {3.1f, 0.09f}, {kLampGlobeBottom, 0.145f}}, 12, trim);
  // Cap over the globe's top, and the finial.
  const float t = kLampGlobeTop;
  push_lathe(out, g,
             {{t - 0.11f, 0.12f}, {t - 0.08f, 0.14f}, {t - 0.05f, 0.14f}, {t + 0.02f, 0.09f}, {t + 0.07f, 0.035f},
              {t + 0.08f, 0.02f}},
             12, iron);
  push_lathe(out, g, {{t + 0.08f, 0.f}, {t + 0.1f, 0.03f}, {t + 0.13f, 0.035f}, {t + 0.16f, 0.02f}, {t + 0.18f, 0.f}},
             8, trim);
}

struct GlobeVertex {
  float pos[3];
  float normal[3];
  float uv[2];  // round (0..1), up (0..1)
};

void push_lamp_globe(std::vector<GlobeVertex>& out, V3 g) {
  constexpr int kSides = 32;
  constexpr int kRings = 14;
  const float y0 = kLampGlobeBottom - 0.01f, y1 = kLampGlobeTop - 0.02f;
  const auto vertex = [&](int ring, int side) {
    const float t = static_cast<float>(ring) / kRings;
    const float a = 6.2831853f * side / kSides;
    const float r = lamp_globe_radius(t);
    const float dt = 1e-3f;
    const float slope = (lamp_globe_radius(std::min(t + dt, 1.f)) - lamp_globe_radius(std::max(t - dt, 0.f))) /
                        ((std::min(t + dt, 1.f) - std::max(t - dt, 0.f)) * (y1 - y0));
    const V3 n = v_norm(V3{std::cos(a), -slope, std::sin(a)});
    GlobeVertex v{};
    v.pos[0] = g[0] + std::cos(a) * r;
    v.pos[1] = g[1] + y0 + t * (y1 - y0);
    v.pos[2] = g[2] + std::sin(a) * r;
    std::copy(n.begin(), n.end(), v.normal);
    v.uv[0] = static_cast<float>(side) / kSides;
    v.uv[1] = t;
    return v;
  };
  for (int ring = 0; ring < kRings; ++ring) {
    for (int side = 0; side < kSides; ++side) {
      const GlobeVertex a = vertex(ring, side), b = vertex(ring, side + 1), c = vertex(ring + 1, side + 1),
                        d = vertex(ring + 1, side);
      out.insert(out.end(), {a, b, c, a, c, d});
    }
  }
}

constexpr char kGlobeWgsl[] = R"WGSL(
struct Globe {
    view0: vec4f,
    view1: vec4f,
    view2: vec4f,
    proj: mat4x4f,
    core: vec4f,
    rim: vec4f,
};
@group(0) @binding(0) var<uniform> globe: Globe;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) view_pos: vec3f,
    @location(1) normal: vec3f,
    @location(2) uv: vec2f,
};

@vertex
fn vs_globe(@location(0) p: vec3f, @location(1) n: vec3f, @location(2) uv: vec2f) -> VsOut {
    let w = vec4f(p, 1.0);
    let mv = vec3f(dot(globe.view0, w), dot(globe.view1, w), dot(globe.view2, w));
    var out: VsOut;
    out.pos = vec4f(mv, 1.0) * globe.proj;
    out.view_pos = mv;
    out.normal = vec3f(dot(globe.view0.xyz, n), dot(globe.view1.xyz, n), dot(globe.view2.xyz, n));
    out.uv = uv;
    return out;
}

// Frosted ribbed glass lit from inside by the arc tube: hottest where it faces the camera and round the tube's height,
// the cool blue-green of mercury toward the edges, the ribs a little darker between.
@fragment
fn fs_globe(in: VsOut) -> @location(0) vec4f {
    let n = normalize(in.normal);
    let facing = clamp(abs(dot(n, normalize(-in.view_pos))), 0.0, 1.0);
    let rib = 0.84 + 0.16 * abs(cos(in.uv.x * 3.14159265 * 16.0));
    let tube = 1.0 - smoothstep(0.15, 0.45, abs(in.uv.y - 0.42));
    let hot = clamp(pow(facing, 1.6) * (0.55 + 0.45 * tube), 0.0, 1.0);
    let col = mix(globe.rim.rgb, globe.core.rgb, hot) * rib;
    return vec4f(min(col, vec3f(1.0)), 1.0);
}
)WGSL";

struct GlobeParams {
  std::array<std::array<float, 4>, 3> view;
  Mat4x4<float> projection;
  std::array<float, 4> core;
  std::array<float, 4> rim;
};

struct GlobeState {
  wgpu::RenderPipeline pipeline;
  uint32_t samples = 0;
  wgpu::Buffer vertices;
  uint32_t vertexCount = 0;
  wgpu::Buffer params;
  const RelightScene* scene = nullptr;
};
GlobeState g_globes;

void encode_props(const wgpu::RenderPassEncoder& pass, const Camera& camera, wgpu::Extent3D size, uint32_t samples,
                  bool motion) {
  const auto& scene = relight_scene();
  if ((!scene.hasFire && scene.lamps.empty()) || !camera.hasView) return;
  if (g_props.scene != &scene) {
    auto mesh = scene.hasFire ? campfire_mesh(scene) : std::vector<PropVertex>{};
    for (const auto& l : scene.lamps) push_lamp_metal(mesh, V3{l[0], l[1], l[2]});
    const wgpu::BufferDescriptor descriptor{
        .label = "Reimagined props",
        .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
        .size = (mesh.size() * sizeof(PropVertex) + 3) & ~size_t{3},
    };
    g_props.vertices = g_device.CreateBuffer(&descriptor);
    g_queue.WriteBuffer(g_props.vertices, 0, mesh.data(), mesh.size() * sizeof(PropVertex));
    g_props.vertexCount = static_cast<uint32_t>(mesh.size());
    g_props.scene = &scene;
  }
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (!g_props.pipeline || g_props.samples != samples || g_props.motion != motion) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kPropsWgsl;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Reimagined props"};
    const auto module = g_device.CreateShaderModule(&moduleDescriptor);
    const std::array attributes{
        wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
        wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x3, .offset = 12, .shaderLocation = 1},
    };
    const wgpu::VertexBufferLayout layout{
        .arrayStride = sizeof(PropVertex), .attributeCount = attributes.size(), .attributes = attributes.data()};
    const std::array targets{
        wgpu::ColorTargetState{.format = format,
                               .writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
                                            wgpu::ColorWriteMask::Blue},
        wgpu::ColorTargetState{.format = wgpu::TextureFormat::RG16Float, .writeMask = wgpu::ColorWriteMask::None},
        wgpu::ColorTargetState{.format = wgpu::TextureFormat::R32Float, .writeMask = wgpu::ColorWriteMask::None},
        wgpu::ColorTargetState{.format = wgpu::TextureFormat::R8Unorm, .writeMask = wgpu::ColorWriteMask::None},
    };
    const wgpu::FragmentState fragment{.module = module,
                                       .entryPoint = "fs_props",
                                       .targetCount = motion ? targets.size() : 1,
                                       .targets = targets.data()};
    const wgpu::DepthStencilState depthStencil{
        .format = webgpu::g_graphicsConfig.depthFormat,
        .depthWriteEnabled = true,
        .depthCompare = gx::UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "Reimagined props",
        .vertex = {.module = module, .entryPoint = "vs_props", .bufferCount = 1, .buffers = &layout},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
        .depthStencil = &depthStencil,
        .multisample = {.count = samples},
        .fragment = &fragment,
    };
    g_props.pipeline = g_device.CreateRenderPipeline(&descriptor);
    g_props.samples = samples;
    g_props.motion = motion;
  }
  if (!g_props.params) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Reimagined props",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(PropParams),
    };
    g_props.params = g_device.CreateBuffer(&descriptor);
  }
  PropParams params{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) params.view[r][c] = camera.view[r * 4 + c];
  params.projection = camera.projection;
  g_queue.WriteBuffer(g_props.params, 0, &params, sizeof(params));
  const wgpu::BindGroupEntry entry{.binding = 0, .buffer = g_props.params, .size = sizeof(PropParams)};
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_props.pipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &entry};
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const auto& vp = camera.viewport;
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, gx::UseReversedZ ? 1.f - vp.zfar : vp.znear,
                   gx::UseReversedZ ? 1.f - vp.znear : vp.zfar);
  const auto left = static_cast<uint32_t>(std::clamp(vp.left, 0.f, static_cast<float>(size.width)));
  const auto top = static_cast<uint32_t>(std::clamp(vp.top, 0.f, static_cast<float>(size.height)));
  const auto right = static_cast<uint32_t>(std::clamp(vp.left + vp.width, 0.f, static_cast<float>(size.width)));
  const auto bottom = static_cast<uint32_t>(std::clamp(vp.top + vp.height, 0.f, static_cast<float>(size.height)));
  if (right <= left || bottom <= top) return;
  pass.SetScissorRect(left, top, right - left, bottom - top);
  pass.SetPipeline(g_props.pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.SetVertexBuffer(0, g_props.vertices);
  pass.Draw(g_props.vertexCount);
}



// ---- The props into the moon's shadow map (lamp posts and globes, the campfire's logs and rocks): depth only, written
// the way the game's draws are there (the shadow matrix is their projection, the whole map their viewport).
constexpr char kPropShadowWgsl[] = R"WGSL(
struct PropShadow {
    view0: vec4f,
    view1: vec4f,
    view2: vec4f,
    shadow: array<vec4f, 4>,
};
@group(0) @binding(0) var<uniform> ps: PropShadow;

@vertex
fn vs_prop_shadow(@location(0) p: vec3f) -> @builtin(position) vec4f {
    let w = vec4f(p, 1.0);
    let mv = vec4f(dot(ps.view0, w), dot(ps.view1, w), dot(ps.view2, w), 1.0);
    return vec4f(dot(ps.shadow[0], mv), dot(ps.shadow[1], mv), dot(ps.shadow[2], mv), dot(ps.shadow[3], mv));
}
)WGSL";

struct PropShadowParams {
  std::array<std::array<float, 4>, 3> view;
  std::array<std::array<float, 4>, 4> shadow;
};

struct PropShadowState {
  wgpu::RenderPipeline pipeline;
  uint32_t samples = 0;
  wgpu::Buffer vertices;  // positions only
  uint32_t vertexCount = 0;
  std::vector<std::pair<uint32_t, uint32_t>> lampRanges;  // each lamp's vertices (first, count), in lamp order
  std::array<wgpu::Buffer, 1 + kMaxLampShadows> params;  // the moon's, then each lamp map's
  const RelightScene* scene = nullptr;
};
PropShadowState g_propShadow;

void encode_props_shadow(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depth, const Camera& camera,
                         uint32_t samples, int lampSlot) {
  const auto& scene = relight_scene();
  if (!relighting() || (!scene.hasFire && scene.lamps.empty()) || !camera.hasView) return;
  if (lampSlot < 0 ? !camera.hasShadow : static_cast<uint32_t>(lampSlot) >= camera.lampShadowCount) return;
  if (g_propShadow.scene != &scene) {
    auto mesh = scene.hasFire ? campfire_mesh(scene) : std::vector<PropVertex>{};
    g_propShadow.lampRanges.clear();
    for (const auto& l : scene.lamps) {
      const auto first = static_cast<uint32_t>(mesh.size());
      const V3 g{l[0], l[1], l[2]};
      push_lamp_metal(mesh, g);
      std::vector<std::array<float, 2>> globe;
      for (int i = 0; i <= 8; ++i) {
        const float t = i / 8.f;
        globe.push_back({kLampGlobeBottom + t * (kLampGlobeTop - kLampGlobeBottom), lamp_globe_radius(t)});
      }
      push_lathe(mesh, g, globe, 12, V3{});
      g_propShadow.lampRanges.emplace_back(first, static_cast<uint32_t>(mesh.size()) - first);
    }
    std::vector<float> positions;
    positions.reserve(mesh.size() * 3);
    for (const auto& v : mesh) positions.insert(positions.end(), v.pos, v.pos + 3);
    const wgpu::BufferDescriptor descriptor{
        .label = "Reimagined prop shadows",
        .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
        .size = positions.size() * sizeof(float),
    };
    g_propShadow.vertices = g_device.CreateBuffer(&descriptor);
    g_queue.WriteBuffer(g_propShadow.vertices, 0, positions.data(), positions.size() * sizeof(float));
    g_propShadow.vertexCount = static_cast<uint32_t>(mesh.size());
    g_propShadow.scene = &scene;
  }
  if (g_propShadow.vertexCount == 0) return;
  if (!g_propShadow.pipeline || g_propShadow.samples != samples) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kPropShadowWgsl;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Reimagined prop shadows"};
    const auto module = g_device.CreateShaderModule(&moduleDescriptor);
    const wgpu::VertexAttribute attribute{.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0};
    const wgpu::VertexBufferLayout layout{.arrayStride = 12, .attributeCount = 1, .attributes = &attribute};
    const wgpu::DepthStencilState depthStencil{
        .format = webgpu::g_graphicsConfig.depthFormat,
        .depthWriteEnabled = true,
        .depthCompare = gx::UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "Reimagined prop shadows",
        .vertex = {.module = module, .entryPoint = "vs_prop_shadow", .bufferCount = 1, .buffers = &layout},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
        .depthStencil = &depthStencil,
        .multisample = {.count = samples},
    };
    g_propShadow.pipeline = g_device.CreateRenderPipeline(&descriptor);
    g_propShadow.samples = samples;
  }
  auto& buffer = g_propShadow.params[lampSlot + 1];
  if (!buffer) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Reimagined prop shadows",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(PropShadowParams),
    };
    buffer = g_device.CreateBuffer(&descriptor);
  }
  PropShadowParams params{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) params.view[r][c] = camera.view[r * 4 + c];
  const auto& matrix = lampSlot < 0 ? camera.shadow : camera.lampShadow[lampSlot];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) params.shadow[r][c] = matrix[r * 4 + c];
  g_queue.WriteBuffer(buffer, 0, &params, sizeof(params));
  const wgpu::BindGroupEntry entry{.binding = 0, .buffer = buffer, .size = sizeof(PropShadowParams)};
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_propShadow.pipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &entry};
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const wgpu::RenderPassDepthStencilAttachment depthAttachment{
      .view = depth, .depthLoadOp = wgpu::LoadOp::Load, .depthStoreOp = wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Reimagined prop shadows", .colorAttachmentCount = 0, .depthStencilAttachment = &depthAttachment};
  const auto pass = encoder.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(g_propShadow.pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.SetVertexBuffer(0, g_propShadow.vertices);
  // A lamp's own post and globe stay out of its map (its light comes from the whole globe, round its fitter).
  std::pair<uint32_t, uint32_t> own{g_propShadow.vertexCount, 0};
  if (lampSlot >= 0) {
    const auto& light = scene.lights[camera.lampShadowLight[lampSlot]];
    for (size_t i = 0; i < scene.lamps.size() && i < g_propShadow.lampRanges.size(); ++i) {
      if (scene.lamps[i][0] == light.pos[0] && scene.lamps[i][2] == light.pos[2]) own = g_propShadow.lampRanges[i];
    }
  }
  if (own.first > 0) pass.Draw(own.first);
  const uint32_t after = own.first + own.second;
  if (after < g_propShadow.vertexCount) pass.Draw(g_propShadow.vertexCount - after, 1, after);
  pass.End();
}

bool lamps_wanted(const Camera& camera) noexcept {
  return relighting() && !relight_scene().lamps.empty() && camera.hasView;
}

void encode_lamp_glow(const wgpu::RenderPassEncoder& pass, const Camera& camera, wgpu::Extent3D size,
                      uint32_t samples) {
  if (!lamps_wanted(camera)) return;
  const auto& scene = relight_scene();
  if (g_globes.scene != &scene) {
    std::vector<GlobeVertex> mesh;
    for (const auto& l : scene.lamps) push_lamp_globe(mesh, V3{l[0], l[1], l[2]});
    const wgpu::BufferDescriptor descriptor{
        .label = "Reimagined lamp globes",
        .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
        .size = (mesh.size() * sizeof(GlobeVertex) + 3) & ~size_t{3},
    };
    g_globes.vertices = g_device.CreateBuffer(&descriptor);
    g_queue.WriteBuffer(g_globes.vertices, 0, mesh.data(), mesh.size() * sizeof(GlobeVertex));
    g_globes.vertexCount = static_cast<uint32_t>(mesh.size());
    g_globes.scene = &scene;
  }
  if (!g_globes.pipeline || g_globes.samples != samples) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kGlobeWgsl;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Reimagined lamp globes"};
    const auto module = g_device.CreateShaderModule(&moduleDescriptor);
    const std::array attributes{
        wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
        wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x3, .offset = 12, .shaderLocation = 1},
        wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x2, .offset = 24, .shaderLocation = 2},
    };
    const wgpu::VertexBufferLayout layout{
        .arrayStride = sizeof(GlobeVertex), .attributeCount = attributes.size(), .attributes = attributes.data()};
    const wgpu::ColorTargetState target{
        .format = webgpu::g_graphicsConfig.surfaceConfiguration.format,
        .writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue,
    };
    const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_globe", .targetCount = 1, .targets = &target};
    const wgpu::DepthStencilState depthStencil{
        .format = webgpu::g_graphicsConfig.depthFormat,
        .depthWriteEnabled = true,
        .depthCompare = gx::UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "Reimagined lamp globes",
        .vertex = {.module = module, .entryPoint = "vs_globe", .bufferCount = 1, .buffers = &layout},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
        .depthStencil = &depthStencil,
        .multisample = {.count = samples},
        .fragment = &fragment,
    };
    g_globes.pipeline = g_device.CreateRenderPipeline(&descriptor);
    g_globes.samples = samples;
  }
  if (!g_globes.params) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Reimagined lamp globes",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(GlobeParams),
    };
    g_globes.params = g_device.CreateBuffer(&descriptor);
  }
  GlobeParams params{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) params.view[r][c] = camera.view[r * 4 + c];
  params.projection = camera.projection;
  params.core = {0.86f, 1.f, 1.f, 1.f};
  for (int c = 0; c < 3; ++c) params.rim[c] = scene.lampColor[c] * 0.8f;
  g_queue.WriteBuffer(g_globes.params, 0, &params, sizeof(params));
  const wgpu::BindGroupEntry entry{.binding = 0, .buffer = g_globes.params, .size = sizeof(GlobeParams)};
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_globes.pipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &entry};
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const auto& vp = camera.viewport;
  const auto left = static_cast<uint32_t>(std::clamp(vp.left, 0.f, static_cast<float>(size.width)));
  const auto top = static_cast<uint32_t>(std::clamp(vp.top, 0.f, static_cast<float>(size.height)));
  const auto right = static_cast<uint32_t>(std::clamp(vp.left + vp.width, 0.f, static_cast<float>(size.width)));
  const auto bottom = static_cast<uint32_t>(std::clamp(vp.top + vp.height, 0.f, static_cast<float>(size.height)));
  if (right <= left || bottom <= top) return;
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, gx::UseReversedZ ? 1.f - vp.zfar : vp.znear,
                   gx::UseReversedZ ? 1.f - vp.znear : vp.zfar);
  pass.SetScissorRect(left, top, right - left, bottom - top);
  pass.SetPipeline(g_globes.pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.SetVertexBuffer(0, g_globes.vertices);
  pass.Draw(g_globes.vertexCount);
}

bool sky_wanted(const Camera& camera) noexcept {
  return relighting() && (relight_scene().hasSky || relight_scene().hasFire || !relight_scene().lamps.empty()) &&
         camera.hasView;
}

void encode_sky_only(const wgpu::RenderPassEncoder& pass, const Camera& camera, wgpu::Extent3D size,
                     uint32_t samples, bool motion);

void encode_sky(const wgpu::RenderPassEncoder& pass, const Camera& camera, wgpu::Extent3D size, uint32_t samples,
                bool motion) {
  if (!sky_wanted(camera)) return;
  if (relight_scene().hasSky) encode_sky_only(pass, camera, size, samples, motion);
  encode_props(pass, camera, size, samples, motion);
}

void encode_sky_only(const wgpu::RenderPassEncoder& pass, const Camera& camera, wgpu::Extent3D size,
                     uint32_t samples, bool motion) {
  const auto& scene = relight_scene();
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (!g_sky.pipeline || g_sky.samples != samples || g_sky.motion != motion || g_sky.format != format) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kSkyWgsl;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Relight sky"};
    const auto module = g_device.CreateShaderModule(&moduleDescriptor);
    // The EFB pass's targets: the EFB (colour only, never its alpha) and, with motion vectors, three it leaves alone.
    const std::array targets{
        wgpu::ColorTargetState{.format = format,
                               .writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
                                            wgpu::ColorWriteMask::Blue},
        wgpu::ColorTargetState{.format = wgpu::TextureFormat::RG16Float, .writeMask = wgpu::ColorWriteMask::None},
        wgpu::ColorTargetState{.format = wgpu::TextureFormat::R32Float, .writeMask = wgpu::ColorWriteMask::None},
        wgpu::ColorTargetState{.format = wgpu::TextureFormat::R8Unorm, .writeMask = wgpu::ColorWriteMask::None},
    };
    const wgpu::FragmentState fragment{
        .module = module, .entryPoint = "fs_sky", .targetCount = motion ? targets.size() : 1, .targets = targets.data()};
    const wgpu::DepthStencilState depthStencil{
        .format = webgpu::g_graphicsConfig.depthFormat,
        .depthWriteEnabled = false,
        .depthCompare = wgpu::CompareFunction::Always,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "Relight sky",
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .depthStencil = &depthStencil,
        .multisample = {.count = samples},
        .fragment = &fragment,
    };
    g_sky.pipeline = g_device.CreateRenderPipeline(&descriptor);
    g_sky.samples = samples;
    g_sky.motion = motion;
    g_sky.format = format;
  }
  if (!g_skyParams) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Relight sky",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(SkyParams),
    };
    g_skyParams = g_device.CreateBuffer(&descriptor);
  }
  // View direction to world: R^T (the view matrix's rotation, rows).
  const auto& v = camera.view;
  SkyParams p{};
  for (int i = 0; i < 3; ++i) p.v[i] = {v[0 + i], v[4 + i], v[8 + i], 0.f};
  const auto& P = camera.projection;
  p.v[3] = {1.f / P.m0.m[0], 1.f / P.m1.m[1], P.m0.m[2], P.m1.m[2]};
  const auto& vp = camera.viewport;
  p.v[4] = {vp.left, vp.top, vp.width, vp.height};
  const bool moonPicture = camera.moonTexture && camera.moonTexture->sampleTextureView;
  p.v[5] = {scene.skyZenith[0], scene.skyZenith[1], scene.skyZenith[2], moonPicture ? 1.f : 0.f};
  p.v[6] = {scene.skyHorizon[0], scene.skyHorizon[1], scene.skyHorizon[2], 0.f};
  const float radius = scene.hasMoon ? scene.moonRadiusDeg * 3.14159265f / 180.f : 0.f;
  p.v[7] = {scene.moonDir[0], scene.moonDir[1], scene.moonDir[2], radius};
  p.v[8] = {scene.moonDisc[0], scene.moonDisc[1], scene.moonDisc[2], scene.starDensity};
  constexpr float kDeg = 3.14159265f / 180.f;
  p.v[9] = {scene.mountainLow * kDeg, scene.mountainHigh * kDeg, scene.hasMountains ? 1.f : 0.f, 3.3f};
  p.v[10] = {scene.mountainColor[0], scene.mountainColor[1], scene.mountainColor[2], 0.f};
  g_queue.WriteBuffer(g_skyParams, 0, &p, sizeof(p));
  if (!g_skySampler) {
    const wgpu::SamplerDescriptor samplerDescriptor{
        .label = "Relight sky moon",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .mipmapFilter = wgpu::MipmapFilterMode::Linear,
    };
    g_skySampler = g_device.CreateSampler(&samplerDescriptor);
    const wgpu::TextureDescriptor textureDescriptor{
        .label = "Relight sky no moon",
        .usage = wgpu::TextureUsage::TextureBinding,
        .size = {1, 1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    g_skyNoMoon = g_device.CreateTexture(&textureDescriptor);
    g_skyNoMoonView = g_skyNoMoon.CreateView();
  }
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = g_skyParams, .size = sizeof(SkyParams)},
      wgpu::BindGroupEntry{.binding = 1,
                           .textureView = moonPicture ? camera.moonTexture->sampleTextureView : g_skyNoMoonView},
      wgpu::BindGroupEntry{.binding = 2, .sampler = g_skySampler},
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_sky.pipeline.GetBindGroupLayout(0), .entryCount = entries.size(), .entries = entries.data()};
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  pass.SetViewport(0.f, 0.f, static_cast<float>(size.width), static_cast<float>(size.height), 0.f, 1.f);
  pass.SetPipeline(g_sky.pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.Draw(3);
}

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
const LampShadowTargets& lamp_shadow_targets(uint32_t samples, bool motion) {
  const uint32_t size = relight_scene().lampShadowSize;
  if (g_lampShadowSamples != samples || g_lampShadowTargets.size != size) {
    g_lampShadowTextures = {};
    g_lampShadowTargets = {};
    g_lampShadowSamples = samples;
    g_lampShadowTargets.size = size;
  }
  const auto make = [&](size_t i, wgpu::TextureView& view, wgpu::TextureFormat format, bool sampled) {
    if (g_lampShadowTextures[i]) return;
    const wgpu::TextureDescriptor descriptor{
        .label = "Lamp shadow map",
        .usage = wgpu::TextureUsage::RenderAttachment |
                 (sampled ? wgpu::TextureUsage::TextureBinding : wgpu::TextureUsage::None),
        .size = {size, size, 1},
        .format = format,
        .sampleCount = samples,
    };
    g_lampShadowTextures[i] = g_device.CreateTexture(&descriptor);
    view = g_lampShadowTextures[i].CreateView();
  };
  for (uint32_t k = 0; k < kMaxLampShadows; ++k)
    make(k, g_lampShadowTargets.depth[k], webgpu::g_graphicsConfig.depthFormat, true);
  make(kMaxLampShadows, g_lampShadowTargets.color, webgpu::g_graphicsConfig.surfaceConfiguration.format, false);
  if (motion) {
    make(kMaxLampShadows + 1, g_lampShadowTargets.motion, wgpu::TextureFormat::RG16Float, false);
    make(kMaxLampShadows + 2, g_lampShadowTargets.motionDepth, wgpu::TextureFormat::R32Float, false);
    make(kMaxLampShadows + 3, g_lampShadowTargets.mark, wgpu::TextureFormat::R8Unorm, false);
  }
  return g_lampShadowTargets;
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
  // The lamps nearest the view get shadow maps (lights flagged as shadowing, within the moon's shadow radius or 40).
  g_camera.lampShadowCount = 0;
  if (g_camera.hasView && relighting()) {
    const auto& v = g_camera.view;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(v[0 + i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    const float reach = scene.shadowRadius > 0.f ? scene.shadowRadius : 40.f;
    std::vector<std::pair<float, uint32_t>> nearby;
    for (uint32_t i = 0; i < scene.lights.size(); ++i) {
      if (!scene.lights[i].shadow) continue;
      const float dx = scene.lights[i].pos[0] - eye[0], dz = scene.lights[i].pos[2] - eye[2];
      const float d2 = dx * dx + dz * dz;
      if (d2 <= reach * reach) nearby.emplace_back(d2, i);
    }
    std::sort(nearby.begin(), nearby.end());
    const uint32_t count = std::min<uint32_t>(lamp_shadow_slots(), nearby.size());
    for (uint32_t k = 0; k < count; ++k) {
      g_camera.lampShadowLight[k] = nearby[k].second;
      g_camera.lampShadow[k] = lamp_shadow_matrix(v, scene.lights[nearby[k].second].pos);
    }
    g_camera.lampShadowCount = count;
  }
  // The moon's picture, looked up once per name (and again after a reload).
  static std::string s_moonName;
  static const RelightScene* s_moonScene = nullptr;
  static TextureHandle s_moon;
  static uint32_t s_moonSources = UINT32_MAX;
  if (relighting() && scene.hasSky &&
      (&scene != s_moonScene || scene.moonTexture != s_moonName ||
       (!s_moon && s_moonSources != texture_replacement::texture_source_generation()))) {
    s_moonScene = &scene;
    s_moonName = scene.moonTexture;
    s_moonSources = texture_replacement::texture_source_generation();
    s_moon = {};
    if (!s_moonName.empty()) {
      if (auto found = texture_replacement::find_replacement_by_name(s_moonName); found.has_value()) s_moon = *found;
      Log.info("Reimagined: moon texture {} {}", s_moonName, s_moon ? "found" : "not found (drawn disc instead)");
    }
  }
  g_camera.moonTexture = relighting() && scene.hasSky ? s_moon : TextureHandle{};
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
    rp.extra = {scene.moonShadowStrength, 0.f, 0.f, 0.f};
    lamp_shadow_targets(samples, false);  // bound even when the frame has none
    for (uint32_t k = 0; k < camera.lampShadowCount; ++k)
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) rp.lampShadow[k * 4 + r][c] = camera.lampShadow[k][r * 4 + c];
    rp.lampInfo = {static_cast<float>(scene.lampShadowSize), kLampShadowNear, scene.lampShadowBias,
                   kLampShadowTanHalf};
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
      rp.lights[i * 2 + 1][3] = 0.f;
      for (uint32_t k = 0; k < camera.lampShadowCount; ++k)
        if (camera.lampShadowLight[k] == i) rp.lights[i * 2 + 1][3] = static_cast<float>(k + 1);
    }
    if (scene.hasFire) {
      static const auto start = std::chrono::steady_clock::now();
      const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
      const float flicker = 1.f + 0.12f * std::sin(t * 13.f) + 0.08f * std::sin(t * 7.3f + 1.f) +
                            0.06f * std::sin(t * 23.f + 2.f);
      toView(scene.firePos, 1.f, rp.firePos.data());
      rp.firePos[3] = 1.f;
      rp.fireInfo = {scene.fireSize, 0.f, t, flicker * scene.fireIntensity};
      const float up[3] = {0.f, 1.f, 0.f}, east[3] = {1.f, 0.f, 0.f};
      toView(up, 0.f, rp.fireUp.data());
      toView(east, 0.f, rp.fireEast.data());
      const size_t i = std::min(scene.lights.size(), kMaxRelightLights - 1);
      const float lightPos[3] = {scene.firePos[0], scene.firePos[1] + 0.6f * scene.fireSize / 0.6f,
                                 scene.firePos[2]};
      toView(lightPos, 1.f, rp.lights[i * 2].data());
      rp.lights[i * 2][3] = 8.f * scene.radiusScale;
      const float fireColor[3] = {1.f, 0.5f, 0.16f};
      for (int c = 0; c < 3; ++c) rp.lights[i * 2 + 1][c] = fireColor[c] * 1.6f * flicker * scene.fireIntensity;
      rp.info[0] = static_cast<float>(i + 1);
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
    if (scene.hasFire) {
      draw(encoder, Kind::Fire, color, size, &depth, nullptr);
      if (scene.fireMask) draw_fire_depth(encoder, depth, size);
    }
    if (lamps_wanted(camera)) {
      const wgpu::RenderPassColorAttachment attachment{
          .view = color, .loadOp = wgpu::LoadOp::Load, .storeOp = wgpu::StoreOp::Store};
      const wgpu::RenderPassDepthStencilAttachment depthAttachment{
          .view = depth, .depthLoadOp = wgpu::LoadOp::Load, .depthStoreOp = wgpu::StoreOp::Store};
      const wgpu::RenderPassDescriptor passDescriptor{
          .label = "Reimagined lamp globes",
          .colorAttachmentCount = 1,
          .colorAttachments = &attachment,
          .depthStencilAttachment = &depthAttachment,
      };
      const auto pass = encoder.BeginRenderPass(&passDescriptor);
      encode_lamp_glow(pass, camera, size, samples);
      pass.End();
    }
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
  g_sky = {};
  g_skyParams = {};
  g_props = {};
  g_globes = {};
  g_propShadow = {};
  g_skySampler = {};
  g_skyNoMoon = {};
  g_skyNoMoonView = {};
  g_shadowTextures = {};
  g_shadowTargets = {};
  g_shadowSamples = 0;
  g_lampShadowTextures = {};
  g_lampShadowTargets = {};
  g_lampShadowSamples = 0;
  g_aoTargets = {};
}
} // namespace aurora::gfx::scene_stage
