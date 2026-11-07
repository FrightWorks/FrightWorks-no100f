// ROM-free integration probe. Links the actual maintained Aurora renderer.
#include "gfx/common.hpp"
#include "gfx/clear.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/motion_effects.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/texture.hpp"
#include "gx/gx.hpp"
#include "gx/fifo.hpp"
#include "gx/command_processor.hpp"
#include "gx/frame_interpolation.hpp"
#include <dolphin/gx.h>
#include <aurora/aurora.h>
#include "webgpu/hdr_presentation.hpp"
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace {
using namespace aurora;
std::atomic<unsigned> errors{};
std::atomic<unsigned> guestWrites{};
// Keep destinations alive through shutdown, including any failing wait.
std::array<uint8_t, 16 * 16 * 4 + 32> guarded;
std::array<uint8_t, 16 * 16 * 4 + 32> guardedBake;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void submit(bool final, bool download = false, bool async = false, bool sealed = false) {
  auto encoder = webgpu::g_device.CreateCommandEncoder();
  if (final) gfx::end_frame(encoder); else gfx::end_batch(encoder);
  std::optional<gfx::SealedFrame> frame;
  if (sealed) {
    frame.emplace();
    gfx::seal_frame(*frame);
    gfx::render(*frame, encoder);
  } else {
    gfx::render(encoder);
  }
  if (download) gfx::efb_ram::encode_downloads(encoder);
  if (async) gfx::efb_ram::encode_async_downloads(encoder);
  auto commands = encoder.Finish();
  webgpu::g_queue.Submit(1, &commands);
  if (download) require(gfx::efb_ram::complete_downloads(), "EFB readback failed");
  gfx::after_submit();
  if (!final) require(gfx::resume_frame(), "Batch resume failed");
}

constexpr std::array<std::array<uint8_t, 4>, 4> colors{{
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
using Pixels = std::vector<uint8_t>;
Pixels expected(unsigned extent) {
  Pixels bytes(extent * extent * 4);
  // GX RGBA8: 4x4 tiles, sixteen A/R pairs followed by sixteen G/B pairs.
  for (unsigned y = 0; y < extent; ++y) for (unsigned x = 0; x < extent; ++x) {
    const auto color = colors[y * 4 / extent];
    const auto tile = ((y / 4) * (extent / 4) + x / 4) * 64;
    const auto pair = ((y % 4) * 4 + x % 4) * 2;
    bytes[tile + pair] = color[3]; bytes[tile + pair + 1] = color[0];
    bytes[tile + 32 + pair] = color[1]; bytes[tile + 33 + pair] = color[2];
  }
  return bytes;
}

Pixels run(unsigned splitEvery, bool async = false, bool offscreen = false, unsigned geometry = 0, bool capacityStress = false, bool interpolate = false, bool frameWorker = false, unsigned copyCase = 0) {
  require(!async || !offscreen, "Combined probe mode is not supported");
  guardedBake.fill(0xa5);
  gx::g_gxState.clearColor = {0.f, 0.f, 0.f, 1.f};
  require(frameWorker ? aurora_begin_frame() : gfx::begin_frame(), "Frame begin failed");
  std::array<std::array<float, 3>, 4> positions{};
  if (geometry) {
    alignas(32) static std::array<uint8_t, 32768> fifo;
    GXInit(fifo.data(), fifo.size());
    gx::g_gxState.viewportPolicy = AURORA_VIEWPORT_NATIVE;
    GXSetViewport(0.f, 0.f, 64.f, 64.f, 0.f, 1.f);
    GXSetScissor(0, 0, 64, 64);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(false, GX_ALWAYS, false);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetColorUpdate(true); GXSetAlphaUpdate(true);
    GXSetNumTexGens(0); GXSetNumChans(1); GXSetNumTevStages(copyCase ? copyCase : 1);
    for (unsigned stage = 1; stage < copyCase; ++stage) {
      GXSetTevOrder(static_cast<GXTevStageID>(stage), GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
      GXSetTevOp(static_cast<GXTevStageID>(stage), GX_PASSCLR);
    }
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    const float projection[]{interpolate ? 0.f : 1.f, 1.f, 0.f, 1.f, 0.f, 0.f, -0.5f};
    GXSetProjectionv(projection);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, geometry == 2 ? GX_INDEX8 : GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    if (geometry == 2) GXSetArray(GX_VA_POS, positions.data(), sizeof(positions), sizeof(positions[0]), true);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    gx::fifo::drain();
  }
  const auto pipeline = gfx::pipeline_ref(gfx::clear::PipelineConfig{});
  for (unsigned band = 0; band < 4; ++band) {
    const auto c = colors[band];
    if (geometry) {
      const float top = 1.f - band * 0.5f;
      const float bottom = top - 0.5f;
      const float z = interpolate ? -1.f : 0.f;
      positions = {{{-1.f, top, z}, {1.f, top, z}, {1.f, bottom, z}, {-1.f, bottom, z}}};
      // Keep the same array address/format and change only its bytes between draws.
      if (geometry == 2) GXInvalidateVtxCache();
      if (geometry == 3) {
        std::array<uint8_t, 64> raw{};
        for (unsigned index = 0; index < positions.size(); ++index) {
          for (unsigned axis = 0; axis < 3; ++axis) {
            const auto bits = std::bit_cast<uint32_t>(positions[index][axis]);
            for (unsigned byte = 0; byte < 4; ++byte)
              raw[index * 16 + axis * 4 + byte] = bits >> (24 - byte * 8);
          }
          std::copy(c.begin(), c.end(), raw.begin() + index * 16 + 12);
        }
        require(gx::fifo::submit_raw_draw(GX_QUADS, GX_VTXFMT0, raw.data(), 4, raw.size()),
                "Raw bridge rejected valid quad");
      } else {
      GXBegin(GX_QUADS, GX_VTXFMT0, 4);
      for (unsigned index = 0; index < positions.size(); ++index) {
        if (geometry == 2) GXPosition1x8(index);
        else GXPosition3f32(positions[index][0], positions[index][1], positions[index][2]);
        GXColor4u8(c[0], c[1], c[2], 255);
      }
      GXEnd();
      }
    } else {
    gfx::push_draw_command(gfx::clear::DrawData{
        .pipeline = pipeline,
        .color = {c[0] / 255., c[1] / 255., c[2] / 255., 1.},
        .depth = 0.5f,
        .useScissor = true,
        .scissor = {0, static_cast<int32_t>(band * 16), 64, 16}});
    }
    if (offscreen && band == 0) {
      // Suspend a partially recorded EFB, bake an independently observable copy,
      // then resume it before a possible capacity-boundary submission.
      gfx::begin_offscreen(64, 64);
      gfx::push_draw_command(gfx::clear::DrawData{
          .pipeline = pipeline, .color = {1., 0., 1., 1.}, .depth = 0.25f});
      if (capacityStress) for (unsigned draw = 0; draw < 24; ++draw) {
        gfx::push_draw_command(gfx::clear::DrawData{
            .pipeline = pipeline, .color = {1., 0., 1., 1.}, .depth = 0.25f,
            .useScissor = true, .scissor = {0, 0, 4, 4}});
      }
      auto baked = gfx::new_render_texture(64, 64, GX_TF_RGBA8, "Aurora probe offscreen bake");
      gfx::resolve_pass(baked, {0, 0, 64, 64}, false, false, false,
                        {0.f, 0.f, 0.f, 1.f}, 1.f, GX_TF_RGBA8, nullptr, false,
                        nullptr, false, 1.f, false, false, true);
      gfx::efb_ram::schedule(guardedBake.data() + 16, 16, 16, GX_TF_RGBA8, baked);
      gfx::end_offscreen();
      if (capacityStress) {
        require(gfx::efb_ram::prepare_downloads(), "Early bake readback preparation failed");
        for (unsigned draw = 0; draw < 24; ++draw) {
          gfx::push_draw_command(gfx::clear::DrawData{
              .pipeline = pipeline, .color = {1., 0., 0., 1.}, .depth = 0.5f,
              .useScissor = true, .scissor = {0, 0, 4, 4}});
        }
      }
    }
    if (splitEvery && band < 3 && (band + 1) % splitEvery == 0) submit(false);
  }
  auto texture = gfx::new_render_texture(64, 64, GX_TF_RGBA8, "Aurora probe persistent copy");
  static std::array<uint8_t, 64 * 64 * 4> copyDestination;
  if (copyCase) {
    gx::fifo::drain();
    GXSetTexCopySrc(0, 0, 64, 64);
    GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(copyDestination.data(), GX_FALSE);
    texture = gx::g_gxState.copyTextures.at(copyDestination.data()).handle;
  } else {
    // A partial clear forces the real snapshot and clear-uniform paths after the copy. Like GXCopyTex, an EFB
    // without alpha (GXInit's RGB8) copies as opaque: GX draws do not write its alpha, so it would otherwise be
    // whatever an earlier case left in the EFB (it persists between frames).
    gfx::resolve_pass(texture, {0, 0, 64, 64}, true, true, true, {0.f, 0.f, 0.f, 1.f}, 1.f, GX_TF_RGBA8, nullptr, false,
                      nullptr, !gx::render_target_has_alpha(gx::g_gxState.pixelFmt), 1.f, false, false, true);
  }
  guarded.fill(0xa5);
  const unsigned extent = async ? 4 : 16;
  const unsigned bytes = extent * extent * 4;
  gfx::efb_ram::schedule(guarded.data() + 16, extent, extent, GX_TF_RGBA8, texture);
  const auto before = guestWrites.load(std::memory_order_acquire);
  if (async) gfx::efb_ram::seal_async_downloads();
  else require(gfx::efb_ram::prepare_downloads(), "Readback preparation failed");
  if (frameWorker) {
    require(!async && aurora_flush_efb_copies_to_ram(), "Worker-mode EFB readback failed");
    aurora_end_frame();
  } else submit(true, !async, async);
  if (async) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (guestWrites.load(std::memory_order_acquire) == before) {
      webgpu::g_instance.ProcessEvents();
      require(std::chrono::steady_clock::now() < deadline, "Async readback did not complete");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  require(std::all_of(guarded.begin(), guarded.begin() + 16, [](auto b) { return b == 0xa5; }) &&
          std::all_of(guarded.begin() + 16 + bytes, guarded.end(), [](auto b) { return b == 0xa5; }),
          "Readback wrote outside its destination");
  if (offscreen) {
    require(std::all_of(guardedBake.begin(), guardedBake.begin() + 16, [](auto b) { return b == 0xa5; }) &&
            std::all_of(guardedBake.end() - 16, guardedBake.end(), [](auto b) { return b == 0xa5; }),
            "Offscreen readback wrote outside its destination");
    for (unsigned tile = 0; tile < 16; ++tile) for (unsigned pair = 0; pair < 16; ++pair) {
      const auto offset = 16 + tile * 64 + pair * 2;
      require(guardedBake[offset] == 255 && guardedBake[offset + 1] == 255 &&
              guardedBake[offset + 32] == 0 && guardedBake[offset + 33] == 255,
              "Offscreen bake did not preserve expected magenta pixels");
    }
  }
  Pixels pixels(guarded.begin() + 16, guarded.begin() + 16 + bytes);
  return pixels;
}

enum class ParticlePath { Bridge, Immediate, ImmediateThenBridge, BridgeThenImmediate };

constexpr std::array particlePaths{
    ParticlePath::Bridge, ParticlePath::Immediate,
    ParticlePath::ImmediateThenBridge, ParticlePath::BridgeThenImmediate};

Pixels run_alpha_order(gx::CutoutMode mode, unsigned variant = 0,
                       ParticlePath path = ParticlePath::Bridge) {
  gx::set_cutout_mode(mode);
  gx::g_gxState.clearColor = {0.f, 0.f, 0.f, 1.f};
  require(gfx::begin_frame(), "Alpha-order frame begin failed");
  gfx::push_draw_command(gfx::clear::DrawData{
      .pipeline = gfx::pipeline_ref(gfx::clear::PipelineConfig{}),
      .color = {0.f, 0.f, 0.f, 1.f}, .depth = gx::clear_depth_value()});
  alignas(32) static std::array<uint8_t, 32768> fifo;
  alignas(32) static std::array<uint8_t, 64> white;
  white.fill(255);
  GXInit(fifo.data(), fifo.size());
  gx::g_gxState.viewportPolicy = AURORA_VIEWPORT_NATIVE;
  GXSetViewport(0.f, 0.f, 64.f, 64.f, 0.f, 1.f);
  GXSetScissor(0, 0, 64, 64);
  GXSetCullMode(GX_CULL_NONE);
  GXSetColorUpdate(true);
  GXSetAlphaUpdate(true);
  GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
  GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetNumChans(1);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
  GXSetChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXTexObj texture{};
  GXInitTexObj(&texture, white.data(), 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, false);
  GXLoadTexObj(&texture, GX_TEXMAP0);
  const float projection[]{0.f, 1.f, 0.f, 1.f, 0.f, -0.01f, -0.1f};
  GXSetProjectionv(projection);
  const float identity[3][4]{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
  GXLoadPosMtxImm(identity, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  const auto quad = [](float z, std::array<u8, 4> color) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (const auto& xy : std::array<std::array<float, 2>, 4>{{{-1, 1}, {1, 1}, {1, -1}, {-1, -1}}}) {
      GXPosition3f32(xy[0] * -z, xy[1] * -z, z);
      GXColor4u8(color[0], color[1], color[2], color[3]);
      GXTexCoord2f32(0.f, 0.f);
    }
    GXEnd();
  };
  GXSetZMode(true, GX_LEQUAL, true);
  const bool particleCase = variant >= 11 && variant < 25;
  quad(particleCase ? -8.f : -4.f, {0, 0, 255, 255});
  if (particleCase) {
    std::array<std::array<float, 3>, 4> post{{{-4, 4, -4}, {0, 4, -4}, {0, -4, -4}, {-4, -4, -4}}};
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetArray(GX_VA_POS, post.data(), sizeof(post), sizeof(post[0]), true);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (unsigned index = 0; index < post.size(); ++index) {
      GXPosition1x8(index); GXColor4u8(255, 0, 0, 255); GXTexCoord2f32(0, 0);
    }
    GXEnd();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetZMode(true, GX_LEQUAL, false);
    const auto particle = [](std::vector<uint8_t>& raw, float z, std::array<u8, 4> color,
                             bool nonPlanar = false, bool rotated = false) {
      unsigned vertex = 0;
      for (const auto& xy : std::array<std::array<float, 2>, 6>{
               {{-1, 1}, {1, 1}, {1, -1}, {-1, 1}, {1, -1}, {-1, -1}}}) {
        const float vz = z + (nonPlanar && vertex == 1 ? .2f : 0.f) + (rotated ? xy[0] * .2f : 0.f);
        const float size = rotated ? .15f : 1.f;
        for (const float value : {xy[0] * -z * size, xy[1] * -z * size, vz}) {
          const auto bits = std::bit_cast<uint32_t>(value);
          for (int shift : {24, 16, 8, 0}) raw.push_back(bits >> shift);
        }
        raw.insert(raw.end(), color.begin(), color.end());
        for (const float value : {(xy[0] + 1.f) * .5f, (1.f - xy[1]) * .5f}) {
          const auto bits = std::bit_cast<uint32_t>(value);
          for (int shift : {24, 16, 8, 0}) raw.push_back(bits >> shift);
        }
        ++vertex;
      }
    };
    auto send = [path, draw = 0u](const std::vector<uint8_t>& raw) mutable {
      const bool immediate = path == ParticlePath::Immediate ||
                             (path == ParticlePath::ImmediateThenBridge && draw == 0) ||
                             (path == ParticlePath::BridgeThenImmediate && draw != 0);
      ++draw;
      if (!immediate) {
        require(gx::fifo::submit_raw_draw(GX_TRIANGLES, GX_VTXFMT0, raw.data(), raw.size() / 24, raw.size()),
                "Particle bridge rejected test data");
        return;
      }
      const auto read = [&](size_t offset) {
        const uint32_t bits = uint32_t(raw[offset]) << 24 | uint32_t(raw[offset + 1]) << 16 |
                              uint32_t(raw[offset + 2]) << 8 | uint32_t(raw[offset + 3]);
        return std::bit_cast<float>(bits);
      };
      GXBegin(GX_TRIANGLES, GX_VTXFMT0, raw.size() / 24);
      for (size_t offset = 0; offset < raw.size(); offset += 24) {
        GXPosition3f32(read(offset), read(offset + 4), read(offset + 8));
        GXColor4u8(raw[offset + 12], raw[offset + 13], raw[offset + 14], raw[offset + 15]);
        GXTexCoord2f32(read(offset + 16), read(offset + 20));
      }
      GXEnd();
    };
    std::vector<uint8_t> green;
    const u8 alpha = variant == 22 ? 64 : variant == 23 ? 255 : variant == 24 ? 0 : 128;
    particle(green, -2, {0, 255, 0, alpha});
    send(green);
    if (variant >= 21) {
      std::vector<uint8_t> leaf;
      particle(leaf, -3, {111, 83, 49, 180}, false, true);
      send(leaf);
    }
    if (variant == 13) quad(-3, {255, 255, 255, 128});
    if (variant == 14) GXSetScissor(32, 0, 32, 64);
    if (variant == 15) {
      const float changed[]{0.f, 1.01f, 0.f, 1.f, 0.f, -0.01f, -0.1f};
      GXSetProjectionv(changed);
    }
    if (variant == 18) GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_COPY);
    std::vector<uint8_t> gray;
    particle(gray, variant == 17 ? -2.f : -6.f, {128, 128, 128, 128}, variant == 16);
    if (variant != 12) particle(gray, variant == 17 ? -2.f : -1.f, {0, 0, 255, 128});
    send(gray);
    if (variant == 19) gfx::flush_deferred_draws();
  } else {
  GXSetZMode(true, GX_LEQUAL, false);
  if (variant == 4) GXSetScissor(0, 0, 32, 64);
  if (variant == 5) GXSetViewport(0.f, 0.f, 32.f, 64.f, 0.f, 1.f);
  quad(-2.f, {0, 255, 0, 128});
  if (variant == 3) quad(-1.f, {0, 0, 255, 128});
  GXSetScissor(0, 0, 64, 64);
  GXSetViewport(0.f, 0.f, 64.f, 64.f, 0.f, 1.f);
  GXSetZMode(true, GX_LEQUAL, true);
  if (variant >= 25) {
    // An invisible blended surface the game draws after its particles, depth write on and no alpha test.
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    quad(-1.5f, {255, 0, 0, 0});
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
  }
  if (variant == 26) {
    GXSetZMode(true, GX_LEQUAL, false);
    quad(-1.8f, {255, 255, 255, 128});  // behind that surface and after it: its depth still hides this
    GXSetZMode(true, GX_LEQUAL, true);
  }
  if (variant < 25 || variant == 27)
    quad(variant == 1 ? -1.5f : -3.f, {255, 0, 0, static_cast<u8>(variant == 2 ? 64 : 255)});
  if (variant == 6 || variant == 7 || variant == 9 || variant == 10) {
    if (variant == 9) {
      const float ortho[]{1.f, 1.f, 0.f, 1.f, 0.f, 0.f, -0.5f};
      GXSetProjectionv(ortho);
    }
    GXSetZMode(variant != 7, variant == 9 ? GX_LEQUAL : variant == 10 ? GX_EQUAL : GX_ALWAYS,
               variant == 6);
    quad(variant == 10 ? -3.f : -1.f, {255, 255, 255, 255});
  }
  }
  gx::fifo::drain();
  if (variant == 8) {
    gfx::push_draw_command(gfx::clear::DrawData{
        .pipeline = gfx::pipeline_ref(gfx::clear::PipelineConfig{}),
        .color = {1., 1., 1., 1.}, .depth = 1.f});
  }
  auto target = gfx::new_render_texture(64, 64, GX_TF_RGBA8, "Alpha order regression");
  gfx::resolve_pass(target, {0, 0, 64, 64}, false, false, false,
                    {0.f, 0.f, 0.f, 1.f}, 1.f, GX_TF_RGBA8, nullptr, false,
                    nullptr, false, 1.f, false, false, true);
  guarded.fill(0xa5);
  gfx::efb_ram::schedule(guarded.data() + 16, 16, 16, GX_TF_RGBA8, target);
  require(gfx::efb_ram::prepare_downloads(), "Alpha-order readback preparation failed");
  submit(true, true, false, variant == 20);
  return Pixels(guarded.begin() + 16, guarded.begin() + 16 + 16 * 16 * 4);
}

void check_alpha_pixel(const Pixels& pixels, unsigned x, std::array<int, 3> rgb) {
  const unsigned tile = (2 * 4 + x / 4) * 64;
  const unsigned pair = (x % 4) * 2;
  const std::array actual{pixels[tile + pair + 1], pixels[tile + 32 + pair], pixels[tile + 33 + pair]};
  for (unsigned c = 0; c < 3; ++c) {
    if (std::abs(static_cast<int>(actual[c]) - rgb[c]) > 2) {
      std::fprintf(stderr, "alpha-order x=%u expected=%d,%d,%d actual=%u,%u,%u\n",
                   x, rgb[0], rgb[1], rgb[2], actual[0], actual[1], actual[2]);
      require(false, "Alpha-order pixel mismatch");
    }
  }
}

// Half-float to float (shared by the HDR present shader test and the motion vector tests).
float half_to_float(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;
  if (exponent == 0) {
    const float value = std::ldexp(static_cast<float>(mantissa), -24);
    return sign ? -value : value;
  }
  return std::bit_cast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
}

// The HDR present shader (calibration uniform + scRGB picture) on the real device: the exact WGSL the game builds,
// drawn onto an RGBA16Float target from a flat grey source with both calibration patches on. Checks that it compiles
// and pipelines without validation errors, that the picture is the linearised, paper-white-scaled, peak-boosted grey,
// that both checker patches show their nit targets (bright and dim cells), and that pixels outside them are the picture.
void check_hdr_present_shader() {
  constexpr uint32_t size = 64;
  constexpr float paper = 200.f, peak = 1000.f, paperPatchNits = 300.f, peakPatchNits = 1000.f;
  const auto code = webgpu::copy_shader_source(webgpu::hdr_fragment_tail(), true);
  wgpu::ShaderSourceWGSL source{};
  source.code = code.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "HDR present test shader"};
  const auto module = webgpu::g_device.CreateShaderModule(&moduleDescriptor);

  const std::array<wgpu::BindGroupLayoutEntry, 2> pictureEntries{
      wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Fragment,
                                 .sampler = {.type = wgpu::SamplerBindingType::Filtering}},
      wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}}};
  const wgpu::BindGroupLayoutDescriptor pictureLayoutDescriptor{.entryCount = pictureEntries.size(),
                                                                .entries = pictureEntries.data()};
  const auto pictureLayout = webgpu::g_device.CreateBindGroupLayout(&pictureLayoutDescriptor);
  const wgpu::BindGroupLayoutEntry uniformEntry{
      .binding = 0, .visibility = wgpu::ShaderStage::Fragment,
      .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(webgpu::HdrPresentationUniform)}};
  const wgpu::BindGroupLayoutDescriptor uniformLayoutDescriptor{.entryCount = 1, .entries = &uniformEntry};
  const auto uniformLayout = webgpu::g_device.CreateBindGroupLayout(&uniformLayoutDescriptor);
  const std::array layouts{pictureLayout, uniformLayout};
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{.bindGroupLayoutCount = layouts.size(),
                                                                .bindGroupLayouts = layouts.data()};
  const auto pipelineLayout = webgpu::g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ColorTargetState colorTarget{.format = wgpu::TextureFormat::RGBA16Float,
                                           .writeMask = wgpu::ColorWriteMask::All};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = 1,
                                     .targets = &colorTarget};
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = "HDR present test pipeline", .layout = pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main"}, .fragment = &fragment};
  const auto pipeline = webgpu::g_device.CreateRenderPipeline(&pipelineDescriptor);

  // A 4x4 grey (128/255) source, so every sample, filtered or not, is the same grey.
  const wgpu::TextureDescriptor sourceDescriptor{
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {4, 4, 1}, .format = wgpu::TextureFormat::RGBA8Unorm};
  const auto sourceTexture = webgpu::g_device.CreateTexture(&sourceDescriptor);
  std::vector<uint8_t> grey(256 * 4, 0);
  for (unsigned row = 0; row < 4; ++row)
    for (unsigned texel = 0; texel < 4; ++texel)
      for (unsigned c = 0; c < 4; ++c) grey[row * 256 + texel * 4 + c] = c == 3 ? 255 : 128;
  const wgpu::TexelCopyTextureInfo sourceCopy{.texture = sourceTexture};
  const wgpu::TexelCopyBufferLayout sourceLayout{.bytesPerRow = 256, .rowsPerImage = 4};
  const wgpu::Extent3D sourceExtent{4, 4, 1};
  webgpu::g_queue.WriteTexture(&sourceCopy, grey.data(), grey.size(), &sourceLayout, &sourceExtent);
  const wgpu::SamplerDescriptor samplerDescriptor{.magFilter = wgpu::FilterMode::Linear,
                                                  .minFilter = wgpu::FilterMode::Linear};
  const auto sampler = webgpu::g_device.CreateSampler(&samplerDescriptor);
  const std::array<wgpu::BindGroupEntry, 2> pictureBindings{
      wgpu::BindGroupEntry{.binding = 0, .sampler = sampler},
      wgpu::BindGroupEntry{.binding = 1, .textureView = sourceTexture.CreateView()}};
  const wgpu::BindGroupDescriptor pictureGroupDescriptor{.layout = pictureLayout, .entryCount = pictureBindings.size(),
                                                         .entries = pictureBindings.data()};
  const auto pictureGroup = webgpu::g_device.CreateBindGroup(&pictureGroupDescriptor);

  // The uniform comes from the production helper; only the two patch rectangles and nit targets are set here.
  auto uniform = webgpu::hdr_presentation_uniform(paper, peak, size, size);
  const std::array<float, 4> paperRect{.1f, .1f, .4f, .4f}, peakRect{.6f, .1f, .9f, .4f};
  uniform.paperRect = paperRect;
  uniform.peakRect = peakRect;
  uniform.parameters[0] = paperPatchNits;
  uniform.parameters[1] = peakPatchNits;
  const wgpu::BufferDescriptor uniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                 .size = sizeof(uniform)};
  const auto uniformBuffer = webgpu::g_device.CreateBuffer(&uniformDescriptor);
  webgpu::g_queue.WriteBuffer(uniformBuffer, 0, &uniform, sizeof(uniform));
  const wgpu::BindGroupEntry uniformBinding{.binding = 0, .buffer = uniformBuffer, .size = sizeof(uniform)};
  const wgpu::BindGroupDescriptor uniformGroupDescriptor{.layout = uniformLayout, .entryCount = 1,
                                                         .entries = &uniformBinding};
  const auto uniformGroup = webgpu::g_device.CreateBindGroup(&uniformGroupDescriptor);

  const wgpu::TextureDescriptor targetDescriptor{
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
      .size = {size, size, 1}, .format = wgpu::TextureFormat::RGBA16Float};
  const auto target = webgpu::g_device.CreateTexture(&targetDescriptor);
  constexpr uint32_t rowBytes = size * 8;  // already a multiple of 256
  const wgpu::BufferDescriptor readbackDescriptor{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                                  .size = rowBytes * size};
  const auto readback = webgpu::g_device.CreateBuffer(&readbackDescriptor);
  const auto encoder = webgpu::g_device.CreateCommandEncoder();
  const wgpu::RenderPassColorAttachment attachment{.view = target.CreateView(), .loadOp = wgpu::LoadOp::Clear,
                                                   .storeOp = wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor passDescriptor{.colorAttachmentCount = 1, .colorAttachments = &attachment};
  const auto pass = encoder.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, pictureGroup);
  pass.SetBindGroup(1, uniformGroup);
  pass.SetViewport(0.f, 0.f, size, size, 0.f, 1.f);
  pass.Draw(3);
  pass.End();
  const wgpu::TexelCopyTextureInfo from{.texture = target};
  const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = rowBytes, .rowsPerImage = size}, .buffer = readback};
  const wgpu::Extent3D extent{size, size, 1};
  encoder.CopyTextureToBuffer(&from, &to, &extent);
  const auto commands = encoder.Finish();
  webgpu::g_queue.Submit(1, &commands);
  bool mapped = false;
  const auto mapFuture = readback.MapAsync(wgpu::MapMode::Read, 0, rowBytes * size, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                             mapped = status == wgpu::MapAsyncStatus::Success;
                                           });
  webgpu::g_instance.WaitAny(mapFuture, 5000000000ull);
  require(mapped, "HDR shader test readback did not map");
  const auto* data = static_cast<const uint16_t*>(readback.GetConstMappedRange(0, rowBytes * size));
  const auto at = [&](unsigned x, unsigned y, unsigned c) { return half_to_float(data[y * size * 4 + x * 4 + c]); };
  const auto near = [](float got, float want) { return std::fabs(got - want) <= 0.01f * std::max(1.f, std::fabs(want)) + 0.002f; };
  const auto patch = [&](unsigned x, unsigned y, const std::array<float, 4>& rect, float nits, float dim) {
    const float u = (x + .5f) / size, v = (y + .5f) / size;
    const int cx = static_cast<int>(std::floor((u - rect[0]) / (rect[2] - rect[0]) * 8));
    const int cy = static_cast<int>(std::floor((v - rect[1]) / (rect[3] - rect[1]) * 4));
    return nits * (((cx + cy) & 1) == 0 ? 1.f : dim) / 80.f;
  };
  const float linear = std::pow(128.f / 255.f, 2.2f);
  const float picture = linear * (1.f + (peak / paper - 1.f) * std::pow(linear, 4.f)) * paper / 80.f;
  require(near(at(32, 48, 0), picture) && near(at(32, 48, 1), picture) && near(at(32, 48, 2), picture) &&
          near(at(32, 48, 3), 1.f), "HDR present picture is not the scaled, boosted grey");
  require(near(at(5, 5, 0), picture) && near(at(62, 62, 0), picture), "HDR present picture changed outside the patches");
  bool brightSeen = false, dimSeen = false;
  for (const auto& [rect, nits, dim] : {std::tuple{paperRect, paperPatchNits, 0.6f}, std::tuple{peakRect, peakPatchNits, 0.9f}}) {
    for (unsigned y = static_cast<unsigned>(rect[1] * size) + 1; y < static_cast<unsigned>(rect[3] * size) - 1; y += 3) {
      for (unsigned x = static_cast<unsigned>(rect[0] * size) + 1; x < static_cast<unsigned>(rect[2] * size) - 1; x += 3) {
        const float want = patch(x, y, rect, nits, dim);
        require(near(at(x, y, 0), want) && near(at(x, y, 1), want) && near(at(x, y, 2), want) && near(at(x, y, 3), 1.f),
                "HDR calibration patch value differs from its nit target");
        (want == nits / 80.f ? brightSeen : dimSeen) = true;
      }
    }
  }
  require(brightSeen && dimSeen, "HDR calibration patches did not show both checker cells");
  readback.Unmap();
  std::puts("Actual Aurora HDR present shader: picture, both calibration patches and checker cells match");
}

