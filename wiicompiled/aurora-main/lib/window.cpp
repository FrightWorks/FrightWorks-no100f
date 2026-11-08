#include "window.hpp"

#ifdef AURORA_ENABLE_GX
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#endif
#include "input.hpp"
#include "internal.hpp"
#include "monitor_labels.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/render_size_limits.hpp>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_timer.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(SDL_PLATFORM_ANDROID)
#include <jni.h>
extern "C" void Android_LockActivityMutex(void);
extern "C" void Android_UnlockActivityMutex(void);
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "dolphin/vi/vi_internal.hpp"
#include <aurora/window_owner_sdl.hpp>

namespace aurora::window {
int hdr_display_state() {
  if (window_owner::enabled() && !window_owner::is_owner())
    return window_owner::call([] { return hdr_display_state(); });
#if defined(_WIN32)
  const auto display = SDL_GetDisplayForWindow(get_sdl_window());
  static SDL_DisplayID cachedDisplay = 0;
  static Uint64 checkedAt = 0;
  static int cachedState = -1;
  const auto now = SDL_GetTicks();
  if (display == cachedDisplay && checkedAt != 0 && now - checkedAt < 500) return cachedState;
  cachedDisplay = display;
  checkedAt = now;
  cachedState = -1;
  const auto monitor = static_cast<HMONITOR>(SDL_GetPointerProperty(SDL_GetDisplayProperties(display),
      SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, nullptr));
  MONITORINFOEXW monitorInfo{};
  monitorInfo.cbSize = sizeof(monitorInfo);
  if (!monitor || !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&monitorInfo))) return cachedState;
  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) break;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    const auto result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (result == ERROR_INSUFFICIENT_BUFFER) continue;
    if (result != ERROR_SUCCESS) break;
    const DISPLAYCONFIG_PATH_INFO* match = nullptr;
    for (UINT32 i = 0; i < pathCount; ++i) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[i].sourceInfo.adapterId,
                       paths[i].sourceInfo.id};
      if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) return cachedState;
      if (wcscmp(source.viewGdiDeviceName, monitorInfo.szDevice) != 0) continue;
      if (match != nullptr) return cachedState; // Mirrored outputs are ambiguous.
      match = &paths[i];
    }
    if (match == nullptr) break;
    // Local layout also builds against SDKs predating Windows 11's active-color-mode query.
    struct AdvancedColor2 {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      UINT32 flags, encoding, bitsPerChannel, activeMode;
    } color{};
    color.header = {static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15), sizeof(color),
                    match->targetInfo.adapterId, match->targetInfo.id};
    auto error = DisplayConfigGetDeviceInfo(&color.header);
    if (error == ERROR_SUCCESS) {
      cachedState = (color.flags & 2) != 0 && color.activeMode == 2 ? 1 : 0;
    } else if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED) {
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy{};
      legacy.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(legacy),
                       match->targetInfo.adapterId, match->targetInfo.id};
      if (DisplayConfigGetDeviceInfo(&legacy.header) == ERROR_SUCCESS) {
        if (!legacy.advancedColorEnabled) cachedState = 0;
        // SDL confirms PQ output with headroom. Otherwise legacy ACM/HDR is ambiguous.
        else if (SDL_GetBooleanProperty(SDL_GetWindowProperties(get_sdl_window()),
                     SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)) cachedState = 1;
      }
    }
    break;
  }
  return cachedState;
#else
  return -1;
#endif
}

std::string hdr_display_name() {
  if (window_owner::enabled() && !window_owner::is_owner())
    return window_owner::call([] { return hdr_display_name(); });
  const char* name = SDL_GetDisplayName(SDL_GetDisplayForWindow(get_sdl_window()));
  return name != nullptr ? name : "Unknown display";
}

namespace {
Module Log("aurora::window");

SDL_Window* g_window;
SDL_Renderer* g_renderer;
float g_frameBufferScale = 0.f;
// Render height in lines (VISetFrameBufferLines); 0 leaves the size to g_frameBufferScale.
float g_frameBufferLines = 0.f;
bool g_frameBufferAspectFit = true;
std::atomic<float> g_fixedAspect{0.f};  // see set_fixed_aspect
bool g_presentSurfaceFill = false;
int g_presentAspectWidth = 0;
int g_presentAspectHeight = 0;
AuroraWindowSize g_windowSize;
std::vector<AuroraEvent> g_events;
std::atomic_bool g_backgrounded = false;
std::atomic_bool g_nativeResizePending = false;
std::atomic<AuroraDisplayMode> g_displayMode{AURORA_DISPLAY_MODE_WINDOWED};
// Window owner thread only. The monitor id for fullscreen; empty or "auto" = the monitor the window is on.
std::string g_fullscreenMonitor;
// Where the window was before borderless fullscreen moved it to that monitor: put back on leaving fullscreen, so the
// player's window spot (window_x/y) is not overwritten.
std::optional<std::pair<int, int>> g_windowPosBeforeFullscreen;
#if defined(SDL_PLATFORM_ANDROID)
std::atomic_bool g_surfaceReady = false;
#else
std::atomic_bool g_surfaceReady = true;
#endif
bool g_lastPaused = false;
bool g_gotFocus = false;
#if defined(_WIN32)
WNDPROC g_prevWindowProc = nullptr;
// Native window handle captured once when the hook is installed. The presenter checks the native
// size several times per slot, which used to be an SDL lookup for a handle that never changes.
HWND g_nativeWindowHandle = nullptr;
// Packed native client size ((width << 32) | height) kept by the window procedure; zero means
// unknown. Atomic because that procedure runs on the SDL thread while the presenter reads it.
std::atomic<uint64_t> g_nativeClientSize{0};

constexpr uint64_t pack_native_size(uint32_t width, uint32_t height) noexcept {
  return (static_cast<uint64_t>(width) << 32) | static_cast<uint64_t>(height);
}

LRESULT CALLBACK aurora_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  bool sizeChanged = msg == WM_SIZE;
  if (msg == WM_WINDOWPOSCHANGED) {
    const auto* pos = reinterpret_cast<const WINDOWPOS*>(lparam);
    sizeChanged = pos != nullptr && (pos->flags & SWP_NOSIZE) == 0;
  }
  if (sizeChanged) {
    g_nativeResizePending.store(true, std::memory_order_release);
    if (msg == WM_SIZE) {
      // WM_SIZE carries the new client size directly.
      g_nativeClientSize.store(pack_native_size(static_cast<uint32_t>(LOWORD(lparam)),
                                                static_cast<uint32_t>(HIWORD(lparam))),
                               std::memory_order_release);
    } else {
      // WM_WINDOWPOSCHANGED carries no client size. Invalidate before forwarding: DefWindowProc turns
      // it into a nested WM_SIZE through this procedure, which refills the cache authoritatively.
      g_nativeClientSize.store(0, std::memory_order_release);
    }
  }
  if (g_prevWindowProc == nullptr) {
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }
  const auto result = CallWindowProc(g_prevWindowProc, hwnd, msg, wparam, lparam);
  // SDL_PumpEvents stays inside Windows' move/size and menu loops. Publish after SDL has
  // handled each native message, not only after the outer pump eventually returns.
  if (sizeChanged || msg == WM_MOVE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
      msg == WM_SHOWWINDOW || msg == WM_DPICHANGED) {
    window_owner::refresh_window_state();
  }
  return result;
}

