#include "bloom.hpp"

#include "../internal.hpp"
#include "tracy/Tracy.hpp"

#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aurora::gfx::bloom {
namespace {
using webgpu::g_device;
using webgpu::g_queue;

// ----- Tuning
// The light draws, alone over black, glow from this lightness (0 to 1) and fully from this one. Lightness is the
// brightest channel less kGrey of the dimmest: the game's lights are coloured, so their dark parts (a window's frame
// and bars) and grey parts glow little (the cyan lamps' texture also holds their grey pole, which the game draws
// distant lamps' poles with, lit pale blue). Near white counts from kWhite up (the moon, the sparkles).
constexpr float kSourceLow = 0.08f;
constexpr float kSourceHigh = 0.22f;
constexpr float kGrey = 1.f;
constexpr float kWhite = 0.6f;
// How strongly the glow is added at full strength; a wide glow (a big window close up) is eased toward 1 / kSoften so
// it does not wash out, and on a light itself only this share of it is added (it glows around, and keeps its own
// texture).
constexpr float kIntensity = 3.6f;
constexpr float kSoften = 1.2f;
constexpr float kOnLight = 0.3f;
// The blur's sizes: half the picture, then halving; the last is 1/64 of the picture's height, so the glow's reach
// follows the picture, not its resolution.
constexpr size_t kLevels = 6;

std::atomic<float> g_strength{0.f};
std::atomic_int g_viewMode{0};
// The game's light textures; the version changes with the list, so each texture looks itself up again once.
std::mutex g_lightTexturesMutex;
absl::flat_hash_set<std::string> g_lightTextures;
std::atomic<uint32_t> g_lightListVersion{1};

struct Params {
  std::array<float, 4> src;   // the sampled texture's size, 1 / size
  std::array<float, 4> dst;   // the target's size, 1 / size
  std::array<float, 4> efb;   // EFB width, height, unused
  std::array<float, 4> tune;  // source low, source high, glow scale, softening
  std::array<float, 4> vp;    // the game's viewport in EFB pixels: left, top, right, bottom
  std::array<float, 4> tune2; // share on a light, 1 = the view shows the source as it is, grey, white
};
static_assert(sizeof(Params) == 96);

constexpr const char* kBloomWgsl = R"""(
struct Params {
    src: vec4f,
    dst: vec4f,
    efb: vec4f,
    tune: vec4f,
    vp: vec4f,
    tune2: vec4f,
};
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var tail_tex: texture_2d<f32>;
@group(0) @binding(2) var src_tex: texture_2d<f32>;
@group(0) @binding(3) var src_smp: sampler;

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
    return vec4f(p[i], 0.0, 1.0);
}

// The light draws alone at EFB pixel `p`, their dark and grey parts faded out.
fn light_at(p: vec2i) -> vec3f {
    let c = textureLoad(tail_tex, clamp(p, vec2i(0), vec2i(params.efb.xy) - 1), 0).rgb;
    let hi = max(c.r, max(c.g, c.b));
    let lightness = max(hi - params.tune2.z * min(c.r, min(c.g, c.b)), hi - params.tune2.w);
    return c * smoothstep(params.tune.x, params.tune.y, lightness);
}

// The glow's source at half size (the 2x2 EFB pixels under half-size pixel `p`), inside the game's picture.
@fragment
fn fs_source(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let p = vec2i(pos.xy);
    let at = vec2f(p * 2) + 1.0;
    if (any(at < params.vp.xy) || any(at > params.vp.zw)) {
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }
    let q = p * 2;
    let sum = light_at(q) + light_at(q + vec2i(1, 0)) + light_at(q + vec2i(0, 1)) + light_at(q + vec2i(1, 1));
    return vec4f(sum * 0.25, 1.0);
}

fn tap(uv: vec2f, offset: vec2f) -> vec3f {
    return textureSampleLevel(src_tex, src_smp, uv + offset * params.src.zw, 0.0).rgb;
}

fn luma(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

// Halving: 13 taps in five overlapping boxes (the centre one counts half), which keeps it smooth and steady as things
// move. The first halving weights each box by 1 / (1 + its brightness), so a single bright pixel (a spark) does not
// flicker as a large blob.
fn down(pos: vec4f, steady: bool) -> vec4f {
    let uv = pos.xy * params.dst.zw;
    let a = tap(uv, vec2f(-2.0, -2.0));
    let b = tap(uv, vec2f(0.0, -2.0));
    let c = tap(uv, vec2f(2.0, -2.0));
    let d = tap(uv, vec2f(-1.0, -1.0));
    let e = tap(uv, vec2f(1.0, -1.0));
    let f = tap(uv, vec2f(-2.0, 0.0));
    let g = tap(uv, vec2f(0.0, 0.0));
    let h = tap(uv, vec2f(2.0, 0.0));
    let i = tap(uv, vec2f(-1.0, 1.0));
    let j = tap(uv, vec2f(1.0, 1.0));
    let k = tap(uv, vec2f(-2.0, 2.0));
    let l = tap(uv, vec2f(0.0, 2.0));
    let m = tap(uv, vec2f(2.0, 2.0));
    var boxes = array((d + e + i + j) * 0.25, (a + b + g + f) * 0.25, (b + c + h + g) * 0.25, (f + g + l + k) * 0.25,
                      (g + h + m + l) * 0.25);
    var shares = array(0.5, 0.125, 0.125, 0.125, 0.125);
    var sum = vec3f(0.0);
    var total = 0.0;
    for (var n = 0; n < 5; n++) {
        let w = shares[n] * select(1.0, 1.0 / (1.0 + luma(boxes[n])), steady);
        sum += boxes[n] * w;
        total += w;
    }
    return vec4f(sum / total, 1.0);
}

@fragment
fn fs_down_first(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return down(pos, true);
}

@fragment
fn fs_down(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return down(pos, false);
}

// Doubling: a 3x3 tent over the smaller size, added to the larger one.
@fragment
fn fs_up(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let uv = pos.xy * params.dst.zw;
    var sum = tap(uv, vec2f(0.0, 0.0)) * 4.0;
    sum += (tap(uv, vec2f(-1.0, 0.0)) + tap(uv, vec2f(1.0, 0.0)) + tap(uv, vec2f(0.0, -1.0)) + tap(uv, vec2f(0.0, 1.0))) * 2.0;
    sum += tap(uv, vec2f(-1.0, -1.0)) + tap(uv, vec2f(1.0, -1.0)) + tap(uv, vec2f(-1.0, 1.0)) + tap(uv, vec2f(1.0, 1.0));
    return vec4f(sum / 16.0, 1.0);
}

// Over the EFB: the glow (half size, filtered up), scaled, a wide one eased, less of it on the lights themselves.
@fragment
fn fs_composite(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let uv = pos.xy * 0.5 * params.src.zw;
    let glow = textureSampleLevel(src_tex, src_smp, uv, 0.0).rgb * params.tune.z;
    let eased = glow / (1.0 + params.tune.w * max(glow.r, max(glow.g, glow.b)));
    let light = light_at(vec2i(pos.xy));
    let on_light = smoothstep(0.0, 0.5, max(light.r, max(light.g, light.b)));
    return vec4f(eased * mix(1.0, params.tune2.x, on_light), 1.0);
}

// The views: the glow as it is added (1), or the source (2, as it is).
@fragment
fn fs_view(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let uv = pos.xy * 0.5 * params.src.zw;
    let glow = textureSampleLevel(src_tex, src_smp, uv, 0.0).rgb * params.tune.z;
    return vec4f(select(glow / (1.0 + params.tune.w * max(glow.r, max(glow.g, glow.b))), glow, params.tune2.y > 0.5), 1.0);
}
)""";

enum class Kind { Source, DownFirst, Down, Up, Composite, View, Count };

struct Pipelines {
  uint32_t samples = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  std::array<wgpu::RenderPipeline, static_cast<size_t>(Kind::Count)> pipelines;
};
Pipelines g_pipelines;
wgpu::Sampler g_sampler;
// The light draws alone (EFB format; with MSAA its samples, and the resolve), the motion targets' stand-ins, and the
// glow at its sizes (half, then halving).
webgpu::TextureWithSampler g_tail;
webgpu::TextureWithSampler g_tailResolved;
std::array<webgpu::TextureWithSampler, 3> g_motionScratch;
// The EFB's depth at the scene stage.
webgpu::TextureWithSampler g_stageDepth;
std::array<webgpu::TextureWithSampler, kLevels> g_levels;
// Whether encode made the glow for this frame's view (encode_view shows it once).
bool g_viewReady = false;
// One parameters buffer per draw: source, the halvings, the doublings, composite.
constexpr size_t kDraws = 2 + 2 * (kLevels - 1);
std::array<wgpu::Buffer, kDraws> g_params;

constexpr wgpu::TextureFormat kGlowFormat = wgpu::TextureFormat::RGBA16Float;

void build_pipelines(wgpu::TextureFormat format, uint32_t samples) {
  if (g_pipelines.samples == samples && g_pipelines.format == format) {
    return;
  }
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = kBloomWgsl;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Bloom"};
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const wgpu::BlendState add{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::One},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  const auto make = [&](const char* entry, bool efb, bool adds) {
    // Over the EFB: its format and sample count, and never its alpha (the game blends with it).
    const wgpu::ColorTargetState target{
        .format = efb ? format : kGlowFormat,
        .blend = adds ? &add : nullptr,
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
  p[static_cast<size_t>(Kind::Source)] = make("fs_source", false, false);
  p[static_cast<size_t>(Kind::DownFirst)] = make("fs_down_first", false, false);
  p[static_cast<size_t>(Kind::Down)] = make("fs_down", false, false);
  p[static_cast<size_t>(Kind::Up)] = make("fs_up", false, true);
  p[static_cast<size_t>(Kind::Composite)] = make("fs_composite", true, true);
  p[static_cast<size_t>(Kind::View)] = make("fs_view", true, false);
  g_pipelines.samples = samples;
  g_pipelines.format = format;
  if (!g_sampler) {
    const wgpu::SamplerDescriptor sampler{
        .label = "Bloom",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
    };
    g_sampler = g_device.CreateSampler(&sampler);
  }
  for (auto& buffer : g_params) {
    if (buffer)
      continue;
    const wgpu::BufferDescriptor descriptor{
        .label = "Bloom parameters",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(Params),
    };
    buffer = g_device.CreateBuffer(&descriptor);
  }
}

wgpu::Extent3D half_of(wgpu::Extent3D size) {
  return {std::max(1u, (size.width + 1) / 2), std::max(1u, (size.height + 1) / 2), 1};
}

void prepare(webgpu::TextureWithSampler& target, wgpu::Extent3D size, const char* label,
             wgpu::TextureFormat format = kGlowFormat, uint32_t samples = 1) {
  if (target.texture && target.size.width == size.width && target.size.height == size.height &&
      target.format == format && target.texture.GetSampleCount() == samples) {
    return;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = samples > 1 ? wgpu::TextureUsage::RenderAttachment
                           : wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .size = size,
      .format = format,
      .sampleCount = samples,
  };
  target.texture = g_device.CreateTexture(&descriptor);
  target.view = target.texture.CreateView();
  target.size = size;
  target.format = format;
}

std::array<float, 4> size_of(wgpu::Extent3D size) {
  const float w = static_cast<float>(size.width);
  const float h = static_cast<float>(size.height);
  return {w, h, 1.f / w, 1.f / h};
}

struct Inputs {
  const wgpu::TextureView* tail = nullptr;
  const wgpu::TextureView* src = nullptr;
};

// One full-screen triangle with `kind`'s pipeline into `target` (`size`; `load` keeps what is there), with draw
// `slot`'s parameters. Only the bindings the pass reads are bound (the layouts are derived from what each entry point
// uses).
void draw(const wgpu::CommandEncoder& encoder, Kind kind, size_t slot, const Params& params,
          const wgpu::TextureView& target, wgpu::Extent3D size, bool load, const Inputs& inputs,
          const std::array<uint32_t, 4>* scissor = nullptr) {
  const auto& pipeline = g_pipelines.pipelines[static_cast<size_t>(kind)];
  g_queue.WriteBuffer(g_params[slot], 0, &params, sizeof(params));
  std::vector<wgpu::BindGroupEntry> entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = g_params[slot], .size = sizeof(Params)},
  };
  if (inputs.tail != nullptr)
    entries.push_back(wgpu::BindGroupEntry{.binding = 1, .textureView = *inputs.tail});
  if (inputs.src != nullptr) {
    entries.push_back(wgpu::BindGroupEntry{.binding = 2, .textureView = *inputs.src});
    entries.push_back(wgpu::BindGroupEntry{.binding = 3, .sampler = g_sampler});
  }
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = pipeline.GetBindGroupLayout(0),
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const wgpu::RenderPassColorAttachment attachment{
      .view = target,
      .loadOp = load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 1.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Bloom",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  const auto pass = encoder.BeginRenderPass(&passDescriptor);
  pass.SetViewport(0.f, 0.f, static_cast<float>(size.width), static_cast<float>(size.height), 0.f, 1.f);
  if (scissor != nullptr)
    pass.SetScissorRect((*scissor)[0], (*scissor)[1], (*scissor)[2], (*scissor)[3]);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.Draw(3);
  pass.End();
}

Params base_params(wgpu::Extent3D size) {
  return Params{
      .efb = {static_cast<float>(size.width), static_cast<float>(size.height), 0.f, 0.f},
      .tune = {kSourceLow, kSourceHigh, 0.f, kSoften},
      .tune2 = {kOnLight, 0.f, kGrey, kWhite},
  };
}

// The parameters for the game's picture inside the EFB (letterbox bars stay black; the whole EFB when the viewport says
// nothing).
Params picture_params(wgpu::Extent3D size, const webgpu::Viewport& viewport) {
  const float left = std::clamp(viewport.left, 0.f, static_cast<float>(size.width));
  const float top = std::clamp(viewport.top, 0.f, static_cast<float>(size.height));
  const float right = std::clamp(viewport.left + viewport.width, left, static_cast<float>(size.width));
  const float bottom = std::clamp(viewport.top + viewport.height, top, static_cast<float>(size.height));
  const bool hasViewport = right - left >= 1.f && bottom - top >= 1.f;
  auto params = base_params(size);
  params.vp = hasViewport ? std::array{left, top, right, bottom}
                          : std::array{0.f, 0.f, static_cast<float>(size.width), static_cast<float>(size.height)};
  return params;
}

std::array<uint32_t, 4> picture_scissor(const Params& params) {
  return {static_cast<uint32_t>(params.vp[0]), static_cast<uint32_t>(params.vp[1]),
          static_cast<uint32_t>(params.vp[2] - params.vp[0]), static_cast<uint32_t>(params.vp[3] - params.vp[1])};
}
} // namespace

void set_strength(float strength) noexcept {
  g_strength.store(std::isfinite(strength) ? std::clamp(strength, 0.f, 1.f) : 0.f, std::memory_order_relaxed);
}
float strength() noexcept { return g_strength.load(std::memory_order_relaxed); }

void set_view(int mode) noexcept { g_viewMode.store(mode >= 0 && mode <= 2 ? mode : 0, std::memory_order_relaxed); }
int view() noexcept { return g_viewMode.load(std::memory_order_relaxed); }

bool wanted() noexcept { return strength() > 0.f || view() != 0; }

void set_light_textures(const std::vector<std::string>& keys) noexcept {
  std::lock_guard lock(g_lightTexturesMutex);
  g_lightTextures = {keys.begin(), keys.end()};
  g_lightListVersion.fetch_add(1, std::memory_order_release);
}

bool is_light_texture(TextureRef& texture) noexcept {
  const uint32_t version = g_lightListVersion.load(std::memory_order_acquire);
  if (texture.lightListVersion != version) {
    std::lock_guard lock(g_lightTexturesMutex);
    texture.lightTexture = !texture.debugKey.empty() && g_lightTextures.contains(texture.debugKey);
    texture.lightListVersion = version;
  }
  return texture.lightTexture;
}

void encode_keep_depth(const wgpu::CommandEncoder& encoder, const wgpu::Texture& depth, uint32_t samples) {
  if (!wanted() || !depth) {
    return;
  }
  const wgpu::Extent3D size{depth.GetWidth(), depth.GetHeight(), 1};
  const auto format = depth.GetFormat();
  if (!g_stageDepth.texture || g_stageDepth.size.width != size.width || g_stageDepth.size.height != size.height ||
      g_stageDepth.format != format || g_stageDepth.texture.GetSampleCount() != samples) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Bloom stage depth",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst,
        .size = size,
        .format = format,
        .sampleCount = samples,
    };
    g_stageDepth.texture = g_device.CreateTexture(&descriptor);
    g_stageDepth.view = g_stageDepth.texture.CreateView();
    g_stageDepth.size = size;
    g_stageDepth.format = format;
  }
  const wgpu::TexelCopyTextureInfo from{.texture = depth};
  const wgpu::TexelCopyTextureInfo to{.texture = g_stageDepth.texture};
  encoder.CopyTextureToTexture(&from, &to, &size);
}