// ----- Motion vectors: the same perspective quad on consecutive frames, moved by its position matrix.

// The 4 bytes of a 32-bit target's pixel (x, y), as the screenshot path reads them: copied to a buffer.
std::array<uint8_t, 4> read_texel(const wgpu::Texture& texture, unsigned x, unsigned y) {
  require(static_cast<bool>(texture), "No target to read");
  const wgpu::BufferDescriptor descriptor{.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                          .size = 256};
  const auto buffer = webgpu::g_device.CreateBuffer(&descriptor);
  auto encoder = webgpu::g_device.CreateCommandEncoder();
  const wgpu::TexelCopyTextureInfo source{.texture = texture, .origin = {x, y, 0}};
  const wgpu::TexelCopyBufferInfo destination{.layout = {.bytesPerRow = 256, .rowsPerImage = 1}, .buffer = buffer};
  const wgpu::Extent3D size{1, 1, 1};
  encoder.CopyTextureToBuffer(&source, &destination, &size);
  const auto commands = encoder.Finish();
  webgpu::g_queue.Submit(1, &commands);
  std::atomic_bool done{false};
  bool mapped = false;
  buffer.MapAsync(wgpu::MapMode::Read, 0, 4, wgpu::CallbackMode::AllowProcessEvents,
                  [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                    mapped = status == wgpu::MapAsyncStatus::Success;
                    done = true;
                  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done) {
    webgpu::g_instance.ProcessEvents();
    require(std::chrono::steady_clock::now() < deadline, "Motion readback did not complete");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(mapped, "Motion readback failed");
  std::array<uint8_t, 4> result;
  std::memcpy(result.data(), buffer.GetConstMappedRange(0, 4), result.size());
  buffer.Unmap();
  return result;
}

// The EFB motion vector (render pixels, where the surface was last frame minus where it is) at pixel (x, y).
std::array<float, 2> read_motion(unsigned x, unsigned y) {
  const auto half = std::bit_cast<std::array<uint16_t, 2>>(read_texel(webgpu::g_motionVectors.texture, x, y));
  return {half_to_float(half[0]), half_to_float(half[1])};
}

// One frame: a quad over the middle half of the 64x64 viewport (z = -4), moved `shift` (normalized device
// coordinates, so 32 pixels per unit; the matrix moves it 4 units per unit) to the right by its position matrix.
// `blended`: alpha-blended, still writing depth. `twoQuads`: instead two narrower quads, x -0.9 to -0.6 and -0.4 to
// -0.1 (y -0.25 to 0.25), each its own draw on its own matrix slot, both moved the same: two still draws that agree on
// the camera's change, so a frame-wide shift reads as the camera's own.
void draw_motion_frame(float shift, bool blended = false, bool twoQuads = false) {
  gx::g_gxState.clearColor = {0.f, 0.f, 0.f, 1.f};
  require(gfx::begin_frame(), "Motion frame begin failed");
  gfx::push_draw_command(gfx::clear::DrawData{.pipeline = gfx::pipeline_ref(gfx::clear::PipelineConfig{}),
                                              .color = {0.f, 0.f, 0.f, 1.f},
                                              .depth = gx::clear_depth_value()});
  alignas(32) static std::array<uint8_t, 32768> fifo;
  GXInit(fifo.data(), fifo.size());
  gx::g_gxState.viewportPolicy = AURORA_VIEWPORT_NATIVE;
  GXSetViewport(0.f, 0.f, 64.f, 64.f, 0.f, 1.f);
  GXSetScissor(0, 0, 64, 64);
  GXSetCullMode(GX_CULL_NONE);
  GXSetColorUpdate(true);
  GXSetAlphaUpdate(true);
  if (blended) {
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
  } else {
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
  }
  GXSetZMode(true, GX_LEQUAL, true);
  GXSetNumTexGens(0);
  GXSetNumChans(1);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXSetChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  const float projection[]{0.f, 1.f, 0.f, 1.f, 0.f, -0.01f, -0.1f}; // perspective: x, y divided by -z
  GXSetProjectionv(projection);
  constexpr float z = -4.f;
  const float position[3][4]{{1.f, 0.f, 0.f, shift * -z}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
  GXLoadPosMtxImm(position, GX_PNMTX0);
  GXLoadPosMtxImm(position, GX_PNMTX1);
  GXSetCurrentMtx(GX_PNMTX0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  const auto quad = [&](float left, float right, float bottom, float top) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (const auto& xy :
         std::array<std::array<float, 2>, 4>{{{left, top}, {right, top}, {right, bottom}, {left, bottom}}}) {
      GXPosition3f32(xy[0] * -z, xy[1] * -z, z);
      GXColor4u8(255, 255, 255, 255);
    }
    GXEnd();
  };
  if (twoQuads) {
    quad(-.9f, -.6f, -.25f, .25f);
    GXSetCurrentMtx(GX_PNMTX1);
    quad(-.4f, -.1f, -.25f, .25f);
  } else {
    quad(-.5f, .5f, -.5f, .5f);
  }
  gx::fifo::drain();
  submit(true, false, false, true);
}

// Draws until the pipelines for the current motion setting are built, as unready ones are skipped.
void settle_motion_pipelines(bool blended = false, bool twoQuads = false) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (unsigned frame = 0; frame < 2 || gfx::queued_pipeline_count() != 0; ++frame) {
    require(std::chrono::steady_clock::now() < deadline, "Motion vector pipelines were not built");
    draw_motion_frame(0.f, blended, twoQuads);
    webgpu::g_instance.ProcessEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// One frame repeating the last draw with no GX state written since, so only a motion setting change can end the
// resolved pipeline: a frame with one material can do this (a movie, say).
void draw_same_state_frame(bool settingChanged) {
  const auto generation = gx::g_gxState.pipelineStateGeneration;
  gx::g_gxState.clearColor = {0.f, 0.f, 0.f, 1.f};
  require(gfx::begin_frame(), "Motion frame begin failed");
  require((generation != gx::g_gxState.pipelineStateGeneration) == settingChanged,
          settingChanged ? "Resolved pipeline kept across a motion setting change"
                         : "Resolved pipeline dropped with no motion setting change");
  constexpr float z = -4.f;
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (const auto& xy : std::array<std::array<float, 2>, 4>{{{-.5f, .5f}, {.5f, .5f}, {.5f, -.5f}, {-.5f, -.5f}}}) {
    GXPosition3f32(xy[0] * -z, xy[1] * -z, z);
    GXColor4u8(255, 255, 255, 255);
  }
  GXEnd();
  gx::fifo::drain();
  submit(true, false, false, true);
}

void check_motion(unsigned x, unsigned y, std::array<float, 2> expected, const char* what) {
  const auto actual = read_motion(x, y);
  if (std::fabs(actual[0] - expected[0]) > 0.25f || std::fabs(actual[1] - expected[1]) > 0.25f) {
    std::fprintf(stderr, "motion %s at %u,%u expected=%g,%g actual=%g,%g\n", what, x, y, expected[0], expected[1],
                 actual[0], actual[1]);
    require(false, "Motion vector mismatch");
  }
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::filesystem::create_directories(argv[1]);
  AuroraConfig config{};
  config.appName = "Aurora readback regression tests";
  config.userPath = argv[1];
  config.cachePath = argv[1];
  config.resourcesPath = argv[1];
  config.desiredBackend = BACKEND_AUTO;
  config.windowWidth = 64;
  config.windowHeight = 64;
  config.msaa = 1;
  config.maxTextureAnisotropy = 1;
  config.logLevel = LOG_INFO;
  config.logCallback = [](AuroraLogLevel level, const char* module, const char* message, unsigned size) {
    if (level >= LOG_ERROR) ++errors;
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(size), message);
  };
  const auto initialized = aurora_initialize(1, argv, &config);
  if (initialized.initializationStatus != AURORA_INITIALIZATION_SUCCESS) return 3;
  aurora_set_skip_unready_pipelines(true);
  aurora_set_guest_write_hooks(nullptr, [](const void*, size_t) {
    guestWrites.fetch_add(1, std::memory_order_release);
  });
  try {
    const auto prewarmQueued = gfx::queued_pipeline_count();
    const auto prewarmDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (gfx::queued_pipeline_count() != 0) {
      require(std::chrono::steady_clock::now() < prewarmDeadline, "Seeded pipeline prewarm did not finish");
      webgpu::g_instance.ProcessEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("Actual Aurora completed seeded startup queue (%u observed pending)\n", prewarmQueued);
    // Motion vectors: on only while requested; a still quad has none; one moved 0.25 (8 pixels) to the right by its
    // matrix was 8 pixels to the left last frame, both where it covers and nowhere else.
    require(!webgpu::g_motionVectors.texture, "Motion vector targets exist while not requested");
    gfx::request_motion_vectors(true);
    // Its pipelines (with the motion targets) are new: draw until they are built, as unready ones are skipped.
    const auto motionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (unsigned frame = 0; frame < 2 || gfx::queued_pipeline_count() != 0; ++frame) {
      require(std::chrono::steady_clock::now() < motionDeadline, "Motion vector pipelines were not built");
      draw_motion_frame(0.f);
      webgpu::g_instance.ProcessEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    draw_motion_frame(0.f);
    check_motion(32, 32, {0.f, 0.f}, "still");
    draw_motion_frame(.25f);
    check_motion(36, 32, {-8.f, 0.f}, "moved");
    check_motion(4, 32, {0.f, 0.f}, "background");
    // A camera cut: when the whole picture moves more than a camera can in one frame (here 2.4 units, over the
    // 2-unit limit), the frame has no motion; 1 unit is still motion.
    settle_motion_pipelines(false, true);
    draw_motion_frame(.25f, false, true);
    check_motion(16, 32, {-8.f, 0.f}, "camera move, first quad");
    check_motion(32, 32, {-8.f, 0.f}, "camera move, second quad");
    draw_motion_frame(0.f, false, true);
    draw_motion_frame(0.f, false, true);
    draw_motion_frame(.6f, false, true);
    check_motion(27, 32, {0.f, 0.f}, "camera cut, first quad");
    check_motion(43, 32, {0.f, 0.f}, "camera cut, second quad");
    std::printf("Actual Aurora camera cut: no motion past the cut limit, -8 px below it\n");
    gfx::request_motion_vectors(false);
    draw_motion_frame(0.f);
    require(!webgpu::g_motionVectors.texture, "Motion vector targets kept after the request ended");
    std::printf("Actual Aurora motion vectors: still 0, moved -8 px, off frees the targets\n");
    // The motion setting changes the pass's attachments: a pipeline resolved before the change must not be reused,
    // and nothing is resolved again while it stays the same.
    settle_motion_pipelines();
    draw_same_state_frame(false);
    gfx::request_motion_vectors(true);
    draw_same_state_frame(true);
    settle_motion_pipelines();
    draw_same_state_frame(false);
    gfx::request_motion_vectors(false);
    draw_same_state_frame(true);
    std::printf("Actual Aurora motion setting change resolves the pipeline again\n");
    // An alpha-blended surface moves like any other. It owns the depth only on a GPU that can blend the 32-bit float
    // target; elsewhere (AURORA_GFX_FLAGS=nofloat32blend hides the feature) it leaves the depth behind it.
    gfx::request_motion_vectors(true);
    settle_motion_pipelines(true);
    draw_motion_frame(.25f, true);
    check_motion(36, 32, {-8.f, 0.f}, "blended");
    const auto depthAt = [](unsigned x, unsigned y) {
      return std::bit_cast<float>(read_texel(webgpu::g_motionDepth.texture, x, y));
    };
    require((depthAt(36, 32) != depthAt(4, 32)) == webgpu::g_float32Blendable,
            webgpu::g_float32Blendable ? "Alpha-blended surface did not own its depth"
                                       : "Alpha-blended surface wrote depth with no blendable target");
    std::printf("Actual Aurora alpha-blended motion: moved -8 px, float32-blendable %d\n",
                webgpu::g_float32Blendable ? 1 : 0);
    // The blurred picture is what is presented, so a screenshot or frame capture copies it (aurora.cpp
    // encode_frame_capture).
    gfx::motion::set_blur(1.f);
    settle_motion_pipelines();
    {
      const auto picture = webgpu::current_present_source();
      auto encoder = webgpu::g_device.CreateCommandEncoder();
      const auto blurred = gfx::motion::encode(
          encoder, picture,
          gfx::MotionTargets{webgpu::g_motionVectors, webgpu::g_motionDepth, webgpu::g_motionMark}, 1.f);
      const auto commands = encoder.Finish();
      webgpu::g_queue.Submit(1, &commands);
      require(blurred.texture.Get() != picture.texture.Get(), "Motion blur did not replace the picture");
      require((blurred.texture.GetUsage() & wgpu::TextureUsage::CopySrc) != wgpu::TextureUsage::None,
              "Blurred picture cannot be copied to a screenshot");
      require(read_texel(blurred.texture, 32, 32)[0] > 200 && read_texel(blurred.texture, 4, 32)[0] < 50,
              "Blurred picture does not show the frame");
    }
    gfx::motion::set_blur(0.f);
    draw_motion_frame(0.f);
    require(!webgpu::g_motionVectors.texture, "Motion vector targets kept after blur was turned off");
    std::printf("Actual Aurora blurred picture copied for capture\n");
    for (const auto mode : {gx::CutoutMode::TwoPass, gx::CutoutMode::TwoPassAll}) {
      check_alpha_pixel(run_alpha_order(mode), 4, {127, 128, 0});
      check_alpha_pixel(run_alpha_order(mode, 1), 4, {255, 0, 0});
      check_alpha_pixel(run_alpha_order(mode, 3), 4, {63, 64, 128});
      check_alpha_pixel(run_alpha_order(mode, 25), 4, {0, 128, 127});
      check_alpha_pixel(run_alpha_order(mode, 26), 4, {0, 128, 127});
      check_alpha_pixel(run_alpha_order(mode, 27), 4, {127, 128, 0});
      for (unsigned variant : {4, 5}) {
        const auto pixels = run_alpha_order(mode, variant);
        check_alpha_pixel(pixels, 4, {127, 128, 0});
        check_alpha_pixel(pixels, 12, {255, 0, 0});
      }
      for (unsigned variant : {6, 7, 8, 9, 10})
        check_alpha_pixel(run_alpha_order(mode, variant), 4, {255, 255, 255});
      for (const auto path : particlePaths) {
        for (unsigned variant : {11, 12, 13, 14, 15, 16, 17, 18, 19, 20}) {
          const auto pixels = run_alpha_order(mode, variant, path);
          const auto left = variant == 12 || variant == 14 ? std::array{127, 128, 0} :
                            variant == 13 ? std::array{95, 96, 192} :
                            variant == 17 ? std::array{63, 64, 160} :
                            variant == 18 ? std::array{127, 128, 128} : std::array{63, 64, 128};
          const auto right = variant == 12 ? std::array{32, 160, 95} :
                             variant == 13 ? std::array{64, 80, 207} :
                             variant == 18 ? std::array{64, 192, 255} :
                             variant >= 14 && variant <= 17 ? std::array{32, 64, 191} : std::array{16, 80, 175};
          check_alpha_pixel(pixels, 4, left);
          check_alpha_pixel(pixels, 12, right);
        }
        for (unsigned variant : {21, 22, 23, 24}) {
          const auto pixels = run_alpha_order(mode, variant, path);
          const auto left = variant == 22 ? std::array{95, 32, 128} :
                            variant == 23 ? std::array{0, 127, 128} :
                            variant == 24 ? std::array{127, 0, 128} : std::array{63, 64, 128};
          const auto right = variant == 22 ? std::array{24, 56, 199} :
                             variant == 23 ? std::array{0, 127, 128} :
                             variant == 24 ? std::array{32, 32, 223} : std::array{16, 80, 175};
          check_alpha_pixel(pixels, 4, left);
          check_alpha_pixel(pixels, 12, right);
          if (variant == 24) check_alpha_pixel(pixels, 8, {48, 38, 173});
        }
      }
    }
    check_alpha_pixel(run_alpha_order(gx::CutoutMode::TwoPassAll, 2), 4, {32, 128, 95});
    for (const auto mode : {gx::CutoutMode::Game, gx::CutoutMode::Cut}) {
      check_alpha_pixel(run_alpha_order(mode), 4, {255, 0, 0});
      for (unsigned variant : {25, 26, 27})
        check_alpha_pixel(run_alpha_order(mode, variant), 4, {0, 128, 127});
      for (const auto path : particlePaths) {
        const auto particles = run_alpha_order(mode, 11, path);
        check_alpha_pixel(particles, 4, {63, 64, 128});
        check_alpha_pixel(particles, 12, {32, 64, 191});
        for (unsigned variant : {21, 22, 23, 24}) {
          const auto pixels = run_alpha_order(mode, variant, path);
          check_alpha_pixel(pixels, 4, variant == 22 ? std::array{95, 32, 128} :
                                     variant == 23 ? std::array{0, 127, 128} :
                                     variant == 24 ? std::array{127, 0, 128} : std::array{63, 64, 128});
          check_alpha_pixel(pixels, 12, variant == 22 ? std::array{32, 48, 207} :
                                      variant == 23 ? std::array{32, 95, 160} :
                                      variant == 24 ? std::array{32, 32, 223} : std::array{32, 64, 191});
        }
      }
    }
    gx::set_cutout_mode(gx::CutoutMode::TwoPassAll);
    std::puts("Actual Aurora late foliage, fringe/cloud blending, translucent order, viewport/scissor and barriers passed");
    const auto control = run(0);
    for (unsigned band = 0; band < 4; ++band) {
      const auto tile = band * 4 * 64;
      std::fprintf(stderr, "band=%u ARGB=%u,%u,%u,%u\n", band, control[tile],
                   control[tile + 1], control[tile + 32], control[tile + 33]);
    }
    require(control == expected(16), "Unsplit pixels differ from independently expected GX data");
    for (unsigned iteration = 0; iteration < 9; ++iteration) {
      const auto splitEvery = iteration % 3 + 1;
      require(run(splitEvery) == control, "Split pixels differ from unsplit control");
      std::printf("Actual Aurora split=%u iteration=%u matched native tiled readback\n", splitEvery, iteration);
    }
    for (unsigned iteration = 0; iteration < 9; ++iteration) {
      require(run(iteration % 3 + 1, true) == expected(4), "Async pixels differ from expected GX data");
      std::printf("Actual Aurora async iteration=%u matched native tiled readback\n", iteration);
    }
    for (unsigned splitEvery = 0; splitEvery < 4; ++splitEvery) {
      require(run(splitEvery, false, true) == expected(16), "Offscreen interlude changed the suspended EFB");
      std::printf("Actual Aurora offscreen split=%u preserved bake and suspended EFB\n", splitEvery);
    }
    for (unsigned splitEvery = 0; splitEvery < 4; ++splitEvery) {
      require(run(splitEvery, false, false, true) == expected(16), "GX FIFO quad pixels differ from expected output");
      std::printf("Actual Aurora GX FIFO split=%u preserved direct vertices, indices and uniforms\n", splitEvery);
    }
    for (unsigned splitEvery = 0; splitEvery < 4; ++splitEvery) {
      require(run(splitEvery, false, false, 2) == expected(16), "Invalidated GX array pixels differ from expected output");
      std::printf("Actual Aurora GX invalidation split=%u refreshed the same array address\n", splitEvery);
    }
    const gfx::StagingSizes physical{gfx::VertexBufferSize, gfx::UniformBufferSize,
                                      gfx::IndexBufferSize, gfx::StorageBufferSize};
    // The uniform tail the renderer keeps free (gfx::staging_has_space), and room for exactly two clear uniforms
    // after it, padded to this device's alignment (64 bytes on some GPUs, 256 on others): the third clear must split.
    const auto uniformTail = gx::MaxUniformSize + gfx::efb_ram::MaxAsyncReadbackSlots * gfx::staging_uniform_bytes(48);
    for (unsigned buffer = 0; buffer < 4; ++buffer) {
      auto limits = physical;
      limits[buffer] = buffer == 0   ? 128
                       : buffer == 1 ? uniformTail + 2 * gfx::staging_uniform_bytes(16)
                       : buffer == 2 ? 24
                                     : 2 * gfx::staging_storage_bytes(48);
      gfx::set_staging_capacity_limits_for_testing(limits);
      const auto before = gfx::staging_split_count();
      require(run(0, false, false, buffer == 1 ? 0 : buffer == 3 ? 2 : 1) == expected(16),
              "Automatic capacity split changed pixels");
      require(gfx::staging_split_count() > before, "Forced capacity did not split");
      const auto highWater = gfx::staging_high_water();
      for (unsigned i = 0; i < limits.size(); ++i)
        require(highWater[i] <= limits[i], "Actual staging usage exceeded admission budget");
      std::printf("Actual staging high-water V/U/I/S=%llu/%llu/%llu/%llu bytes\n",
          static_cast<unsigned long long>(highWater[0]), static_cast<unsigned long long>(highWater[1]),
          static_cast<unsigned long long>(highWater[2]), static_cast<unsigned long long>(highWater[3]));
      std::printf("Actual Aurora automatic capacity buffer=%u splits=%llu matched pixels\n", buffer,
                  static_cast<unsigned long long>(gfx::staging_split_count() - before));
    }
    auto limits = physical;
    limits[0] = 128;
    gfx::set_staging_capacity_limits_for_testing(limits);
    require(run(0, false, false, 3) == expected(16), "Raw bridge capacity split changed pixels");
    std::puts("Actual Aurora raw bridge capacity split preserved direct quad pixels");
    limits = physical;
    limits[1] = uniformTail + 3 * gfx::staging_uniform_bytes(16); // three clear uniforms at this device's alignment
    gfx::set_staging_capacity_limits_for_testing(limits);
    const auto beforeBake = gfx::staging_split_count();
    require(run(0, false, true, 0, true) == expected(16),
            "Automatic offscreen split changed bake or suspended EFB");
    require(gfx::staging_split_count() - beforeBake >= 9, "Offscreen test did not reuse all staging slots");
    std::printf("Actual Aurora automatic offscreen/readback splits=%llu preserved all pixels\n",
                static_cast<unsigned long long>(gfx::staging_split_count() - beforeBake));
    gfx::set_staging_capacity_limits_for_testing(physical);
    aurora_set_frame_interpolation_fps(120);
    for (unsigned frame = 0; frame < 3; ++frame)
      require(run(0, false, false, 2, false, true) == expected(16), "Perspective warmup changed pixels");
    AuroraFrameInterpolationDiagnostics interpolation{};
    gx::get_frame_interpolation_diagnostics(interpolation);
    require(interpolation.matchable > 0 && interpolation.activeSamples > 0,
            "Interpolation probe did not establish matching perspective draws");
    limits = physical;
    limits[3] = 2 * gfx::staging_storage_bytes(48);
    gfx::set_staging_capacity_limits_for_testing(limits);
    require(run(0, false, false, 2, false, true) == expected(16), "Interpolated split changed native pixels");
    gx::get_frame_interpolation_diagnostics(interpolation);
    require(!interpolation.replaySafe, "Split frame incorrectly retained interpolation replay");
    aurora_set_frame_interpolation_fps(0);
    std::puts("Actual Aurora matched perspective interpolation survived capacity split and disabled replay");
    limits = physical;
    limits[0] = 32; // One quad needs 64 bytes: typed rejection before any draw allocation.
    gfx::set_staging_capacity_limits_for_testing(limits);
    bool oversized = false;
    try { run(0, false, false, 1); }
    catch (const gfx::StagingCapacityError&) { oversized = true; }
    require(oversized, "Oversized primitive was not rejected");
    require(gfx::staging_usage() == gfx::StagingSizes{}, "Oversized primitive partially allocated");
    gx::fifo::clear_buffer();
    gfx::abort_frame();
    gfx::set_staging_capacity_limits_for_testing(physical);
    std::puts("Actual Aurora oversized primitive rejected before staging mutation");
    require(run(0, false, false, 1) == expected(16), "Renderer failed after rejected primitive cleanup");
    limits = physical;
    limits[3] = 2 * gfx::staging_storage_bytes(48);
    gfx::set_staging_capacity_limits_for_testing(limits);
    aurora_set_frame_interpolation_fps(120);
    for (unsigned frame = 0; frame < 16; ++frame)
      require(run(0, false, false, 2, false, true, true) == expected(16),
              "Frame-worker capacity split changed pixels");
    // Grant preparation of the next frame before joining DONE, exactly as the
    // real producer does; leave no worker waiting for a future begin_frame.
    require(aurora_begin_frame(), "Final worker frame preparation failed");
    aurora::wait_for_frame_worker();
    gfx::abort_frame();
    aurora_set_frame_interpolation_fps(0);
    gfx::set_staging_capacity_limits_for_testing(physical);
    std::puts("Actual Aurora frame worker completed 16 capacity-split perspective frames");
    check_hdr_present_shader();
    require(errors == 0, "Renderer reported an error");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    aurora_shutdown();
    return 4;
  }
  aurora_shutdown();
  if (errors != 0) return 4;
  std::puts("Actual Aurora clear/resolve/snapshot/downsample/readback batches passed");
}