void install_window_proc_hook() noexcept {
  const auto props = SDL_GetWindowProperties(g_window);
  auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (hwnd == nullptr) {
    return;
  }
  g_nativeWindowHandle = hwnd;
  g_nativeClientSize.store(0, std::memory_order_release);
  g_prevWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                                                                reinterpret_cast<LONG_PTR>(aurora_window_proc)));
}

void uninstall_window_proc_hook() noexcept {
  if (g_window == nullptr || g_prevWindowProc == nullptr) {
    g_nativeWindowHandle = nullptr;
    g_nativeClientSize.store(0, std::memory_order_release);
    return;
  }
  const auto props = SDL_GetWindowProperties(g_window);
  auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (hwnd != nullptr) {
    SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevWindowProc));
  }
  g_prevWindowProc = nullptr;
  g_nativeWindowHandle = nullptr;
  g_nativeClientSize.store(0, std::memory_order_release);
}
#endif

// Native client size of the window without going through SDL. Returns false when the platform has
// no cheap native query, in which case callers fall back to get_window_size().
bool query_native_client_size(uint32_t& width, uint32_t& height) noexcept {
#if defined(_WIN32)
  const uint64_t cached = g_nativeClientSize.load(std::memory_order_acquire);
  if (cached != 0) {
    width = static_cast<uint32_t>(cached >> 32);
    height = static_cast<uint32_t>(cached & 0xffffffffull);
    return true;
  }
  if (g_nativeWindowHandle != nullptr) {
    RECT client{};
    if (GetClientRect(g_nativeWindowHandle, &client) != 0) {
      width = static_cast<uint32_t>(std::max<LONG>(client.right - client.left, 0));
      height = static_cast<uint32_t>(std::max<LONG>(client.bottom - client.top, 0));
      if (width != 0 && height != 0) {
        g_nativeClientSize.store(pack_native_size(width, height), std::memory_order_release);
      }
      return true;
    }
  }
#else
  (void)width;
  (void)height;
#endif
  return false;
}

bool operator==(const AuroraWindowSize& lhs, const AuroraWindowSize& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fb_width == rhs.fb_width &&
         lhs.fb_height == rhs.fb_height && lhs.native_fb_height == rhs.native_fb_height &&
         lhs.native_fb_width == rhs.native_fb_width && lhs.scale == rhs.scale;
}

Vec2<int> scale_frame_buffer_to_aspect(int w, int h, float scale, float aspect) {
  if (w <= 0 || h <= 0) {
    return {std::max(w, 1), std::max(h, 1)};
  }
  const auto size = render_size_limits::scale_framebuffer_to_aspect(
      static_cast<uint32_t>(w), static_cast<uint32_t>(h), scale, aspect);
  return {static_cast<int>(size.width), static_cast<int>(size.height)};
}

void resize_swapchain() noexcept {
#if defined(SDL_PLATFORM_ANDROID)
  SurfaceLock surfaceLock;
#endif
  // Consume only requests that preceded this synchronization; a native resize arriving mid-rebuild
  // stays flagged for the next ordered frame boundary.
  g_nativeResizePending.exchange(false, std::memory_order_acq_rel);
  const auto size = get_window_size();
  if (size == g_windowSize) {
    return;
  }
  if (size.scale != g_windowSize.scale) {
    if (g_windowSize.scale > 0.f) {
      Log.info("Display scale changed to {}", size.scale);
    }
  }
  g_windowSize = size;
  if (g_renderer != nullptr) {
    SDL_SetRenderLogicalPresentation(g_renderer, static_cast<int>(size.native_fb_width),
                                     static_cast<int>(size.native_fb_height), SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderScale(g_renderer, size.scale, size.scale);
  }
#ifdef AURORA_ENABLE_GX
  webgpu::resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height);
  if (g_onRenderTargetResized != nullptr) {
    g_onRenderTargetResized();  // GX re-maps its viewport/scissor to the new target scale (gx.cpp)
  }
#endif
}

