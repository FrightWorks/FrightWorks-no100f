#pragma once

#include "common.hpp"
#include "../webgpu/gpu.hpp"

#include <array>
#include <string>

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

// Debug view (5 = the relighting's light alone, 6 = the moon's shadow alone): 0 off, 1 tint (everything drawn before the stage is tinted blue; what is drawn after keeps its colour),
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
  // Relighting experiment: the world-to-view matrix (rows of a 3x4), the position matrix most of this frame's solid
  // perspective draws use (the level is drawn in world space with the camera's view matrix).
  std::array<float, 12> view{};
  bool hasView = false;
  // Moon shadows: view space to the moon's shadow map's clip space (rows of a 4x4), when the frame has them.
  std::array<float, 16> shadow{};
  bool hasShadow = false;
  // Reimagined sky: the file's moon texture ("moontex KEY"), found at the capture (texture packs can replace it).
  TextureHandle moonTexture;
};
void note_camera(const Mat4x4<float>& projection, const webgpu::Viewport& viewport) noexcept;
// A solid perspective draw's position matrix (12 floats, rows of a 3x4): a vote for the view matrix.
void note_world_matrix(const float* matrix) noexcept;
const Camera& camera() noexcept;

// Relighting experiment (NO100F_RELIGHT=<light file>): solid perspective draws lose their baked vertex colours
// (shader.cpp) and the stage lights the solid scene from the file's ambient, moon and point lights (world space).
bool relighting() noexcept;
// -1 = no light file, 0 = off, 1 = on. set_relight: 0 off, 1 on, 2 reload the light file (dev menu "relight").
int relight_state() noexcept;
void set_relight(int mode) noexcept;
// Changes with every set_relight: the pipeline state memo is redone (the relight bit is in the pipeline config).
unsigned relight_generation() noexcept;
// The light file's unlit textures ("unlit KEY", KEY as TextureRef::debugKey): drawn as the game draws them.
bool is_unlit_texture(const std::string& key) noexcept;
// The light file's hidden textures ("hide KEY"): draws that sample one are skipped (painted shadow decals).
bool is_hidden_texture(const std::string& key) noexcept;
// The file's opaque textures ("opaque KEY"): drawn without blending, so the game can't fade them.
bool is_opaque_texture(const std::string& key) noexcept;
// The file's "deblue" (0-255) for the relit draws' shader (ShaderConfig.deblue).
uint8_t relight_deblue() noexcept;

// Moon shadows: the solid scene is drawn a second time from the moon into a depth map, before the stage. Each solid
// draw gets a copy of its uniforms (shader_info.cpp build_uniform), whose projection is replaced, once the frame's
// camera is known (common.cpp, at the stage), by the camera's view space to the shadow map's clip space.
// note_shadow_uniform: the absolute offset of a copy's projection in the uniform staging buffer.
void note_shadow_uniform(size_t offset) noexcept;
// Writes Camera::shadow (or a matrix that draws nothing) into every copy noted this frame; `uniforms` = the staging.
void write_shadow_uniforms(uint8_t* uniforms) noexcept;
// The frame is submitted in parts: the copies' staging is gone.
void drop_shadow_uniforms() noexcept;
// The shadow map's targets for EFB pipelines with `samples` per pixel: a colour target and stand-ins for the motion
// targets (`motion`) that the pipelines write too, and the depth map itself. Square, `size` texels.
struct ShadowTargets {
  wgpu::TextureView color, depth, motion, motionDepth, mark;
  uint32_t size = 0;
};
const ShadowTargets& shadow_targets(uint32_t samples, bool motion);

// The light file's night sky ("sky", "moondisc", "stars"): drawn over the whole EFB at the start of the scene's pass
// (right after its clear, before the game draws anything), from the stage camera; the game's draws cover it.
// `motion`: the pass has the motion targets (left alone).
bool sky_wanted(const Camera& camera) noexcept;
void encode_sky(const wgpu::RenderPassEncoder& pass, const Camera& camera, wgpu::Extent3D size, uint32_t samples,
                bool motion);

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
