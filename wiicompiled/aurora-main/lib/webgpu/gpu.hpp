#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>
#include <string>
#include <aurora/gfx.h>

#include "wgpu.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

namespace aurora::webgpu {
struct GraphicsConfig {
  wgpu::SurfaceConfiguration surfaceConfiguration;
  wgpu::TextureFormat depthFormat;
  uint32_t msaaSamples;
  uint16_t textureAnisotropy;
  uint32_t maxTextureDimension2D;
};
struct TextureWithSampler {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  wgpu::Sampler sampler;
};
struct PresentSource {
  wgpu::BindGroup bindGroup;
  wgpu::Texture texture;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  // The render-target (EFB) rectangle the picture was copied from, x, y, width, height in render pixels; zero width
  // = the render target itself, 1:1. Motion vectors map through it to the presented picture.
  std::array<float, 4> efbRect{};
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::BackendType g_backendType;
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
// The EFB's motion targets when gfx::motion_vectors_active() (else empty), render-target size and sample count (with
// MSAA they are multisampled like the EFB and never resolved: averaging the motion of two surfaces describes neither,
// so readers take sample 0).
// RG16F: per-pixel motion vectors.
extern TextureWithSampler g_motionVectors;
// R32F: the depth of the surface that owns each pixel's motion (raw, as the depth buffer holds it), which survives the
// game clearing the depth buffer before its HUD.
extern TextureWithSampler g_motionDepth;
// R8: 1 where the HUD is drawn: effects that use the motion (motion blur) leave those pixels alone.
extern TextureWithSampler g_motionMark;
enum class MotionTarget { Motion, Depth, Mark };
TextureWithSampler create_motion_target(MotionTarget target, uint32_t width, uint32_t height, uint32_t samples);
// The three EFB motion targets above, or none (enabled false).
void create_motion_targets(bool enabled);
// The device can blend 32-bit float targets (motion depth of alpha-blended surfaces needs it).
extern bool g_float32Blendable;
extern wgpu::RenderPipeline g_CopyPipeline;
extern wgpu::RenderPipeline g_PresentPipeline;
extern wgpu::BindGroup g_CopyBindGroup;
extern wgpu::BindGroup g_HdrCalibrationBindGroup;
extern wgpu::Instance g_instance;
extern bool g_bcTexturesSupported;
// Vertex fetch written for drivers that miscompile aurora's storage-buffer vertex pulling (Adreno:
// skinned models' texture coordinates and normals come out scrambled). See gx/shader.cpp.
extern bool g_safeVertexFetch;
// Shader diagnostics from AURORA_GFX_FLAGS (gx/shader.cpp apply_shader_debug).
enum ShaderDebug : uint32_t {
  ShaderDebugNoTextures = 1u << 0,
  ShaderDebugTexCoords = 1u << 1,
  ShaderDebugDumpSource = 1u << 2,
};
extern uint32_t g_shaderDebug;
// The adapter's device name, for AuroraInfo.gpuName.
const char* adapter_name() noexcept;

bool initialize(AuroraBackend backend);
void shutdown();
// Device-loss callbacks may run on a Dawn-managed thread and only publish loss state; frame
// threads call this at ordered boundaries to turn it into the one user-visible fatal error.
void fail_if_device_lost() noexcept;
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
// NSMBW: V-Sync (Fifo) on or off; the change is applied by take_vsync_change at a frame boundary.
// HDR output for the next device setup (startup only).
void request_hdr(bool enabled, float paperWhiteNits, float peakNits) noexcept;
bool set_hdr_brightness(float paperWhiteNits, float peakNits) noexcept;
bool hdr_active() noexcept;
// HDR was requested but startup found Windows HDR off for the game's display (so hdr_active() is false).
bool hdr_sdr_display() noexcept;
// The present shader: the shared copy shader plus, for HDR, the calibration uniform and the HDR fragment tail.
// Exposed so the renderer tests run the same text the game does.
const char* hdr_fragment_tail();
std::string copy_shader_source(const std::string& fragmentTail, bool calibration);
void set_hdr_calibration(const AuroraHdrCalibration* calibration) noexcept;
// Called under the presentation queue-submit lock, before submitting the HDR copy.
void upload_hdr_calibration();
void request_vsync(bool enabled) noexcept;
bool vsync_enabled() noexcept;
// Applies a pending V-Sync change to the surface configuration; true when the swapchain must be reconfigured.
bool take_vsync_change() noexcept;
bool vsync_change_pending() noexcept;
void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                      bool force = false);
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled);
const TextureWithSampler& present_source() noexcept;
PresentSource current_present_source() noexcept;
void set_present_source_override(wgpu::BindGroup bindGroup, wgpu::Texture texture, wgpu::Extent3D size,
                                 wgpu::TextureFormat format, std::array<float, 4> efbRect = {}) noexcept;
void clear_present_source_override() noexcept;
wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source);
wgpu::BindGroup create_copy_bind_group(wgpu::TextureView sourceView, wgpu::Sampler sampler);
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height,
                                               float content_aspect) noexcept;
void draw_clear(const wgpu::RenderPassEncoder& pass, bool clearColor, bool clearAlpha, bool clearDepth,
                const Vec4<float>& clearColorValue, float clearDepthValue);

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void* userdata);
void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void* userdata);
void cache_shutdown();

struct BlobCacheStats {
  uint64_t lookups;
  uint64_t hits;
  uint64_t stores;
  uint64_t hitBytes;
};
BlobCacheStats blob_cache_stats() noexcept;
// Persists the monolithic Vulkan pipeline cache into the blob cache (no-op elsewhere).
void serialize_pipeline_caches() noexcept;

} // namespace aurora::webgpu