void set_window_icon() noexcept {
  if (g_config.iconRGBA8 == nullptr) {
    return;
  }
  auto* iconSurface =
      SDL_CreateSurfaceFrom(static_cast<int>(g_config.iconWidth), static_cast<int>(g_config.iconHeight),
                            SDL_GetPixelFormatForMasks(32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000),
                            g_config.iconRGBA8, static_cast<int>(4 * g_config.iconWidth));
  ASSERT(iconSurface != nullptr, "Failed to create icon surface: {}", SDL_GetError());
  TRY_WARN(SDL_SetWindowIcon(g_window, iconSurface), "Failed to set window icon: {}", SDL_GetError());
  SDL_DestroySurface(iconSurface);
}

bool SDLCALL lifecycle_event_watch(void*, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_WINDOW_RESIZED:
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
  case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    g_nativeResizePending.store(true, std::memory_order_release);
    break;
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_APPLE)
  case SDL_EVENT_WINDOW_MINIMIZED:
    g_backgrounded.store(true, std::memory_order_relaxed);
    break;
  case SDL_EVENT_WINDOW_RESTORED:
    g_backgrounded.store(false, std::memory_order_relaxed);
    break;
  // A phone app is killed, never shut down, so the pipelines compiled while playing are
  // saved for the next launch when it leaves the screen (Dawn's device is internally locked).
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
  case SDL_EVENT_TERMINATING:
    webgpu::serialize_pipeline_caches();
    break;
#endif
  default:
    break;
  }
  return true;
}

void sync_paused() {
  const bool paused = is_paused();
  if (g_lastPaused == paused) {
    return;
  }
  g_lastPaused = paused;
  g_events.push_back(AuroraEvent{
      .type = paused ? AURORA_PAUSED : AURORA_UNPAUSED,
  });
}

bool is_alt_enter_event(const SDL_Event& event) noexcept {
  if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
    return false;
  }

  const SDL_Keymod altMask = static_cast<SDL_Keymod>(SDL_KMOD_LALT | SDL_KMOD_RALT | SDL_KMOD_ALT);
  return event.key.key == SDLK_RETURN && (event.key.mod & altMask) != 0;
}

void process_event(SDL_Event& event) {
#ifdef AURORA_ENABLE_GX
  imgui::process_event(event);
#endif

  switch (event.type) {
  case SDL_EVENT_KEY_DOWN:
    if (is_alt_enter_event(event)) {
      set_display_mode(get_fullscreen() ? AURORA_DISPLAY_MODE_WINDOWED : AURORA_DISPLAY_MODE_BORDERLESS);
      g_nativeResizePending.store(true, std::memory_order_release);
    }
    break;
  case SDL_EVENT_WINDOW_MOVED: {
    g_events.push_back(AuroraEvent{
        .type = AURORA_WINDOW_MOVED,
        .windowPos = {.x = event.window.data1, .y = event.window.data2},
    });
    break;
  }
  case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
    g_events.push_back(AuroraEvent{
        .type = AURORA_DISPLAY_SCALE_CHANGED,
        .windowSize = get_window_size(),
    });
    break;
  }
  case SDL_EVENT_WINDOW_RESIZED:
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
    g_events.push_back(AuroraEvent{
        .type = AURORA_WINDOW_RESIZED,
        .windowSize = get_window_size(),
    });
    break;
  }
  case SDL_EVENT_GAMEPAD_ADDED: {
    auto instance = input::add_controller(event.gdevice.which);
    g_events.push_back(AuroraEvent{
        .type = AURORA_CONTROLLER_ADDED,
        .controller = instance,
    });
    break;
  }
  case SDL_EVENT_GAMEPAD_REMAPPED: {
    if (input::refresh_controller(event.gdevice.which)) {
      g_events.push_back(AuroraEvent{
          .type = AURORA_CONTROLLER_ADDED,
          .controller = event.gdevice.which,
      });
    }
    break;
  }
  case SDL_EVENT_GAMEPAD_REMOVED: {
    input::remove_controller(event.gdevice.which);
    g_events.push_back(AuroraEvent{
        .type = AURORA_CONTROLLER_REMOVED,
        .controller = event.gdevice.which,
    });
    break;
  }
  case SDL_EVENT_MOUSE_WHEEL:
    input::set_mouse_scroll(event.wheel.x, event.wheel.y);
    break;
  case SDL_EVENT_QUIT:
    g_events.push_back(AuroraEvent{
        .type = AURORA_EXIT,
    });
    break;
  default:
    if (event.type == g_sdlCustomEventsStart + CustomEvent::FutureResize) {
      // The request flag is set before this wakeup is queued. Renderer resource mutation belongs to
      // begin/end_frame_impl, which may be running on the frame worker.
      g_events.push_back(AuroraEvent{
          .type = AURORA_WINDOW_RESIZED,
          .windowSize = get_window_size(),
      });
    } else if (event.type == g_sdlCustomEventsStart + CustomEvent::RefreshSurface) {
      // This event only wakes the window loop; the frame worker applies the pending surface
      // configuration at its next ordered frame boundary.
    }
    break;
  }

  sync_paused();
  g_events.push_back(AuroraEvent{
      .type = AURORA_SDL_EVENT,
      .sdl = event,
  });
}
} // namespace

const AuroraEvent* poll_events() {
  g_events.clear();
  SDL_Event event;
  // Clear out the previous scroll values to prevent ghost input
  input::set_mouse_scroll(0, 0);
  if (is_paused()) {
    if (SDL_WaitEvent(&event)) {
      process_event(event);
    } else {
      Log.warn("SDL_WaitEvent failed: {}", SDL_GetError());
    }
  }
  while (SDL_PollEvent(&event)) {
    process_event(event);
  }
  g_events.push_back(AuroraEvent{
      .type = AURORA_NONE,
  });
  return g_events.data();
}