const wgpu::TextureView& stage_depth() noexcept { return g_stageDepth.view; }

MotionScratch motion_scratch(wgpu::Extent3D size, uint32_t samples) {
  prepare(g_motionScratch[0], size, "Bloom motion stand-in", wgpu::TextureFormat::RG16Float, samples);
  prepare(g_motionScratch[1], size, "Bloom motion depth stand-in", wgpu::TextureFormat::R32Float, samples);
  prepare(g_motionScratch[2], size, "Bloom HUD mark stand-in", wgpu::TextureFormat::R8Unorm, samples);
  return {.motion = g_motionScratch[0].view, .depth = g_motionScratch[1].view, .mark = g_motionScratch[2].view};
}

TailTarget tail_target(wgpu::Extent3D size, uint32_t samples) {
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  prepare(g_tailResolved, size, "Bloom tail", format);
  if (samples <= 1) {
    g_tail = {};
    return {.view = g_tailResolved.view};
  }
  prepare(g_tail, size, "Bloom tail (multisampled)", format, samples);
  return {.view = g_tail.view, .resolve = g_tailResolved.view};
}

void encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, wgpu::Extent3D size, uint32_t samples,
            const webgpu::Viewport& viewport) {
  const int mode = view();
  if (!wanted() || !g_tailResolved.texture || !color || size.width == 0 || size.height == 0) {
    return;
  }
  ZoneScoped;
  build_pipelines(webgpu::g_graphicsConfig.surfaceConfiguration.format, samples);
  std::array<wgpu::Extent3D, kLevels> sizes{};
  sizes[0] = half_of(size);
  for (size_t i = 1; i < kLevels; ++i)
    sizes[i] = half_of(sizes[i - 1]);
  for (size_t i = 0; i < kLevels; ++i)
    prepare(g_levels[i], sizes[i], "Bloom glow");
  auto params = picture_params(size, viewport);
  size_t slot = 0;
  params.dst = size_of(sizes[0]);
  draw(encoder, Kind::Source, slot++, params, g_levels[0].view, sizes[0], false, {.tail = &g_tailResolved.view});
  if (mode != 2) {
    for (size_t i = 1; i < kLevels; ++i) {
      params.src = size_of(sizes[i - 1]);
      params.dst = size_of(sizes[i]);
      draw(encoder, i == 1 ? Kind::DownFirst : Kind::Down, slot++, params, g_levels[i].view, sizes[i], false,
           {.src = &g_levels[i - 1].view});
    }
    for (size_t i = kLevels - 1; i > 0; --i) {
      params.src = size_of(sizes[i]);
      params.dst = size_of(sizes[i - 1]);
      draw(encoder, Kind::Up, slot++, params, g_levels[i - 1].view, sizes[i - 1], true, {.src = &g_levels[i].view});
    }
  }
  if (mode != 0) {
    g_viewReady = true; // encode_view shows it after the tail
    return;
  }
  // The glow now holds every size added up; at full strength, kIntensity of their average is added.
  params.src = size_of(sizes[0]);
  params.dst = size_of(size);
  params.tune[2] = kIntensity * strength() / static_cast<float>(kLevels);
  const auto scissor = picture_scissor(params);
  draw(encoder, Kind::Composite, slot, params, color, size, true,
       {.tail = &g_tailResolved.view, .src = &g_levels[0].view}, &scissor);
}

