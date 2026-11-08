#include "scene_stage.hpp"

#include "bloom.hpp"
#include "../gx/gx.hpp"
#include "../internal.hpp"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace aurora::gfx::scene_stage {
namespace {
using webgpu::g_device;
using webgpu::g_queue;

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

enum class Kind { Occlusion, BlurH, BlurV, Apply, AoView, Tint, Depth, Ink, InkView, Count };

struct Pipelines {
  uint32_t samples = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  std::array<wgpu::RenderPipeline, static_cast<size_t>(Kind::Count)> pipelines;
};
Pipelines g_pipelines;
wgpu::Buffer g_params;
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
  const auto make = [&](const char* entry, bool efb, bool multiplies) {
    // Over the EFB: its format and sample count, and never its alpha (the game blends with it).
    const wgpu::ColorTargetState target{
        .format = efb ? format : kAoFormat,
        .blend = multiplies ? &multiply : nullptr,
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

bool wanted() noexcept {
  return g_viewMode.load(std::memory_order_relaxed) != 0 || g_aoStrength.load(std::memory_order_relaxed) > 0.f ||
         g_inkLevel.load(std::memory_order_relaxed) != 0 || bloom::wanted();
}

void set_ink_outlines(int level) noexcept {
  g_inkLevel.store(level >= 0 && level <= 2 ? level : 0, std::memory_order_relaxed);
}
int ink_outlines() noexcept { return g_inkLevel.load(std::memory_order_relaxed); }

void begin_frame() noexcept { g_solidSurfaces.clear(); }
void note_solid_surface(HashType surface) noexcept { g_solidSurfaces.insert(surface); }
bool is_solid_surface(HashType surface) noexcept { return g_solidSurfaces.contains(surface); }

void set_ambient_occlusion(float strength) noexcept {
  g_aoStrength.store(std::isfinite(strength) ? std::clamp(strength, 0.f, 1.f) : 0.f, std::memory_order_relaxed);
}
float ambient_occlusion() noexcept { return g_aoStrength.load(std::memory_order_relaxed); }

void set_view(int mode) noexcept { g_viewMode.store(mode >= 0 && mode <= 4 ? mode : 0, std::memory_order_relaxed); }
int view() noexcept { return g_viewMode.load(std::memory_order_relaxed); }

void note_camera(const Mat4x4<float>& projection, const webgpu::Viewport& viewport) noexcept {
  g_camera.projection = projection;
  g_camera.viewport = viewport;
}
const Camera& camera() noexcept { return g_camera; }

void encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, const wgpu::TextureView& depth,
            wgpu::Extent3D size, uint32_t samples, const Camera& camera) {
  const int mode = view();
  const float strength = ambient_occlusion();
  const int ink = ink_outlines();
  if ((mode == 0 && strength <= 0.f && ink == 0) || !color || !depth || size.width == 0 || size.height == 0) {
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
  g_aoTargets = {};
}
} // namespace aurora::gfx::scene_stage