static SDL_DisplayID fullscreen_target_display();

bool create_window(AuroraBackend backend) {
  if (window_owner::enabled() && !window_owner::is_owner())
    return window_owner::call([=] { return create_window(backend); });
  if (window_owner::enabled() && (backend == BACKEND_OPENGL || backend == BACKEND_OPENGLES))
    throw std::runtime_error("Window owner does not support SDL OpenGL contexts");
  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if TARGET_OS_IOS || TARGET_OS_TV
  flags |= SDL_WINDOW_FULLSCREEN;
#else
  flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
  if (g_config.startFullscreen) {
    flags |= SDL_WINDOW_FULLSCREEN;
  }
#endif
  switch (backend) {
#ifdef AURORA_ENABLE_GX
#ifdef DAWN_ENABLE_BACKEND_VULKAN
  case BACKEND_VULKAN:
    flags |= SDL_WINDOW_VULKAN;
    break;
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
  case BACKEND_METAL:
    flags |= SDL_WINDOW_METAL;
    break;
#endif
#ifdef DAWN_ENABLE_BACKEND_OPENGL
  case BACKEND_OPENGL:
  case BACKEND_OPENGLES:
    flags |= SDL_WINDOW_OPENGL;
    break;
#endif
#endif
  default:
    break;
  }
  auto width = static_cast<Sint32>(g_config.windowWidth);
  auto height = static_cast<Sint32>(g_config.windowHeight);
  if (width == 0 || height == 0) {
    width = 1280;
    height = 960;
  }
  if (width < 640) {
    width = 640;
  }
  if (height < 480) {
    height = 480;
  }

  Sint32 posX = g_config.hasWindowPosition ? g_config.windowPosX : SDL_WINDOWPOS_CENTERED;
  Sint32 posY = g_config.hasWindowPosition ? g_config.windowPosY : SDL_WINDOWPOS_CENTERED;
  // Starting fullscreen on a chosen monitor: open there, so the window never shows on another monitor on its way.
  if (const SDL_DisplayID target = fullscreen_target_display(); target != 0) {
    // Without a saved spot the window's home is the centre of the main monitor, as when it opens in a window.
    const Sint32 mainCentered = SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetPrimaryDisplay());
    g_windowPosBeforeFullscreen = {g_config.hasWindowPosition ? posX : mainCentered,
                                   g_config.hasWindowPosition ? posY : mainCentered};
    posX = SDL_WINDOWPOS_CENTERED_DISPLAY(target);
    posY = SDL_WINDOWPOS_CENTERED_DISPLAY(target);
  }

  const auto props = SDL_CreateProperties();
  TRY(SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, g_config.appName), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_TITLE_STRING, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, posX), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, posY), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_GetError());
  g_window = SDL_CreateWindowWithProperties(props);
  if (g_window == nullptr) {
    Log.error("Failed to create window: {}", SDL_GetError());
    return false;
  }
  SDL_SetWindowMinimumSize(g_window, 640, 480);
  g_displayMode.store(g_config.startFullscreen ? AURORA_DISPLAY_MODE_BORDERLESS : AURORA_DISPLAY_MODE_WINDOWED,
                      std::memory_order_release);
#if defined(_WIN32)
  install_window_proc_hook();
#endif
  set_window_icon();
  return true;
}

bool create_renderer() {
  if (window_owner::enabled())
    throw std::runtime_error("Window owner requires WebGPU, not the SDL renderer fallback");
  if (g_window == nullptr) {
    return false;
  }
  const auto props = SDL_CreateProperties();
  TRY(SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, g_window), "Failed to set {}: {}",
      SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, SDL_GetError());
  // Never vsync: presentation must not throttle the guest's own pacing.
  TRY(SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, SDL_RENDERER_VSYNC_DISABLED),
      "Failed to set {}: {}", SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, SDL_GetError());
  g_renderer = SDL_CreateRendererWithProperties(props);
  if (g_renderer == nullptr) {
    Log.error("Failed to create renderer: {}", SDL_GetError());
    return false;
  }
  return true;
}

void destroy_window() {
  if (window_owner::enabled() && !window_owner::is_owner()) {
    window_owner::call([] { destroy_window(); });
    return;
  }
#if defined(_WIN32)
  uninstall_window_proc_hook();
#endif
  if (g_renderer != nullptr) {
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
  }
  if (g_window != nullptr) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
}

void show_window() {
  if (g_window != nullptr) {
    TRY_WARN(SDL_ShowWindow(g_window), "Failed to show window: {}", SDL_GetError());
  }
}

bool initialize() {
  if (window_owner::enabled() && !window_owner::is_owner())
    return window_owner::call([] { return initialize(); });
  /* We don't want to initialize anything input related here, otherwise the add events will get lost to the void */
  TRY(SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight"), "Error setting {}: {}", SDL_HINT_ORIENTATIONS,
      SDL_GetError());
  TRY(SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_VIDEO), "Error initializing SDL: {}", SDL_GetError());

#if !defined(_WIN32) && !defined(__APPLE__)
  TRY(SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0"), "Error setting {}: {}",
      SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, SDL_GetError());
#endif
  TRY(SDL_SetHint(SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, g_config.appName), "Error setting {}: {}",
      SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, SDL_GetError());
  TRY(SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE_RUMBLE_BRAKE, "1"), "Error setting {}: {}",
      SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE_RUMBLE_BRAKE, SDL_GetError());

  TRY(SDL_DisableScreenSaver(), "Error disabling screensaver: {}", SDL_GetError());
  if (g_config.allowJoystickBackgroundEvents) {
    TRY(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"), "Error setting {}: {}",
        SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_GetError());
  }

  return true;
}