void encode_view(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, wgpu::Extent3D size,
                 uint32_t samples, const webgpu::Viewport& viewport) {
  const int mode = view();
  const bool ready = std::exchange(g_viewReady, false);
  if (mode == 0 || !ready || !g_levels[0].texture || !color || size.width == 0 || size.height == 0 ||
      g_levels[0].size.width != half_of(size).width || g_levels[0].size.height != half_of(size).height) {
    return;
  }
  ZoneScoped;
  build_pipelines(webgpu::g_graphicsConfig.surfaceConfiguration.format, samples);
  auto params = picture_params(size, viewport);
  params.src = size_of(g_levels[0].size);
  params.dst = size_of(size);
  // The glow at full strength, or the source as it is.
  params.tune[2] = mode == 2 ? 1.f : kIntensity / static_cast<float>(kLevels);
  params.tune2[1] = mode == 2 ? 1.f : 0.f;
  const auto scissor = picture_scissor(params);
  draw(encoder, Kind::View, kDraws - 1, params, color, size, true, {.src = &g_levels[0].view}, &scissor);
}

void release_unused_targets() noexcept {
  if (g_tailResolved.texture && !wanted()) {
    g_tail = {};
    g_tailResolved = {};
    g_motionScratch = {};
    g_stageDepth = {};
    g_levels = {};
  }
}

bool holds_targets() noexcept { return static_cast<bool>(g_tailResolved.texture); }

void shutdown() noexcept {
  g_pipelines = {};
  g_sampler = {};
  g_tail = {};
  g_tailResolved = {};
  g_motionScratch = {};
  g_stageDepth = {};
  g_levels = {};
  g_params = {};
}
} // namespace aurora::gfx::bloom
