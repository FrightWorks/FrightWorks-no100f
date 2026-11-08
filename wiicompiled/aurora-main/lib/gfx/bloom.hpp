#pragma once

#include "common.hpp"
#include "texture.hpp"
#include "../webgpu/gpu.hpp"

#include <string>
#include <vector>

namespace aurora::gfx::bloom {
// Bloom: the game's lights (lit windows, fire, lamp light cones) glow softly around themselves.
//
// Nothing in how the game draws tells its lights apart: its lit windows are painted textures no brighter than its
// moonlit paths and stone, its fire and light cones are blended like its fog, and its chimney smoke adds to the picture
// the way light would. So brightness or blending would make the wrong things glow. What does mark them is their
// textures: the game hands over a list (set_light_textures), and the draws that sample one of them are the light. At
// the scene stage (scene_stage.hpp), common.cpp draws those draws once more, alone over black, into the bloom's target:
// the ones of the solid scene and those of the see-through tail that follows (the frame also splits after the tail,
// which marks where the tail ends), against the depth as it is at the stage (so the solid scene in front still hides
// them, but not the see-through surfaces the game draws later with depth writes, such as the faded walls between the
// camera and the room at the start of i004). That is blurred wide at falling sizes and added there, before the tail is
// drawn: see-through surfaces in front of a light (glass, mist, smoke) then cover its glow as they cover the light. A
// frame with no scene stage (the "game" and "cut" cut-out modes, a frame submitted in parts) has no bloom.

// 0 = off, 1 = full. Clamped to [0, 1]; takes effect at the next frame.
void set_strength(float strength) noexcept;
float strength() noexcept;

// Debug view: 0 off, 1 the glow alone (over black, at full strength), 2 its source (the light draws alone),
// both with the HUD on top.
void set_view(int mode) noexcept;
int view() noexcept;

// Whether the frame needs the scene stage and the point after the tail for the bloom.
bool wanted() noexcept;

// The game's light textures, by texture pack name without ".dds" ("tex1_64x64_0123456789abcdef_14", as
// TextureRef::debugKey), replacing the list. Takes effect at the next draw.
void set_light_textures(const std::vector<std::string>& keys) noexcept;
// Whether a texture is on the list (kept on the texture until the list changes).
bool is_light_texture(TextureRef& texture) noexcept;

// The target the light draws are drawn into alone, for an EFB of `size` with `samples` per pixel (its format): `view`
// to draw into, cleared to black; with more than one sample, `resolve` to resolve it into.
struct TailTarget {
  wgpu::TextureView view;
  wgpu::TextureView resolve;
};
TailTarget tail_target(wgpu::Extent3D size, uint32_t samples);
// Stand-ins for the motion targets (motion vectors, their depth, the HUD mark) when the frame has them: the light
// draws' pipelines write those too, and drawing them again must not change the real ones.
struct MotionScratch {
  wgpu::TextureView motion;
  wgpu::TextureView depth;
  wgpu::TextureView mark;
};
MotionScratch motion_scratch(wgpu::Extent3D size, uint32_t samples);
// At the scene stage: keeps a copy of the EFB's depth (`depth`, `samples` per pixel) to draw the lights against
// (stage_depth; empty before the first copy).
void encode_keep_depth(const wgpu::CommandEncoder& encoder, const wgpu::Texture& depth, uint32_t samples);
const wgpu::TextureView& stage_depth() noexcept;
// At the scene stage, after the light draws have been drawn into tail_target: the glow from them, added over `color`
// (the EFB, `samples` per pixel, `size`) inside `viewport` (the game's picture: letterbox bars stay black). With a
// debug view on, the glow is only made, for encode_view.
void encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, wgpu::Extent3D size, uint32_t samples,
            const webgpu::Viewport& viewport);
// After the see-through tail, with a debug view on: the view over `color` (as encode), so nothing the tail draws
// covers it.
void encode_view(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, wgpu::Extent3D size,
                 uint32_t samples, const webgpu::Viewport& viewport);

// Frees the targets when nothing uses them any more (run where encode runs). Commands already recorded keep their own
// references.
void release_unused_targets() noexcept;
bool holds_targets() noexcept;

// Releases the GPU objects (before the device goes).
void shutdown() noexcept;
} // namespace aurora::gfx::bloom