bool initialize_event_watch() {
  if (window_owner::enabled() && !window_owner::is_owner())
    return window_owner::call([] { return initialize_event_watch(); });
  TRY(SDL_AddEventWatch(lifecycle_event_watch, nullptr), "Error adding SDL event watch: {}", SDL_GetError());
  return true;
}

void shutdown() {
  if (window_owner::enabled() && !window_owner::is_owner()) {
    window_owner::call([] { shutdown(); });
    return;
  }
  SDL_RemoveEventWatch(lifecycle_event_watch, nullptr);
  destroy_window();
  TRY_WARN(SDL_EnableScreenSaver(), "Error enabling screensaver: {}", SDL_GetError());
  if (!window_owner::enabled()) SDL_Quit();
}

AuroraWindowSize get_window_size() {
  int width = 0;
  int height = 0;
  int native_fb_w = 0;
  int native_fb_h = 0;
  float scale = 1.f;
  if (window_owner::enabled() && !window_owner::is_owner()) {
    const auto state = window_owner::native_state();
    width = state.width;
    height = state.height;
    native_fb_w = state.pixelsWidth;
    native_fb_h = state.pixelsHeight;
    scale = state.scale;
  } else {
    ASSERT(SDL_GetWindowSize(g_window, &width, &height), "Failed to get window size: {}", SDL_GetError());
    ASSERT(SDL_GetWindowSizeInPixels(g_window, &native_fb_w, &native_fb_h), "Failed to get window size in pixels: {}",
           SDL_GetError());
    scale = SDL_GetWindowDisplayScale(g_window);
  }

  int fb_w = native_fb_w;
  int fb_h = native_fb_h;
  const auto [baseW, baseH] = vi::configured_fb_size();
  // A line target (VISetFrameBufferLines) sizes the picture the game shows: its visible EFB lines
  // become exactly that many rows (the EFB workspace below them scales along), and with 16:9 or
  // the surface's aspect the width is the line count times that aspect, so the pixels are square.
  const uint32_t visibleH = vi::visible_fb_size().y;
  const bool lineTarget = g_frameBufferLines > 0.f && baseH > 0 && visibleH > 0;
  const float frameBufferScale =
      lineTarget ? g_frameBufferLines / static_cast<float>(visibleH) : g_frameBufferScale;
  const auto lineTargetSize = [&](float aspect) {
    return std::pair{
        std::max(1, static_cast<int>(std::lround(g_frameBufferLines *
                                                 render_size_limits::clamp_dynamic_aspect(aspect)))),
        std::max(1, static_cast<int>(std::lround(static_cast<float>(baseH) * frameBufferScale))),
    };
  };
  const float fixedAspect = g_fixedAspect.load(std::memory_order_acquire);
  if (fixedAspect > 0.f && native_fb_w > 0 && native_fb_h > 0) {
    if (lineTarget) {
      std::tie(fb_w, fb_h) = lineTargetSize(fixedAspect);
    } else if (frameBufferScale > 0.f && baseW > 0 && baseH > 0) {
      const auto [scaledW, scaledH] =
          scale_frame_buffer_to_aspect(static_cast<int>(baseW), static_cast<int>(baseH),
                                       frameBufferScale, fixedAspect);
      fb_w = scaledW;
      fb_h = scaledH;
    } else {
      fb_w = std::min(native_fb_w,
                      std::max(1, static_cast<int>(std::lround(native_fb_h * fixedAspect))));
      fb_h = std::min(native_fb_h,
                      std::max(1, static_cast<int>(std::lround(native_fb_w / fixedAspect))));
    }
  } else if (g_frameBufferAspectFit && baseW > 0 && baseH > 0) {
    float renderScale = frameBufferScale > 0.f ? frameBufferScale : 1.f;
    if (frameBufferScale <= 0.f) {
      renderScale = std::min(static_cast<float>(native_fb_w) / static_cast<float>(baseW),
                             static_cast<float>(native_fb_h) / static_cast<float>(baseH));
      if (renderScale < 1.f) {
        renderScale = 1.f;
      }
    }
    fb_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(baseW) * renderScale)));
    fb_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(baseH) * renderScale)));
  } else if (lineTarget && native_fb_w > 0 && native_fb_h > 0) {
    std::tie(fb_w, fb_h) =
        lineTargetSize(static_cast<float>(native_fb_w) / static_cast<float>(native_fb_h));
  } else if (frameBufferScale > 0.f && baseW > 0 && baseH > 0) {
    const auto [scaledW, scaledH] =
        scale_frame_buffer_to_aspect(static_cast<int>(baseW), static_cast<int>(baseH), frameBufferScale,
                                     static_cast<float>(fb_w) / static_cast<float>(fb_h));
    fb_w = scaledW;
    fb_h = scaledH;
  }

  return {
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .fb_width = static_cast<uint32_t>(fb_w),
      .fb_height = static_cast<uint32_t>(fb_h),
      .native_fb_width = static_cast<uint32_t>(native_fb_w),
      .native_fb_height = static_cast<uint32_t>(native_fb_h),
      .scale = scale,
  };
}

SDL_Window* get_sdl_window() { return g_window; }

SDL_Renderer* get_sdl_renderer() { return g_renderer; }

bool is_paused() noexcept {
  if (!is_presentable()) {
    return true;
  }
  const auto flags = SDL_GetWindowFlags(g_window);
  if ((flags & SDL_WINDOW_HIDDEN) != 0u) {
    return true;
  }
  // Wait until the window has received focus before respecting pauseOnFocusLost
  if (!g_gotFocus) {
    g_gotFocus = (flags & SDL_WINDOW_INPUT_FOCUS) != 0u;
    return false;
  }
  return g_config.pauseOnFocusLost && ((flags & SDL_WINDOW_INPUT_FOCUS) == 0u || (flags & SDL_WINDOW_MINIMIZED) != 0u);
}

