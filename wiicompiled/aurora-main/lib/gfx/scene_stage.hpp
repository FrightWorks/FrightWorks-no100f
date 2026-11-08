#pragma once

#include "common.hpp"
#include "../webgpu/gpu.hpp"

namespace aurora::gfx::scene_stage {
// The scene stage: a point in each frame where the game's solid scene is finished and its see-through world draws
// (the world's second, blended pass, fog, particles, glow) have not been drawn yet, nor its HUD. Effects that belong
// to the solid scene run there, on the EFB's colour, reading its depth: ambient occlusion, later lighting.
//
// The game never marks that point, so it is taken from the cut-out modes' tails: when they are flushed (common.cpp
// flush_deferred_draws, or at the next copy or the frame's end), the cut-out fringes and the solid surfaces' second
// layers stay in the pass, the pass ends, the stage runs, and the see-through draws start a new pass that keeps the
// colour and depth. When a frame flushes more than once, the stage runs at the last flush. A frame with nothing held
// back (cut-out mode "game" or "cut", or nothing see-through or cut out) has no stage, and neither has a frame submitted
// in parts (common.cpp end_batch_impl): it shows without the stage's effects, otherwise the same.

// Whether anything uses the stage (its own effects, or the bloom: bloom.hpp): with nothing, the pass is not split and
// nothing is recorded.
bool wanted() noexcept;

// Solid surfaces the game draws a second time, see-through: its two-layer materials (the f001 dock house: the walls
// drawn solid, then the same draws again blended, depth test only). The second layer belongs to the solid surface, so
// it is drawn before the stage. A surface is its geometry and position matrix; the set is per frame (recording thread).
void begin_frame() noexcept;
void note_solid_surface(HashType surface) noexcept;
bool is_solid_surface(HashType surface) noexcept;

// Ambient occlusion: how much the solid scene is darkened where surfaces crowd each other (creases, corners, under
// things), 0 = off, 1 = full. Clamped to [0, 1]; takes effect at the next frame.
void set_ambient_occlusion(float strength) noexcept;
float ambient_occlusion() noexcept;

// Cartoon ink outlines on the solid scene: 0 off, 1 thin (silhouettes), 2 bold (thicker, and sharp creases too).
// Takes effect at the next frame.
void set_ink_outlines(int level) noexcept;
int ink_outlines() noexcept;

// Debug view: 0 off, 1 tint (everything drawn before the stage is tinted blue; what is drawn after keeps its colour),
// 2 depth (the solid scene's distance from the camera, near = white), 3 the ambient occlusion alone (white = none),
// 4 the ink outlines alone (black on white; Bold when the setting is off), all with what is drawn after the stage on
// top.
void set_view(int mode) noexcept;
int view() noexcept;

// The solid scene's camera: the effective projection and the render viewport of the last perspective draw that wrote
// depth. The stage turns depth back into positions with it.
struct Camera {
  Mat4x4<float> projection{};
  webgpu::Viewport viewport{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
};
void note_camera(const Mat4x4<float>& projection, const webgpu::Viewport& viewport) noexcept;
const Camera& camera() noexcept;

// The stage itself, encoded between the two EFB passes: draws over `color` (the EFB, `samples` per pixel, `size`),
// reading `depth` (its depth buffer).
void encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, const wgpu::TextureView& depth,
            wgpu::Extent3D size, uint32_t samples, const Camera& camera);

// Frees the occlusion targets when nothing uses them any more (run where encode runs: it uses them there). Commands
// already recorded keep their own references.
void release_unused_targets() noexcept;
bool holds_targets() noexcept;

// Releases the stage's GPU objects (before the device goes).
void shutdown() noexcept;
} // namespace aurora::gfx::scene_stage