bool is_presentable() noexcept {
  return g_window != nullptr && !g_backgrounded.load(std::memory_order_acquire) &&
         g_surfaceReady.load(std::memory_order_acquire);
}

void pump_events() {
  if (g_window != nullptr) {
    SDL_SyncWindow(g_window);
  }
  SDL_PumpEvents();
}

bool native_resize_pending() noexcept { return g_nativeResizePending.load(std::memory_order_acquire); }

bool native_window_size_matches(uint32_t width, uint32_t height) noexcept {
  if (g_window == nullptr || width == 0 || height == 0) {
    return true;
  }
  uint32_t nativeWidth = 0;
  uint32_t nativeHeight = 0;
  if (query_native_client_size(nativeWidth, nativeHeight)) {
    return nativeWidth == width && nativeHeight == height;
  }
  const auto size = get_window_size();
  return size.native_fb_width == width && size.native_fb_height == height;
}

void set_surface_ready(bool ready) noexcept { g_surfaceReady.store(ready, std::memory_order_release); }

SurfaceLock::SurfaceLock() noexcept {
#if defined(SDL_PLATFORM_ANDROID)
  Android_LockActivityMutex();
#endif
}

SurfaceLock::~SurfaceLock() {
#if defined(SDL_PLATFORM_ANDROID)
  Android_UnlockActivityMutex();
#endif
}

bool push_custom_event(CustomEvent eventType) {
  SDL_Event event{.type = g_sdlCustomEventsStart + eventType};
  return SDL_PushEvent(&event);
}

void set_title(const char* title) {
  TRY_WARN(SDL_SetWindowTitle(g_window, title), "Failed to set window title: {}", SDL_GetError());
}

void set_fullscreen(bool fullscreen) {
  set_display_mode(fullscreen ? AURORA_DISPLAY_MODE_BORDERLESS : AURORA_DISPLAY_MODE_WINDOWED);
}

bool get_fullscreen() { return (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0u; }

#if defined(_WIN32)
namespace {
struct NativeMonitor {
  std::string id;
  std::string name;
  SDL_DisplayID display = 0;
  RECT bounds{};
  bool primary = false;
};

std::string utf8_from_wide(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
  return result;
}

// "\\?\DISPLAY#ACI2498#5&2d9ad897&0&UID4353#{guid}" -> "ACI2498/5&2d9ad897&0&UID4353": the model, then the instance
// (tells identical monitors apart). The launcher makes the same ids (Monitors.cs); it can't read a '#' in a config value.
std::string monitor_id_from_path(const std::string& path) {
  const size_t model = path.find('#');
  const size_t instance = model == std::string::npos ? model : path.find('#', model + 1);
  const size_t end = instance == std::string::npos ? instance : path.find('#', instance + 1);
  if (end == std::string::npos) {
    return {};
  }
  return path.substr(model + 1, instance - model - 1) + '/' + path.substr(instance + 1, end - instance - 1);
}

std::vector<NativeMonitor> query_monitors() {
  std::vector<NativeMonitor> monitors;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  LONG result = ERROR_SUCCESS;
  do {
    result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS) {
      return monitors;
    }
    paths.resize(pathCount);
    modes.resize(modeCount);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
  } while (result == ERROR_INSUFFICIENT_BUFFER);
  if (result != ERROR_SUCCESS) {
    return monitors;
  }

  int displayCount = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
  for (int i = 0; displays != nullptr && i < displayCount; ++i) {
    const auto monitorHandle = static_cast<HMONITOR>(SDL_GetPointerProperty(
        SDL_GetDisplayProperties(displays[i]), SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, nullptr));
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitorHandle == nullptr || !GetMonitorInfoW(monitorHandle, &info)) {
      continue;
    }
    for (UINT32 p = 0; p < pathCount; ++p) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source.header.size = sizeof(source);
      source.header.adapterId = paths[p].sourceInfo.adapterId;
      source.header.id = paths[p].sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
          wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) {
        continue;
      }
      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target.header.size = sizeof(target);
      target.header.adapterId = paths[p].targetInfo.adapterId;
      target.header.id = paths[p].targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
        std::string id = monitor_id_from_path(utf8_from_wide(target.monitorDevicePath));
        if (!id.empty()) {
          std::string name = utf8_from_wide(target.monitorFriendlyDeviceName);
          monitors.push_back({.id = std::move(id),
                              .name = name.empty() ? "Monitor" : std::move(name),
                              .display = displays[i],
                              .bounds = info.rcMonitor,
                              .primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        }
      }
      break;  // mirrored monitors share one desktop: the first stands for them
    }
  }
  SDL_free(displays);
  return monitors;
}

// What a saved id means now: that monitor, else the only connected one of its model (plugged into another port).
const NativeMonitor* find_monitor(const std::vector<NativeMonitor>& monitors, std::string_view id) {
  for (const auto& monitor : monitors) {
    if (monitor.id == id) {
      return &monitor;
    }
  }
  const size_t modelEnd = id.find('/');
  if (modelEnd == std::string_view::npos) {
    return nullptr;
  }
  const std::string_view model = id.substr(0, modelEnd + 1);
  const NativeMonitor* found = nullptr;
  for (const auto& monitor : monitors) {
    if (monitor.id.starts_with(model)) {
      if (found != nullptr) {
        return nullptr;
      }
      found = &monitor;
    }
  }
  return found;
}
} // namespace
#endif

// The display fullscreen must go to; 0 = wherever the window is.
static SDL_DisplayID fullscreen_target_display() {
  if (g_fullscreenMonitor.empty() || g_fullscreenMonitor == "auto") {
    return 0;
  }
#if defined(_WIN32)
  const auto monitors = query_monitors();
  if (const NativeMonitor* monitor = find_monitor(monitors, g_fullscreenMonitor)) {
    return monitor->display;
  }
  Log.warn("Fullscreen monitor {} is not connected: using the main monitor", g_fullscreenMonitor);
  return SDL_GetPrimaryDisplay();
#else
  return 0;
#endif
}

void set_display_mode(AuroraDisplayMode mode) {
  if (window_owner::enabled() && !window_owner::is_owner()) {
    window_owner::submit([=] { set_display_mode(mode); }, 20);
    return;
  }
  if (g_window == nullptr) {
    return;
  }
  if (mode < AURORA_DISPLAY_MODE_WINDOWED || mode > AURORA_DISPLAY_MODE_EXCLUSIVE) {
    Log.warn("Ignoring invalid display mode {}", static_cast<int>(mode));
    return;
  }
  const AuroraDisplayMode currentMode = g_displayMode.load(std::memory_order_acquire);
  // Exclusive mode is re-applied even when already active: the desired refresh
  // rate tracks the frame interpolation target, which can change at runtime.
  if (mode == currentMode && mode != AURORA_DISPLAY_MODE_EXCLUSIVE) {
    return;
  }

  // Native messages can be published to the guest before the SDL call returns.
  // Do not persist fullscreen dimensions as the player's windowed placement.
  g_displayMode.store(mode, std::memory_order_release);
  bool success = true;
  if (mode == AURORA_DISPLAY_MODE_WINDOWED) {
    success = SDL_SetWindowFullscreen(g_window, false);
    if (success) {
      success = SDL_SetWindowFullscreenMode(g_window, nullptr);
    }
    if (success && g_windowPosBeforeFullscreen) {
      success = SDL_SetWindowPosition(g_window, g_windowPosBeforeFullscreen->first, g_windowPosBeforeFullscreen->second);
      g_windowPosBeforeFullscreen.reset();
    }
  } else if (mode == AURORA_DISPLAY_MODE_BORDERLESS) {
    if (SDL_GetWindowFullscreenMode(g_window) != nullptr) {
      success = SDL_SetWindowFullscreen(g_window, false);
    }
    if (success) {
      success = SDL_SetWindowFullscreenMode(g_window, nullptr);
    }
    if (success) {
      // Borderless has no mode to carry a monitor: it goes to the monitor the window is on.
      const SDL_DisplayID target = fullscreen_target_display();
      if (target != 0 && target != SDL_GetDisplayForWindow(g_window)) {
        int x = 0;
        int y = 0;
        if (!g_windowPosBeforeFullscreen && SDL_GetWindowPosition(g_window, &x, &y)) {
          g_windowPosBeforeFullscreen = {x, y};
        }
        success = SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(target),
                                        SDL_WINDOWPOS_CENTERED_DISPLAY(target));
      }
    }
    if (success) {
      success = SDL_SetWindowFullscreen(g_window, true);
    }
  } else {
    const SDL_DisplayID target = fullscreen_target_display();
    const SDL_DisplayID display = target != 0 ? target : SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode* desktop = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
    const uint32_t interpolationFps = aurora_get_frame_interpolation_fps();
    const float targetHz = interpolationFps > 60 ? static_cast<float>(interpolationFps) : 60.0f;
    SDL_DisplayMode closest{};
    if (desktop == nullptr ||
        !SDL_GetClosestFullscreenDisplayMode(display, desktop->w, desktop->h, targetHz, true, &closest)) {
      g_displayMode.store(currentMode, std::memory_order_release);
      Log.warn("No exclusive fullscreen mode near {:.0f} Hz is available: {}", targetHz, SDL_GetError());
      return;
    }
    const SDL_DisplayMode* active = SDL_GetWindowFullscreenMode(g_window);
    if (mode == currentMode && active != nullptr && active->displayID == closest.displayID &&
        active->w == closest.w && active->h == closest.h && active->refresh_rate == closest.refresh_rate) {
      return;
    }
    if (get_fullscreen() && mode != currentMode) {
      success = SDL_SetWindowFullscreen(g_window, false);
    }
    if (success) {
      success = SDL_SetWindowFullscreenMode(g_window, &closest);
    }
    if (success) {
      success = SDL_SetWindowFullscreen(g_window, true);
    }
    if (success) {
      Log.info("Exclusive fullscreen requested at {}x{} {:.3f} Hz", closest.w, closest.h, closest.refresh_rate);
    }
  }

  if (!success) {
    g_displayMode.store(currentMode, std::memory_order_release);
    Log.warn("Failed to change display mode: {}", SDL_GetError());
    return;
  }
  g_nativeResizePending.store(true, std::memory_order_release);
  TRY_WARN(push_custom_event(CustomEvent::FutureResize), "Failed to queue fullscreen resize event: {}",
           SDL_GetError());
}

AuroraDisplayMode get_display_mode() { return g_displayMode.load(std::memory_order_acquire); }

uint32_t get_monitors(AuroraMonitor* monitors, uint32_t maxCount) {
  if (window_owner::enabled() && !window_owner::is_owner()) {
    return window_owner::call([=] { return get_monitors(monitors, maxCount); });
  }
#if defined(_WIN32)
  const auto found = query_monitors();
  const NativeMonitor* selected = find_monitor(found, g_fullscreenMonitor);
  std::vector<MonitorLabelInput> inputs;
  for (const auto& monitor : found) {
    const RECT& r = monitor.bounds;
    inputs.push_back({monitor.name, r.left, r.top, r.right, r.bottom, monitor.primary});
  }
  const std::vector<std::string> labels = monitor_labels(inputs);
  uint32_t count = 0;
  for (size_t i = 0; i < found.size() && count < maxCount; ++i) {
    AuroraMonitor& out = monitors[count++];
    std::snprintf(out.id, sizeof(out.id), "%s", found[i].id.c_str());
    std::snprintf(out.label, sizeof(out.label), "%s", labels[i].c_str());
    out.selected = &found[i] == selected;
  }
  return count;
#else
  (void)monitors;
  (void)maxCount;
  return 0;
#endif
}

void set_fullscreen_monitor(const char* id) {
  if (window_owner::enabled() && !window_owner::is_owner()) {
    const std::string copy(id != nullptr ? id : "");
    window_owner::call([=] { set_fullscreen_monitor(copy.c_str()); });
    return;
  }
  g_fullscreenMonitor = id != nullptr ? id : "";
  const AuroraDisplayMode mode = g_displayMode.load(std::memory_order_acquire);
  if (g_window != nullptr && mode != AURORA_DISPLAY_MODE_WINDOWED) {
    set_display_mode(AURORA_DISPLAY_MODE_WINDOWED);
    set_display_mode(mode);
  }
}

void set_window_size(uint32_t width, uint32_t height) {
  TRY_WARN(SDL_RestoreWindow(g_window), "Failed to un-maximize window: {}", SDL_GetError());
  TRY_WARN(SDL_SetWindowSize(g_window, width, height), "Failed to set window size: {}", SDL_GetError());
}

void set_window_position(uint32_t x, uint32_t y) {
  TRY_WARN(SDL_SetWindowPosition(g_window, x, y), "Failed to set window position: {}", SDL_GetError());
}

void center_window() {
  TRY_WARN(SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED),
           "Failed to center window: {}", SDL_GetError());
}

void sync_frame_buffer_size() noexcept {
  if (g_window != nullptr) {
    resize_swapchain();
  }
}

static void push_future_resize_event() {
  TRY_WARN(push_custom_event(CustomEvent::FutureResize), "Failed to push SDL event for future resize: {}",
           SDL_GetError());
}

void (*g_onRenderTargetResized)() = nullptr;

void request_frame_buffer_resize() {
  if (g_window != nullptr) {
    // Publish the request before waking the SDL loop: the render worker consumes it at an ordered
    // frame boundary, and the main thread must never recreate WebGPU targets mid-frame.
    g_nativeResizePending.store(true, std::memory_order_release);
    push_future_resize_event();
  }
}

void set_frame_buffer_scale(float scale) {
  if (scale < 0.f) {
    scale = 0.f;
  }
  if (g_frameBufferScale == scale) {
    return;
  }
  g_frameBufferScale = scale;
  request_frame_buffer_resize();
}

void set_frame_buffer_lines(float lines) {
  if (!std::isfinite(lines) || lines < 0.f) {
    lines = 0.f;
  }
  if (g_frameBufferLines == lines) {
    return;
  }
  g_frameBufferLines = lines;
  request_frame_buffer_resize();
}

bool frame_buffer_line_target() noexcept { return g_frameBufferLines > 0.f; }

void set_frame_buffer_aspect_fit(bool fit) {
  if (g_frameBufferAspectFit == fit) {
    return;
  }

  g_frameBufferAspectFit = fit;
  request_frame_buffer_resize();
}

void set_fixed_aspect(float aspect) {
  if (g_fixedAspect.load(std::memory_order_relaxed) == aspect) {
    return;
  }
  g_fixedAspect.store(aspect, std::memory_order_release);
  request_frame_buffer_resize();
}

void set_present_surface_fill(bool fill) {
  g_presentSurfaceFill = fill;
}

void lock_present_aspect_ratio(int width, int height) {
  if (width <= 0 || height <= 0) {
    unlock_present_aspect_ratio();
    return;
  }
  if (g_presentAspectWidth == width && g_presentAspectHeight == height) {
    return;
  }
  g_presentAspectWidth = width;
  g_presentAspectHeight = height;
}

void unlock_present_aspect_ratio() {
  g_presentAspectWidth = 0;
  g_presentAspectHeight = 0;
}

bool get_present_aspect_ratio(float& aspect) noexcept {
  if (const float fixed = g_fixedAspect.load(std::memory_order_acquire); fixed > 0.f) {
    aspect = fixed;
    return true;
  }
  if (g_presentSurfaceFill && g_window != nullptr) {
    // Queried once per presentation snapshot; use the cached native client size
    // instead of re-entering SDL for a value the window procedure already knows.
    uint32_t nativeWidth = 0;
    uint32_t nativeHeight = 0;
    if (query_native_client_size(nativeWidth, nativeHeight) && nativeWidth > 0 && nativeHeight > 0) {
      aspect = static_cast<float>(nativeWidth) / static_cast<float>(nativeHeight);
      return true;
    }
    int width = 0;
    int height = 0;
    if (SDL_GetWindowSizeInPixels(g_window, &width, &height) && width > 0 && height > 0) {
      aspect = static_cast<float>(width) / static_cast<float>(height);
      return true;
    }
  }
  if (g_presentAspectWidth <= 0 || g_presentAspectHeight <= 0) {
    aspect = 0.f;
    return false;
  }
  aspect = (static_cast<float>(g_presentAspectWidth) / static_cast<float>(g_presentAspectHeight)) *
           vi::present_aspect_correction();
  return aspect > 0.f;
}

void set_background_input(bool value) { SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, value ? "1" : "0"); }

} // namespace aurora::window

#if defined(SDL_PLATFORM_ANDROID)
extern "C" JNIEXPORT void JNICALL Java_org_libsdl_app_SDLSurface_auroraNativeSetSurfaceReady(JNIEnv*, jclass,
                                                                                             jboolean ready) {
  aurora::window::set_surface_ready(ready == JNI_TRUE);
}
#endif
