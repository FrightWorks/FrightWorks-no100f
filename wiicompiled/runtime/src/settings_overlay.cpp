#include <aurora/imgui.h>
#include <unordered_map>
#include <string>
#include <cstdlib>
extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);
#include "netplay_lobby.h"
#include "netplay_ui.h"
#include "settings_overlay.h"
#include "overlay_theme.h"
#include "build_info.h"
#include "no100f_credits.h"
#include "no100f_devmenu.h"
#include "no100f_game_state.h"
#include "no100f_recorder.h"
#include "memory.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "display_settings.h"
#include <dolphin/gx/GXAurora.h>
#include "runtime_log.h"
#include "wii_remote_input.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <aurora/window_owner_sdl.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
bool g_hdrCalibrationOpen = false;
bool g_hdrCalibrationFocusPending = false;
AuroraHdrCalibration g_hdrCalibrationFrame{};
bool g_exitPromptOpen = false;
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(2.25f);  // main.cpp's default (1080p)
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("borderless");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
std::string g_fullscreenMonitor = RuntimeConfigFile::FullscreenDisplay();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(false);  // off unless turned on
bool g_frameGraph = RuntimeConfigFile::FrameGraph(false);
std::atomic<bool> g_vsync{RuntimeConfigFile::VSync(false)};
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

// Render heights: 1x is 480 lines in every aspect (DisplaySettings), the width follows the aspect.
constexpr std::array<ResolutionItem, 12> kResolutions = {{
    {"Auto (screen size)", 0.0f}, {"0.5x (240p)", 0.5f}, {"0.75x (360p)", 0.75f}, {"1x (480p)", 1.0f},
    {"1.5x (720p)", 1.5f}, {"2x (960p)", 2.0f}, {"2.25x (1080p)", 2.25f}, {"3x (1440p)", 3.0f},
    {"4x (1920p)", 4.0f}, {"4.5x (4K)", 4.5f}, {"6x (2880p)", 6.0f}, {"8x (3840p)", 8.0f},
}};

struct AspectItem {
    const char* label;
    int32_t mode;
};
constexpr std::array<AspectItem, 4> kAspects = {{
    {"Original 4:3", DisplaySettings::kAspect4x3},
    {"16:9", DisplaySettings::kAspect16x9},
    {"Fill Window", DisplaySettings::kAspectFill},
    {"Cinematic 2.39:1", DisplaySettings::kAspectCinematic},
}};

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.renderScale = scale;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void SetAspectMode(int32_t mode) {
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.aspect = mode;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetAspectMode(mode);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


// The settings board fades in and takes keyboard/gamepad focus when it opens.
Clock::time_point g_panelOpenedAt{};
bool g_panelFocusPending = false;
bool g_panelOpenedByPad = false;
// With a controller the board has two levels: the page tabs and the page itself (the ##page child window). A goes in,
// B comes back (ImGui's own way out of a child window), LB / RB change the page on either level.
bool g_tabFocusPending = false;   // focus the current page's tab
bool g_pageFocusPending = false;  // focus the page: its first control, or the last one used if the page hasn't changed
bool g_pageKeepsFocus = false;
// The controller button that closed the board is still down: the game mustn't take it for a press (A on Resume = jump).
bool g_gameInputHeldBack = false;

void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
    if (!visible) g_hdrCalibrationOpen = false;
    if (visible) {
        g_panelOpenedAt = Clock::now();
        g_panelFocusPending = true;
    } else {
        g_gameInputHeldBack = true;
    }
}

// The buttons only (a resting finger on a trigger or a drifting stick doesn't count).
bool PadButtonDown() {
    if ((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) == 0) return false;
    for (int key = ImGuiKey_GamepadFaceLeft; key <= ImGuiKey_GamepadR1; ++key) {
        if (ImGui::GetKeyData(static_cast<ImGuiKey>(key))->Down) return true;
    }
    return false;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(OverlayTheme::Px(220.0f), 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImGui::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImGui::CollapsingHeader("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (OverlayTheme::Toggle("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
    ImGui::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
    ImGui::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (OverlayTheme::Toggle("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImGui::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImGui::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        // The captured press belongs to the binding: ImGui can see it a frame later, when the prompt has gone.
        for (int key = ImGuiKey_Gamepad_BEGIN; key < ImGuiKey_Gamepad_END; ++key) {
            ImGui::SetKeyOwner(static_cast<ImGuiKey>(key), ImGuiKeyOwner_Any, ImGuiInputFlags_LockUntilRelease);
        }
        ImGui::SetNavCursorVisible(true);  // the prompt took no navigation, which hid it
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    // A controller's every button is a binding here, A and B too: no navigation (the buttons below are for the mouse).
    const ImGuiWindowFlags navFlags = g_rebind.kind == RebindKind::Controller ? ImGuiWindowFlags_NoNav : 0;
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize | navFlags)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F1, F2 and F10 are reserved.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        ImGui::SetItemDefaultFocus();
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g_rebind.kind != RebindKind::Controller && ImGui::IsKeyPressed(ImGuiKey_NavGamepadCancel, false)) {
            g_rebind.active = false;
        }
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10 && i != SDL_SCANCODE_F1 &&
                    i != SDL_SCANCODE_F2) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = OverlayTheme::Px(220.0f)) {
    // ### : the same control after a rebind changes its caption, or a controller's focus would be lost.
    const std::string caption = std::string(KeyBindingName(scancode)) + "###binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

bool DrawKeyboardSettings(uint32_t port) {
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (OverlayTheme::Toggle("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    if (!enabled) return false;
    ImGui::TextDisabled("Works together with a gamepad on this port. F10 opens settings.");
    if (ImGui::Button("Use WASD + mouse preset") || usePreset) {
        const std::array<int, PAD_BUTTON_COUNT> keys = {
            PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        };
        for (size_t i = 0; i < keys.size(); ++i)
            PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
        const std::array<int, PAD_AXIS_COUNT> axes = {
            SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
            SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        };
        uint32_t axisCount = 0;
        auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i)
            PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
        PADSerializeMappings();
    }
    OverlayTheme::SectionTitle("Keys");
    for (uint32_t i = 0; i < count; ++i) {
        int key = buttons[i].scancode;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(OverlayTheme::Px(220.0f));
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), key, RebindKind::KeyboardButton, buttons[i].padButton);
        ImGui::PopID();
    }
    OverlayTheme::SectionTitle("Stick and trigger keys");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        int key = axes[i].scancode;
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        ImGui::SetNextItemWidth(OverlayTheme::Px(220.0f));
        DrawKeyBinding(label.c_str(), key, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    return true;
}

// Controller settings menu: port selection, controller assignment and button mapping.
int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    OverlayTheme::SectionTitle("Expressions (Dolphin syntax)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + OverlayTheme::Px(440.0f));
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(OverlayTheme::Px(300.0f));
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    OverlayTheme::SectionTitle("Vibration");
    if (OverlayTheme::Toggle("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies to every port.");
    }
}

// The Xbox's Black / White buttons ([controller] xbox_black / xbox_white): any pad button, trigger or stick direction.
// The pad button sends only Black / White, not the GameCube button it is mapped to.
void DrawXboxButtonSettings() {
    using namespace OverlayTheme;
    const float labelWidth = Px(210);
    const float controlWidth = std::max(Px(200), std::min(Px(320), ImGui::GetContentRegionAvail().x - labelWidth));
    static constexpr const char* kActions[] = {"Black (sneak)", "White (soap bubble)"};
    for (size_t action = 0; action < 2; ++action) {
        const std::string current = RuntimeConfigFile::XboxButton(action);
        const auto* currentItem = ControllerNames::FindNativeButton(current);
        const auto label = [](const ControllerNames::NativeButtonItem& item) {
            return item.nativeButton == PAD_NATIVE_BUTTON_DISABLED ? "Off" : item.label;
        };
        RowLabel(kActions[action], labelWidth, controlWidth);
        ImGui::PushID(static_cast<int>(action));
        if (ImGui::BeginCombo("##xboxbutton", currentItem != nullptr ? label(*currentItem) : current.c_str())) {
            for (const auto& item : kNativeButtons) {
                if (item.nativeButton == PAD_NATIVE_BUTTON_INVALID) continue;  // "Default" means nothing here
                const bool selected = currentItem == &item;
                if (ImGui::Selectable(label(item), selected)) RuntimeConfigFile::SetXboxButton(action, item.configName);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    Hint("The chosen pad button sends only Black or White. Hints in the game name it from the next level load.");
}

void DrawGamepadSettings(uint32_t selectedGamePort);

// Controls page: the port, then its gamepad and its keyboard in two sections of their own. Both drive the same port
// at the same time (aurora PADRead adds the keys on top of the gamepad).
void DrawControllerSettings() {
    using namespace OverlayTheme;
    SectionTitle("Controls");
    Hint("Every connected gamepad controls Scooby (player 1), mapped for its type. The keyboard works at the same time.");
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }
    static bool keyboardSection = false;
    const ImVec2 plank(Px(270), Px(40));
    if (Plank("GAMEPAD##controls", !keyboardSection, plank)) keyboardSection = false;
    ImGui::SameLine();
    if (Plank("KEYBOARD AND MOUSE##controls", keyboardSection, plank)) keyboardSection = true;
    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    if (keyboardSection) {
        DrawKeyboardSettings(selectedGamePort);
    } else {
        DrawGamepadSettings(selectedGamePort);
    }
}

void DrawGamepadSettings(uint32_t selectedGamePort) {
    // hle/no100f_xbox_layout.h: works on top of the button mapping below, which stays as it is.
    if (bool xboxLayout = RuntimeConfigFile::XboxOriginalLayout();
        OverlayTheme::Toggle("Original Xbox controls", &xboxLayout)) {
        RuntimeConfigFile::SetXboxOriginalLayout(xboxLayout);
    }
    OverlayTheme::Hint("On: A jump, X head smash, B dig / enter / talk and back in menus, Y on-screen display, LT gum, "
                       "RT run, White soap bubble (LB), Black sneak (RB), Start pause; Black and White work together "
                       "with the triggers. The hints say so too from the next level load. Off (GameCube): B head smash and "
                       "back, X dig, Z + L bubble, Z + R sneak. Gamepads only; the button mapping below stays in GameCube "
                       "buttons either way.");
    if (RuntimeConfigFile::XboxOriginalLayout()) {
        DrawXboxButtonSettings();
    }
    ImGui::Separator();
    const char* currentName = PADGetName(selectedGamePort);
    ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    ImGui::SameLine();
    if (ImGui::SmallButton("Unassign")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
    } else {
        ImGui::SetNextItemWidth(OverlayTheme::Px(320.0f));
        if (ImGui::BeginCombo("##assign", "Assign a connected controller...")) {
            for (uint32_t index = 0; index < controllerCount; ++index) {
                const char* name = PADGetNameForControllerIndex(index);
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(name != nullptr ? name : "Unknown controller")) {
                    PADSetPortForIndex(index, selectedGamePort);
                    g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                    ApplyConfiguredMappings();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSettings(selectedGamePort);
    DrawRumbleSettings();  // above the mapping, and shown with no controller connected too
    if (controllerCount == 0) {
        return;
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    OverlayTheme::SectionTitle("Presets");
    // GameCube and Original Xbox both restore the default mapping; Original Xbox then switches the layout on.
    const auto restoreDefaults = [&](bool xboxLayout) {
        RuntimeConfigFile::SetXboxOriginalLayout(xboxLayout);
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };
    if (ImGui::Button("GameCube")) {
        restoreDefaults(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Original Xbox")) {
        restoreDefaults(true);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };

    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::Button("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    OverlayTheme::SectionTitle("Button mapping");
    ImGui::TextDisabled("LT / L2 = left trigger. RT / R2 = right trigger.");
    ImGui::TextDisabled("LB / L1 = left shoulder. RB / R1 = right shoulder.");
    ImGui::TextDisabled("Click a binding, then press an input. No input for 10 seconds clears it.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(bindingWidth);
        const std::string primaryCaption = std::string(current.label) + "###primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(bindingWidth);
                const std::string altCaption = std::string(altLabel) + "###alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawExpressionSettings();
}

void DrawAudioSettings() {
    using namespace OverlayTheme;
    const float labelWidth = Px(210);
    const float controlWidth = std::max(Px(200), std::min(Px(320), ImGui::GetContentRegionAvail().x - labelWidth));
    SectionTitle("Volume");
    RowLabel("Master", labelWidth, controlWidth);
    if (ImGui::SliderInt("##master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    RowLabel("Music", labelWidth, controlWidth);
    if (ImGui::SliderInt("##music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    RowLabel("Sound effects", labelWidth, controlWidth);
    if (ImGui::SliderInt("##sfx", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    RowLabel("Menus", labelWidth, controlWidth);
    if (ImGui::SliderInt("##ui", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    RowLabel("Voices", labelWidth, controlWidth);
    if (ImGui::SliderInt("##voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }

    SectionTitle("Mute");
    if (Toggle("Mute all sound", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    DrawKeyBinding("Mute shortcut (click, then press a key)", g_muteHotkey, RebindKind::MuteHotkey, 0, Px(160));
    if (Toggle("Mute game music while other media plays", &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
#if defined(__APPLE__)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Detects other apps with active audio output on macOS 14.2 or later. "
            "Apps that keep an output stream running silently may keep game music muted.");
    }
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            Hint("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            Hint("Checking external audio...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            Hint("External audio detection unavailable (requires macOS 14.2 or later).");
#else
            Hint("Media controls are unavailable.");
#endif
        } else {
            Hint("No external media is currently playing.");
        }
    }

    SectionTitle("Advanced");
    if (Toggle("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
}

// LAN Play: this PC's addresses for other players, and the devices asked for rooms directly
// (Tailscale doesn't carry broadcasts; netplay_lobby.h). Peers from `tailscale status` are asked
// anyway; this is for typing in a device's Tailscale name or address by hand.
void DrawLanPlaySettings() {
    static std::vector<std::string> ownAddresses;
    static uint64_t ownAddressesAt = 0;
    if (ownAddressesAt == 0 || SDL_GetTicks() - ownAddressesAt > 2000) {
        ownAddresses = NetplayLobby::OwnAddresses();
        ownAddressesAt = SDL_GetTicks();
    }
    ImGui::TextUnformatted("Rooms are found on your Wi-Fi by themselves, and over Tailscale through");
    ImGui::TextUnformatted("the devices in `tailscale status` and the ones below.");
    ImGui::Separator();
    ImGui::TextUnformatted("This PC (tell other players one of these):");
    for (const auto& address : ownAddresses) {
        ImGui::BulletText("%s%s", address.c_str(), address.rfind("100.", 0) == 0 ? "  (Tailscale)" : "");
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Devices asked for rooms directly:");
    const auto known = NetplayLobby::KnownDevices();
    if (known.empty()) {
        ImGui::TextDisabled("none yet: playing together adds them");
    }
    for (const auto& device : known) {
        ImGui::PushID(device.c_str());
        if (ImGui::SmallButton("Remove")) {
            NetplayLobby::RemoveKnownDevice(device);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(device.c_str());
        ImGui::PopID();
    }
    static char entry[128] = {};
    static std::string result;
    ImGui::SetNextItemWidth(OverlayTheme::Px(260.0f));
    const bool submitted = ImGui::InputTextWithHint("##add-device", "Tailscale name or address", entry, sizeof(entry),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || submitted) && entry[0] != '\0') {
        result = NetplayLobby::AddKnownDevice(entry) ? std::string("Added ") + entry : "Already in the list, or not a name";
        entry[0] = '\0';
    }
    if (!result.empty()) {
        ImGui::TextDisabled("%s", result.c_str());
    }
}

#if defined(_WIN32)
void DrawHdrCalibration() {
    using namespace OverlayTheme;
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 size(std::min(Px(690), viewport->Size.x - Px(24)),
                      std::min(Px(710), viewport->Size.y - Px(24)));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("HDR calibration", &g_hdrCalibrationOpen,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g_hdrCalibrationFocusPending) {
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
            ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
            g_hdrCalibrationFocusPending = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadActivate, false)) ImGui::SetNavCursorVisible(true);
        if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
            ImGui::IsKeyPressed(ImGuiKey_NavGamepadCancel, false)) {
            g_hdrCalibrationOpen = false;
        }
        SectionTitle("HDR brightness");
        bool hdr = RuntimeConfigFile::Hdr();
        float paper = RuntimeConfigFile::HdrPaperWhiteNits();
        float peak = RuntimeConfigFile::HdrPeakNits();
        static bool saveFailed = false;
        bool brightnessChanged = false;
        if (Toggle("HDR on next start", &hdr)) saveFailed = !RuntimeConfigFile::SetHdr(hdr);
        const auto status = aurora_get_hdr_calibration_status();
        ImGui::Text("Display: %s", aurora_get_hdr_display_name());
        const bool ready = status == AURORA_HDR_CALIBRATION_READY;
        switch (status) {
        case AURORA_HDR_CALIBRATION_READY:
            Hint("Live scRGB HDR test patches. Normal picture brightness should feel comfortable; "
                 "bright highlights should retain the checker detail.");
            break;
        case AURORA_HDR_CALIBRATION_OUTPUT_OFF:
            Hint("HDR preview unavailable: turn HDR on and restart the game with Windows HDR enabled.");
            break;
        case AURORA_HDR_CALIBRATION_UNSUPPORTED_BACKEND:
            Hint("True HDR calibration currently requires the Direct3D HDR output path.");
            break;
        case AURORA_HDR_CALIBRATION_DISPLAY_OFF:
            Hint("HDR preview unavailable: Windows HDR is off for the display containing the game.");
            break;
        case AURORA_HDR_CALIBRATION_DISPLAY_UNKNOWN:
            Hint("HDR preview unavailable: the current display's HDR state could not be confirmed.");
            break;
        case AURORA_HDR_CALIBRATION_STAYED_SDR:
            Hint("HDR is on in the settings, but Windows HDR was off for the game's display when it started, so the game "
                 "is showing the standard picture. Turn Windows HDR on and restart the game.");
            break;
        }
        const float width = std::max(Px(140), ImGui::GetContentRegionAvail().x - Px(155));
        RowLabel("Paper white", Px(145), width);
        if (ImGui::SliderFloat("##hdr-paper", &paper, 80.f, 1000.f, "%.1f nits", ImGuiSliderFlags_AlwaysClamp)) {
            saveFailed = !RuntimeConfigFile::SetHdrPaperWhiteNits(paper);
            brightnessChanged = !saveFailed;
        }
        Hint("Normal picture brightness: adjust for comfortable viewing.");
        RowLabel("Peak highlights", Px(145), width);
        if (ImGui::SliderFloat("##hdr-peak", &peak, 80.f, 10000.f, "%.1f nits", ImGuiSliderFlags_AlwaysClamp)) {
            saveFailed = !RuntimeConfigFile::SetHdrPeakNits(peak);
            brightnessChanged = brightnessChanged || !saveFailed;
        }
        if (brightnessChanged && !aurora_set_hdr_brightness(RuntimeConfigFile::HdrPaperWhiteNits(),
                                                          RuntimeConfigFile::HdrPeakNits()))
            saveFailed = true;
        if (peak < paper)
            Hint("Highlights never fall below paper white; the effective game peak is the paper-white value.");
        const float patchWidth = std::max(Px(80), (ImGui::GetContentRegionAvail().x - Px(16)) * 0.5f);
        const float patchHeight = std::clamp(viewport->Size.y * 0.16f, Px(70), Px(125));
        auto Patch = [&](const char* title, float nits, float* rect) {
            ImGui::BeginGroup();
            ImGui::TextUnformatted(title);
            ImGui::Text("%.1f nits target", nits);
            ImGui::Dummy(ImVec2(patchWidth, patchHeight));
            const auto min = ImGui::GetItemRectMin();
            const auto max = ImGui::GetItemRectMax();
            auto* list = ImGui::GetWindowDrawList();
            list->AddRectFilled(min, max, IM_COL32(0, 0, 0, 255));
            if (ready && g_hdrCalibrationOpen && ImGui::IsItemVisible()) {
                const auto clipMin = list->GetClipRectMin();
                const auto clipMax = list->GetClipRectMax();
                rect[0] = (std::max(min.x, clipMin.x) - viewport->Pos.x) / viewport->Size.x;
                rect[1] = (std::max(min.y, clipMin.y) - viewport->Pos.y) / viewport->Size.y;
                rect[2] = (std::min(max.x, clipMax.x) - viewport->Pos.x) / viewport->Size.x;
                rect[3] = (std::min(max.y, clipMax.y) - viewport->Pos.y) / viewport->Size.y;
            } else {
                list->AddText(ImVec2(min.x + Px(8), min.y + Px(8)), Col(Colour::CreamDim), "HDR preview unavailable");
            }
            ImGui::EndGroup();
        };
        Patch("Normal picture", paper, g_hdrCalibrationFrame.paperRect);
        ImGui::SameLine(0.f, Px(16));
        Patch("Bright highlights", std::max(paper, peak), g_hdrCalibrationFrame.peakRect);
        g_hdrCalibrationFrame.paperWhiteNits = paper;
        g_hdrCalibrationFrame.peakNits = std::max(paper, peak);
        Hint("Targets are encoded output values, not measured panel luminance. Windows/display tone mapping "
             "may compress highlights, so this is a visual guide, not a luminance measurement.");
        Hint("Brightness and patches update live while HDR is active. Turning HDR on or off requires a restart. "
             "Windows HDR settings are not changed.");
        if (saveFailed) Hint("Couldn't apply or save HDR settings. Check the runtime log and configuration folder permissions.");
        if (Plank("BACK", true, ImVec2(Px(130), Px(38)))) {
            g_hdrCalibrationOpen = false;
            g_hdrCalibrationFrame = {};
        }
    }
    ImGui::End();
    if (!g_hdrCalibrationOpen) {
        g_hdrCalibrationFrame = {};
        g_pageFocusPending = true;
    }
}
#endif

void DrawVideoSettings() {
    using namespace OverlayTheme;
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    const float labelWidth = Px(210);
    const float controlWidth = std::max(Px(200), std::min(Px(320), ImGui::GetContentRegionAvail().x - labelWidth));

    SectionTitle("Picture");
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    RowLabel("Resolution", labelWidth, controlWidth);
    if (ImGui::BeginCombo("##resolution", resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom")) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            ImGui::BeginDisabled(IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale));
            if (ImGui::Selectable(resolution.label, selected)) SetResolutionScale(resolution.scale);
            ImGui::EndDisabled();
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    uint32_t pictureWidth = 0;
    uint32_t pictureHeight = 0;
    AuroraGetPictureSize(&pictureWidth, &pictureHeight);
    ImGui::SameLine();
    ImGui::TextDisabled("%ux%u", pictureWidth, pictureHeight);

    const DisplaySettings::Settings display = DisplaySettings::Current();
    const auto aspectIt = std::find_if(kAspects.begin(), kAspects.end(), [&](const AspectItem& item) {
        return item.mode == display.aspect;
    });
    RowLabel("Picture shape", labelWidth, controlWidth);
    if (ImGui::BeginCombo("##aspect", aspectIt != kAspects.end() ? aspectIt->label : "Custom")) {
        for (const auto& aspect : kAspects) {
            const bool selected = aspect.mode == display.aspect;
            if (ImGui::Selectable(aspect.label, selected)) SetAspectMode(aspect.mode);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    // The game reads 4:3 versus widescreen once, at boot.
    if ((display.aspect != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()) {
        Hint("Switching between 4:3 and widescreen takes effect after a restart.");
    }

    static constexpr const char* kDisplayModes[] = {
        "Window",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    RowLabel("Display mode", labelWidth, controlWidth);
    if (ImGui::Combo("##displaymode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        Hint("Requests the closest native-resolution display mode to the output frame rate (60 Hz, or the frame "
             "interpolation target).");
    }
#if defined(_WIN32)
    // Same row as the launcher's Settings > Display. The list is read when F10 first draws it and each time it opens.
    static std::vector<AuroraMonitor> monitors;
    static bool monitorsRead = false;
    const auto readMonitors = [] {
        std::array<AuroraMonitor, 16> found{};
        const uint32_t count = aurora_get_monitors(found.data(), static_cast<uint32_t>(found.size()));
        monitors.assign(found.begin(), found.begin() + count);
        monitorsRead = true;
    };
    if (!monitorsRead) {
        readMonitors();
    }
    static constexpr const char* kAutomaticMonitor = "Automatic (where the game window is)";
    const bool automaticMonitor = g_fullscreenMonitor.empty() || g_fullscreenMonitor == "auto";
    const char* monitorPreview = automaticMonitor ? kAutomaticMonitor : "Saved monitor (not connected)";
    for (const AuroraMonitor& monitor : monitors) {
        if (monitor.selected) {
            monitorPreview = monitor.label;
        }
    }
    std::string pickedMonitor;
    RowLabel("Fullscreen monitor", labelWidth, controlWidth);
    if (ImGui::BeginCombo("##fullscreenmonitor", monitorPreview)) {
        if (ImGui::IsWindowAppearing()) {
            readMonitors();
        }
        if (ImGui::Selectable(kAutomaticMonitor, automaticMonitor)) {
            pickedMonitor = "auto";
        }
        for (const AuroraMonitor& monitor : monitors) {
            ImGui::PushID(monitor.id);
            if (ImGui::Selectable(monitor.label, monitor.selected)) {
                pickedMonitor = monitor.id;
            }
            if (monitor.selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!pickedMonitor.empty()) {
        g_fullscreenMonitor = pickedMonitor;
        aurora_set_fullscreen_monitor(g_fullscreenMonitor.c_str());
        RuntimeConfigFile::SetFullscreenDisplay(g_fullscreenMonitor);
        readMonitors();
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_WINDOWED) {
        Hint("Used when the game goes fullscreen (F11 or Display mode).");
    }
#endif
#ifdef AURORA_HAS_INK_OUTLINES
    {
        const int level = std::clamp(RuntimeConfigFile::InkOutlines(), 0, 2);
        RowLabel("Ink outlines", labelWidth, controlWidth);
        if (ImGui::BeginCombo("##inkoutlines", RuntimeConfigFile::kInkOutlineLevels[level])) {
            for (int i = 0; i < 3; ++i) {
                const bool selected = i == level;
                if (ImGui::Selectable(RuntimeConfigFile::kInkOutlineLevels[i], selected) && !selected) {
                    aurora_set_ink_outlines(i);
                    RuntimeConfigFile::SetInkOutlines(i);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        Hint("Cartoon lines around characters and objects. Bold also draws sharp edges inside them.");
    }
#endif

#if defined(_WIN32)
    SectionTitle("HDR");
    if (Plank("HDR calibration...", false, ImVec2(Px(250), Px(38)))) {
        g_hdrCalibrationOpen = true;
        g_hdrCalibrationFocusPending = true;
    }
    Hint("Open brightness controls and real HDR test patterns. HDR output changes require a restart.");
#endif

    SectionTitle("Smoothness");
    constexpr std::array<const char*, 3> kFrameInterpolationModes{
        "Off", "120 FPS", "180 FPS",
    };
    bool frameInterpolationModeChanged = false;
    // At 30 fps the mode is Off (RuntimeConfigFile::FrameInterpolationFps). fps60 can't change while the game runs.
    const bool frameInterpolationLocked = !RuntimeConfigFile::Fps60();
    RowLabel("Frame interpolation", labelWidth, controlWidth);
    ImGui::BeginDisabled(frameInterpolationLocked);
    if (ImGui::BeginCombo("##interpolation", kFrameInterpolationModes[static_cast<size_t>(g_frameInterpolationMode)])) {
        for (int mode = 0; mode < static_cast<int>(kFrameInterpolationModes.size()); ++mode) {
            const bool selected = g_frameInterpolationMode == mode;
            if (ImGui::Selectable(kFrameInterpolationModes[static_cast<size_t>(mode)], selected)) {
                g_frameInterpolationMode = mode;
                frameInterpolationModeChanged = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (frameInterpolationModeChanged) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    Hint(frameInterpolationLocked ? "Only works at 60 fps, so it stays off at 30 fps. The launcher sets the frame rate."
                                  : "Experimental: you might see visual artifacts.");
    {
        // A strength set by hand in Config.toml that is none of the levels shows as its percentage.
        const int percent = std::clamp(RuntimeConfigFile::MotionBlur(), 0, 100);
        std::string current = std::to_string(percent) + "%";
        for (const auto& level : RuntimeConfigFile::kMotionBlurLevels) {
            if (level.percent == percent) current = level.name;
        }
        RowLabel("Motion blur", labelWidth, controlWidth);
        if (ImGui::BeginCombo("##motionblur", current.c_str())) {
            for (const auto& level : RuntimeConfigFile::kMotionBlurLevels) {
                const bool selected = level.percent == percent;
                if (ImGui::Selectable(level.name, selected) && !selected) {
                    aurora_set_motion_blur(RuntimeConfigFile::MotionBlurShutter(level.percent));
                    RuntimeConfigFile::SetMotionBlur(level.percent);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    if (Toggle("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (Toggle("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        Hint("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        Hint("Choose a lower internal resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        Hint("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        Hint("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        Hint(metalFxSupported ? "Render below output resolution for sharper lower-cost output."
                              : "MetalFX spatial upscaling is unavailable on this device.");
        break;
    }
#endif

#if defined(AURORA_HAS_SCENE_STAGE) || defined(AURORA_HAS_BLOOM)
    SectionTitle("Experimental");
#endif
#ifdef AURORA_HAS_SCENE_STAGE
    {
        // A strength set by hand in Config.toml that is none of the levels shows as its percentage.
        const int percent = std::clamp(RuntimeConfigFile::AmbientOcclusion(), 0, 100);
        std::string current = std::to_string(percent) + "%";
        for (const auto& level : RuntimeConfigFile::kAmbientOcclusionLevels) {
            if (level.percent == percent) current = level.name;
        }
        RowLabel("Ambient occlusion", labelWidth, controlWidth);
        if (ImGui::BeginCombo("##ambientocclusion", current.c_str())) {
            for (const auto& level : RuntimeConfigFile::kAmbientOcclusionLevels) {
                const bool selected = level.percent == percent;
                if (ImGui::Selectable(level.name, selected) && !selected) {
                    aurora_set_ambient_occlusion(static_cast<float>(level.percent) / 100.0f);
                    RuntimeConfigFile::SetAmbientOcclusion(level.percent);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        Hint("Soft shade where surfaces meet: in corners, creases and under things. You might see visual glitches.");
    }
#endif
#ifdef AURORA_HAS_BLOOM
    {
        // A strength set by hand in Config.toml that is none of the levels shows as its percentage.
        const int percent = std::clamp(RuntimeConfigFile::Bloom(), 0, 100);
        std::string current = std::to_string(percent) + "%";
        for (const auto& level : RuntimeConfigFile::kBloomLevels) {
            if (level.percent == percent) current = level.name;
        }
        RowLabel("Bloom", labelWidth, controlWidth);
        if (ImGui::BeginCombo("##bloom", current.c_str())) {
            for (const auto& level : RuntimeConfigFile::kBloomLevels) {
                const bool selected = level.percent == percent;
                if (ImGui::Selectable(level.name, selected) && !selected) {
                    aurora_set_bloom(static_cast<float>(level.percent) / 100.0f);
                    RuntimeConfigFile::SetBloom(level.percent);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        Hint("A soft glow around lit windows, lamp light, fire and sparkles. You might see visual glitches.");
    }
#endif

    SectionTitle("Advanced");
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};
    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (Toggle(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(from the next scene)");
    if (Toggle("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (Toggle("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    ImGui::TextDisabled("Graphics API: %s", GraphicsApiDisplayName());
}

// The last frames' times (the UI's frame is the presented one), for the graph.
constexpr int kFrameTimeCount = 180;
float g_frameTimes[kFrameTimeCount] = {};
int g_frameTimeNext = 0;

void DrawFrameGraph() {
    float last = g_frameTimes[(g_frameTimeNext + kFrameTimeCount - 1) % kFrameTimeCount];
    float peak = 0.0f, sum = 0.0f;
    int samples = 0;
    for (float t : g_frameTimes) {
        if (t <= 0.0f) continue;
        peak = std::max(peak, t);
        sum += t;
        ++samples;
    }
    ImGui::Text("Frame: %.1f ms", last);
    ImGui::TextDisabled("avg %.1f  max %.1f", samples > 0 ? sum / samples : 0.0f, peak);
    const float scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 size(kFrameTimeCount * scale, 50.0f * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    // 0 to 33.3 ms (two 60 Hz frames), more when a frame took longer.
    const float top = std::max(100.0f / 3.0f, peak * 1.1f);
    const auto yOf = [&](float ms) { return origin.y + size.y - std::min(ms / top, 1.0f) * size.y; };
    list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 90));
    for (const float line : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
        if (line < top) {
            list->AddLine(ImVec2(origin.x, yOf(line)), ImVec2(origin.x + size.x, yOf(line)), IM_COL32(255, 255, 255, 60));
        }
    }
    const float w = size.x / kFrameTimeCount;
    for (int i = 0; i < kFrameTimeCount; ++i) {
        const float t = g_frameTimes[(g_frameTimeNext + i) % kFrameTimeCount];
        if (t <= 0.0f) continue;
        const ImU32 color = t < 1000.0f / 55.0f ? IM_COL32(90, 220, 90, 230)
                            : t < 1000.0f / 28.0f ? IM_COL32(240, 200, 60, 230)
                                                  : IM_COL32(240, 80, 70, 240);
        const float x = origin.x + i * w;
        list->AddRectFilled(ImVec2(x, yOf(t)), ImVec2(x + std::max(w, 1.0f), origin.y + size.y), color);
    }
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    {
        const float dt = ImGui::GetIO().DeltaTime;
        g_frameTimes[g_frameTimeNext] = dt > 0.0f && dt < 1.0f ? dt * 1000.0f : 0.0f;
        g_frameTimeNext = (g_frameTimeNext + 1) % kFrameTimeCount;
    }
    if (!g_showFps && !g_frameGraph) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (!g_showFps) {
        } else if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
        if (g_frameGraph) {
            DrawFrameGraph();
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawExitPrompt() {
    using namespace OverlayTheme;
    constexpr const char* kTitle = "Quit the game?";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(22), Px(18)));
    const bool open = ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();
    if (!open) return;
    ImGui::PushFont(Heading());
    ImGui::TextColored(Vec(Colour::Lime), "QUIT THE GAME?");
    ImGui::PopFont();
    Hint("Progress since your last save is lost.");
    ImGui::Dummy(ImVec2(0, Px(4)));
    // Unwind at the next guest event boundary, not through the noexcept draw callback.
    if (Plank("QUIT##confirm", true, ImVec2(Px(140), Px(42)))) aurora::window_owner::request_close();
    ImGui::SameLine(0.0f, Px(10));
    if (Plank("CANCEL##cancel", false, ImVec2(Px(140), Px(42)))) g_exitPromptOpen = false;
    ImGui::SetItemDefaultFocus();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadCancel, false)) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// F1 opens the project page. F2 saves the next game frame (without the overlay) as a PNG in
// RuntimeConfigFile::CaptureDirectory: Pictures\FrightWorks (Linux: ~/Pictures/FrightWorks) when installed, else
// Screenshots\ beside the launcher.
constexpr const char* kProjectUrl = "https://github.com/FrightWorks/FrightWorks-no100f";
std::string g_noticeText;
Clock::time_point g_noticeUntil{};
std::filesystem::path g_lastScreenshot;
std::filesystem::path g_noticeFile;  // the file the notice names: a click on the notice shows it in Explorer

// A notice that names a file stays longer (time to reach it with the mouse) and while the mouse is on it.
void ShowNotice(std::string text, std::filesystem::path file = {}) {
    g_noticeText = std::move(text);
    g_noticeFile = std::move(file);
    g_noticeUntil = Clock::now() + std::chrono::milliseconds(g_noticeFile.empty() ? 2500 : 5000);
}

void ShowInExplorer(const std::filesystem::path& file) {
#if defined(_WIN32)
    const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#else
    (void)file;
#endif
}

// F2 also puts the screenshot on the clipboard as a picture (CF_DIB), read back from the saved PNG on a worker thread
// (the PNG itself is encoded off the game thread too). owner = the game window.
void CopyScreenshotToClipboard(std::filesystem::path file, void* owner) {
#if defined(_WIN32)
    std::thread([file = std::move(file), owner] {
        uint32_t width = 0, height = 0;
        uint8_t* rgba = aurora_load_png_rgba(RuntimeConfigFile::PathToUtf8(file).c_str(), &width, &height);
        if (rgba == nullptr) {
            RT_LOG(RT_TAG_RUNTIME) << "screenshot not copied: can't read " << RuntimeConfigFile::PathToUtf8(file)
                                   << std::endl;
            return;
        }
        // 24-bit bottom-up DIB, rows padded to 4 bytes (32-bit DIBs get their unused byte read as alpha by some apps).
        const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~static_cast<size_t>(3);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + stride * height);
        if (auto* header = memory ? static_cast<BITMAPINFOHEADER*>(GlobalLock(memory)) : nullptr) {
            header->biSize = sizeof(BITMAPINFOHEADER);
            header->biWidth = static_cast<LONG>(width);
            header->biHeight = static_cast<LONG>(height);
            header->biPlanes = 1;
            header->biBitCount = 24;
            header->biCompression = BI_RGB;
            header->biSizeImage = static_cast<DWORD>(stride * height);
            auto* bits = reinterpret_cast<uint8_t*>(header + 1);
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t* src = rgba + static_cast<size_t>(height - 1 - y) * width * 4;
                uint8_t* dst = bits + static_cast<size_t>(y) * stride;
                for (uint32_t x = 0; x < width; ++x, src += 4, dst += 3) {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                }
            }
            GlobalUnlock(memory);
        }
        std::free(rgba);
        // Another program may have the clipboard open for a moment.
        bool opened = false;
        for (int attempt = 0; memory && attempt < 10 && !(opened = OpenClipboard(static_cast<HWND>(owner))); ++attempt) {
            Sleep(20);
        }
        const bool copied = opened && EmptyClipboard() && SetClipboardData(CF_DIB, memory) != nullptr;
        if (opened) CloseClipboard();
        if (!copied) {
            if (memory) GlobalFree(memory);  // the clipboard owns it only once SetClipboardData succeeds
            RT_LOG(RT_TAG_RUNTIME) << "screenshot not copied to the clipboard (error " << GetLastError() << ")"
                                   << std::endl;
        }
    }).detach();
#else
    (void)file;
    (void)owner;
#endif
}

// The settings board (F10): on the left a column of Resume, the pages as planks and Quit Game, the page's settings on
// the right, and the keys + build ID at the bottom. With a controller: A opens a page, B goes back, LB / RB change the page.
enum class Page { Video, Audio, Controls, LanPlay, About };
struct PageItem {
    Page page;
    const char* label;
};
constexpr std::array<PageItem, 5> kPages = {{
    {Page::Video, "Video"}, {Page::Audio, "Audio"}, {Page::Controls, "Controls"}, {Page::LanPlay, "LAN Play"},
    {Page::About, "About"},
}};
Page g_page = Page::Video;

void DrawAboutPage() {
    using namespace OverlayTheme;
    SectionTitle("FrightWorks: Night of 100 Frights");
    ImGui::TextUnformatted(no100f_credits::Line().c_str());
    Hint("A native PC port of Scooby-Doo! Night of 100 Frights (GameCube), built on WiiCompiled and Aurora. "
         "It runs your own copy of the game.");
    if (ImGui::Button("Open the project page (F1)")) {
        if (!SDL_OpenURL(kProjectUrl)) ShowNotice(std::string("Couldn't open ") + kProjectUrl);
    }
    SectionTitle("This build");
    ImGui::Text("Version %s", BuildInfo::Version());
    if (!BuildInfo::Release() && BuildInfo::Commit()[0] != '\0') {
        ImGui::Text("Branch %s, commit %s", BuildInfo::Branch(), BuildInfo::Commit());
        if (BuildInfo::Changes()[0] != '\0') {
            ImGui::Text("Uncommitted changes (+%s)", BuildInfo::Changes());
        }
        Hint("A test build: mention this line when you report a bug.");
    }
    ImGui::TextDisabled("Graphics API: %s", GraphicsApiDisplayName());
    if (no100f_devmenu::Enabled()) {
        ImGui::TextColored(Vec(Colour::Lime), "Developer tools are on: F8 opens the dev menu, F3 records a video.");
    }
    SectionTitle("Keys");
    // Pictures\FrightWorks (Windows) or Pictures/FrightWorks (Linux) when installed.
    static const std::string screenshotHint = [] {
        if (RuntimeConfigFile::PortableRootDirectory()) {
            return std::string("Screenshot (Screenshots folder beside the launcher)");
        }
        std::string folder = RuntimeConfigFile::CaptureDirectoryLabel(false);
        folder.pop_back();  // the label ends in a separator
        return "Screenshot (" + folder + ")";
    }();
    const std::array<std::pair<const char*, const char*>, 8> keys = {{
        {"F10 / Esc / Back", "Settings (this board) / back to the game (Back = the controller's Back / Select)"},
        {"F11", "Fullscreen or window"},
        {"F2", screenshotHint.c_str()},
        {"F1", "Project page in your browser"},
        {"F8", "Dev menu (with developer tools on in the launcher)"},
        {"F3", "Start / stop a video (with developer tools on; Videos folder, needs ffmpeg)"},
        {"A / B", "With a controller: A opens a page, B goes back to the page list, then back to the game"},
        {"LB / RB", "Change the page here with a controller"},
    }};
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& [key, what] : keys) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(Vec(Colour::Lime), "%s", key);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(what);
        }
        ImGui::EndTable();
    }
}

void DrawPage(Page page) {
    switch (page) {
    case Page::Video: DrawVideoSettings(); break;
    case Page::Audio:
        DrawAudioSettings();
        DrawRebindPrompt();
        break;
    case Page::Controls:
        DrawControllerSettings();
        DrawRebindPrompt();
        break;
    case Page::LanPlay: DrawLanPlaySettings(); break;
    case Page::About: DrawAboutPage(); break;
    }
}

void DrawTopBar() {
    using namespace OverlayTheme;
    if (!g_topBarVisible) {
        return;
    }
#if defined(_WIN32)
    if (g_hdrCalibrationOpen) {
        DrawHdrCalibration();
        return;
    }
#endif
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fade = std::clamp(std::chrono::duration<float>(Clock::now() - g_panelOpenedAt).count() / 0.14f, 0.0f, 1.0f);

    // Dim the game, darker towards the edges.
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    const ImVec2 v0 = viewport->Pos;
    const ImVec2 v1(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);
    background->AddRectFilled(v0, v1, Col(Colour::Ink, 0.45f * fade));
    const float band = viewport->Size.y * 0.25f;
    background->AddRectFilledMultiColor(v0, ImVec2(v1.x, v0.y + band), Col(Colour::Ink, 0.45f * fade),
                                        Col(Colour::Ink, 0.45f * fade), Col(Colour::Ink, 0.0f), Col(Colour::Ink, 0.0f));
    background->AddRectFilledMultiColor(ImVec2(v0.x, v1.y - band), v1, Col(Colour::Ink, 0.0f), Col(Colour::Ink, 0.0f),
                                        Col(Colour::Ink, 0.45f * fade), Col(Colour::Ink, 0.45f * fade));

    const float margin = Px(20);
    const ImVec2 size(std::min(viewport->Size.x - margin * 2, std::clamp(viewport->Size.x * 0.74f, Px(760), Px(1120))),
                      std::min(viewport->Size.y - margin * 2, std::clamp(viewport->Size.y * 0.82f, Px(520), Px(860))));
    const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(center.x, center.y + (1.0f - fade) * Px(16)), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (g_panelFocusPending) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNavCursorVisible(g_panelOpenedByPad);
        g_panelFocusPending = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(22), Px(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoScrollWithMouse;
    const bool open = ImGui::Begin("FrightWorks settings", nullptr, kFlags);
    ImGui::PopStyleVar(2);
    if (open) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const bool onTabs = ImGui::IsWindowFocused();
        // Opened by key or mouse the cursor is hidden and A does nothing: this press brings it up.
        if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadActivate, false)) ImGui::SetNavCursorVisible(true);
        // ImGui's left stick only scrolls, and there's nothing to scroll on this level.
        if (onTabs && !ImGui::IsAnyItemActive()) {
            constexpr ImGuiInputFlags kRepeat = ImGuiInputFlags_Repeat | ImGuiInputFlags_RepeatRateNavMove;
            constexpr std::array<std::pair<ImGuiKey, ImGuiDir>, 4> kStick = {{
                {ImGuiKey_GamepadLStickUp, ImGuiDir_Up}, {ImGuiKey_GamepadLStickDown, ImGuiDir_Down},
                {ImGuiKey_GamepadLStickLeft, ImGuiDir_Left}, {ImGuiKey_GamepadLStickRight, ImGuiDir_Right},
            }};
            for (const auto& [key, dir] : kStick) {
                if (ImGui::IsKeyPressed(key, kRepeat)) {
                    ImGui::NavMoveRequestSubmit(dir, dir, ImGuiNavMoveFlags_None, ImGuiScrollFlags_None);
                    break;
                }
            }
        }
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
        Board(list, p0, p1, Px(16));

        // Header: the title sign.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImFont* title = Title();
        OutlinedText(list, title, origin, Col(Colour::Lime), Col(Colour::Ink), Px(3), "FRIGHTWORKS");
        const float titleWidth = title->CalcTextSizeA(title->FontSize, FLT_MAX, 0.0f, "FRIGHTWORKS").x;
        ImFont* heading = Heading();
        OutlinedText(list, heading,
                     ImVec2(origin.x + titleWidth + Px(16), origin.y + title->FontSize - heading->FontSize - Px(2)),
                     Col(Colour::Cream), Col(Colour::Ink), Px(2), "SETTINGS");
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, std::max(origin.y + title->FontSize, ImGui::GetCursorScreenPos().y) + Px(14)));

        // Pages + content.
        const float footer = ImGui::GetFontSize() + Px(18);
        const float bodyHeight = ImGui::GetContentRegionAvail().y - footer;
        const float tabsWidth = Px(176);
        // Not while editing a slider: there they are ImGui's slow / fast.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
            !ImGui::IsAnyItemActive()) {
            const int count = static_cast<int>(kPages.size());
            int index = static_cast<int>(g_page);
            const bool previous = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
            const bool next = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false);
            if (previous) g_page = kPages[(index + count - 1) % count].page;
            if (next) g_page = kPages[(index + 1) % count].page;
            if (previous || next) {
                (onTabs ? g_tabFocusPending : g_pageFocusPending) = true;
                ImGui::SetNavCursorVisible(true);
            }
        }
        // Focus on the page panel itself = B coming out of the page: it goes to the page's tab.
        const bool leftPage = g.NavId == ImGui::GetID("##page");
        if (leftPage) g_tabFocusPending = true;
        if (onTabs && !leftPage && ImGui::IsKeyPressed(ImGuiKey_NavGamepadCancel, false)) SetTopBarVisible(false);
        // The column shrinks with the board, so all of it fits the smallest window.
        const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
        const float actionGap = Px(8);  // on top of the spacing: sets Resume and Quit Game apart from the pages
        const float rows = static_cast<float>(kPages.size());
        const float fit = std::min(1.0f, bodyHeight / (rows * Px(46) + 2 * Px(36) + (rows + 1) * spacing.y + 2 * actionGap));
        const ImVec2 actionSize(tabsWidth, Px(36) * fit);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x, spacing.y * fit));
        ImGui::BeginGroup();
        if (ActionPlank("RESUME##resume", Colour::Good, actionSize)) SetTopBarVisible(false);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + actionGap * fit);
        for (const auto& item : kPages) {
            std::string label = item.label;
            std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::toupper(c); });
            if (Plank(label.c_str(), g_page == item.page, ImVec2(tabsWidth, Px(46) * fit))) {
                g_page = item.page;
                if (g.NavActivatePressedId == ImGui::GetItemID()) g_pageFocusPending = true;
            }
            // Moving between tabs shows each page; only A gets to its settings.
            if (g.NavJustMovedToId == ImGui::GetItemID() && !g.NavJustMovedToIsTabbing) g_page = item.page;
            if (g_page == item.page) {
                ImGui::SetItemDefaultFocus();
                if (g_tabFocusPending) {
                    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    ImGui::SetNavCursorVisible(true);
                    g_tabFocusPending = false;
                }
            }
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + actionGap * fit);
        if (ActionPlank("QUIT GAME##quit", Colour::Warn, actionSize)) g_exitPromptOpen = true;
        ImGui::EndGroup();
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, Px(18));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Vec(Colour::Field, 0.72f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(18), Px(14)));
        if (ImGui::BeginChild("##page", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_AlwaysUseWindowPadding)) {
            static Page drawnPage = g_page;
            if (drawnPage != g_page) {
                drawnPage = g_page;
                g_pageKeepsFocus = false;
                if (g.NavCursorVisible) ImGui::SetScrollY(0.0f);  // a controller starts at the top, the mouse keeps its place
            }
            if (g_pageFocusPending) {
                ImGui::FocusWindow(ImGui::GetCurrentWindow());
                ImGui::NavInitWindow(ImGui::GetCurrentWindow(), !g_pageKeepsFocus);
                g_pageKeepsFocus = true;
                g_pageFocusPending = false;
            }
            ImGui::PushID(static_cast<int>(g_page));
            DrawPage(g_page);
            ImGui::PopID();
        }
        g.NextItemData.ItemFlags |= ImGuiItemFlags_NoNav;  // the panel itself is no stop for the D-pad: only A goes in
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Footer: keys on the left, the build on the right.
        ImGui::PushFont(Small());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(6));
        const float footerY = ImGui::GetCursorPosY();
        const size_t firstKey = (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) != 0 ? 0 : 1;
        const std::array<std::pair<const char*, const char*>, 6> footerKeys = {{
            {onTabs ? "A / B" : "B", onTabs ? "open / close" : "back to pages"},
            {"Esc / F10 / Back", "back to the game"}, {"LB / RB", "page"}, {"F2", "screenshot"}, {"F11", "fullscreen"},
            {"F1", "project page"},
        }};
        for (size_t i = firstKey; i < footerKeys.size(); ++i) {
            // A small window leaves out the last ones: the About page lists them all.
            const float keyWidth = ImGui::CalcTextSize(footerKeys[i].first).x + Px(5) + ImGui::CalcTextSize(footerKeys[i].second).x;
            if (i > firstKey && ImGui::GetItemRectMax().x + Px(16) + keyWidth > p0.x + right) break;
            if (i > firstKey) ImGui::SameLine(0.0f, Px(16));
            ImGui::TextColored(Vec(Colour::Lime), "%s", footerKeys[i].first);
            ImGui::SameLine(0.0f, Px(5));
            ImGui::TextDisabled("%s", footerKeys[i].second);
        }
        const char* build = BuildInfo::Id();
        const float buildWidth = ImGui::CalcTextSize(build).x;
        if (ImGui::GetItemRectMax().x + Px(20) < p0.x + right - buildWidth) {
            ImGui::SetCursorPos(ImVec2(right - buildWidth, footerY));
            ImGui::TextDisabled("%s", build);
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

void TakeScreenshot() {
    const std::filesystem::path folder = RuntimeConfigFile::CaptureDirectory(false);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    const std::string base = std::string("FrightWorks_") + stamp;
    std::filesystem::path file = folder / (base + ".png");
    // Two presses in one second: the first file may not be written yet, so skip its name too.
    for (int n = 2; file == g_lastScreenshot || std::filesystem::exists(file, ec); ++n) {
        file = folder / (base + "_" + std::to_string(n) + ".png");
    }
    g_lastScreenshot = file;
    aurora_request_screenshot(RuntimeConfigFile::PathToUtf8(file).c_str());
}

// Developer tools on: "DEV" and the build ID in the bottom-left corner, so screenshots and recordings say which build
// they came from. Drawn in its own full-screen ImGui window, AURORA_CAPTURE_STAMP_WINDOW: F2/F3 captures keep it when
// they leave the menus out ([game] capture_overlay off), which is what the first line here tells aurora.
void DrawDevMark() {
    using namespace OverlayTheme;
    if (!no100f_devmenu::Enabled()) {
        aurora_set_capture_overlay(AURORA_CAPTURE_OVERLAY_NONE);
        return;
    }
    aurora_set_capture_overlay(RuntimeConfigFile::CaptureOverlay() ? AURORA_CAPTURE_OVERLAY_ALL
                                                                   : AURORA_CAPTURE_OVERLAY_STAMP);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    const bool visible = ImGui::Begin(AURORA_CAPTURE_STAMP_WINDOW, nullptr, kFlags);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->PushClipRectFullScreen();
    ImFont* heading = Heading();
    ImFont* small = Small();
    const char* build = BuildInfo::Id();
    const ImVec2 buildSize = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, build);
    const ImVec2 devSize = heading->CalcTextSizeA(heading->FontSize, FLT_MAX, 0.0f, "DEV");
    const float margin = Px(10);
    const float left = viewport->Pos.x + margin;
    const float bottom = viewport->Pos.y + viewport->Size.y - margin;
    const ImVec2 devPos(left, bottom - devSize.y + Px(2));
    const ImVec2 buildPos(devPos.x + devSize.x + Px(8), bottom - buildSize.y);
    list->AddRectFilled(ImVec2(left - Px(6), std::min(devPos.y, buildPos.y) - Px(4)),
                        ImVec2(buildPos.x + buildSize.x + Px(8), bottom + Px(4)), Col(Colour::Ink, 0.55f), Px(8));
    OutlinedText(list, heading, devPos, Col(Colour::Lime), Col(Colour::Ink), Px(2), "DEV");
    list->AddText(small, small->FontSize, buildPos, Col(Colour::Cream, 0.9f), build);
    list->PopClipRect();
    ImGui::End();
}

void DrawNotice() {
    if (const int result = aurora_take_screenshot_result(); result > 0) {
        ShowNotice("Screenshot saved and copied: " + RuntimeConfigFile::CaptureDirectoryLabel(false) +
                       RuntimeConfigFile::PathToUtf8(g_lastScreenshot.filename()),
                   g_lastScreenshot);
        CopyScreenshotToClipboard(g_lastScreenshot, ImGui::GetMainViewport()->PlatformHandleRaw);
    } else if (result < 0) {
        ShowNotice("Screenshot failed (see the log)");
    }
    std::filesystem::path recorderFile;
    if (std::string recorder = no100f_recorder::TakeNotice(recorderFile); !recorder.empty()) {
        ShowNotice(std::move(recorder), std::move(recorderFile));
    }
    if (g_noticeText.empty() || Clock::now() >= g_noticeUntil) {
        return;
    }
    constexpr float kMargin = 10.0f;
    const ImGuiIO& io = ImGui::GetIO();
    // Above the DEV mark when it shares the corner.
    const float bottom = no100f_devmenu::Enabled()
                             ? io.DisplaySize.y - kMargin - OverlayTheme::Heading()->FontSize - OverlayTheme::Px(14)
                             : io.DisplaySize.y - kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    const bool clickable = !g_noticeFile.empty();
    if (ImGui::Begin("Notice", nullptr, clickable ? kFlags : kFlags | ImGuiWindowFlags_NoInputs)) {
        ImGui::TextUnformatted(g_noticeText.c_str());
        if (clickable && ImGui::IsWindowHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            g_noticeUntil = std::max(g_noticeUntil, Clock::now() + std::chrono::milliseconds(500));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ShowInExplorer(g_noticeFile);
                g_noticeText.clear();
            }
        }
    }
    ImGui::End();
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide = !g_topBarVisible && !no100f_devmenu::IsOpen() &&
                            Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    g_gameInputHeldBack = g_gameInputHeldBack && PadButtonDown();
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active || g_gameInputHeldBack ||
                         g_exitPromptOpen || g_topBarVisible || no100f_devmenu::BlocksInput();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    AuroraSetTexturePatches(RuntimeConfigFile::SpinningCoins(true));
    aurora_set_vsync(g_vsync.load());
    aurora_set_fullscreen_monitor(g_fullscreenMonitor.c_str());
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

// The controller's Back / Select / Create button has no GameCube button, so the game never sees it: it opens and
// closes F10 like the key. Not while a mapping uses it (F10's PlayStation preset puts Z there, or a remap / Black /
// White does), so a bound Back still only does its job.
bool BackButtonBound() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        for (const bool alt : {false, true}) {
            uint32_t count = 0;
            const PADButtonMapping* mappings = alt ? PADGetAltButtonMappings(port, &count) : PADGetButtonMappings(port, &count);
            for (uint32_t i = 0; mappings != nullptr && i < count; ++i) {
                if (mappings[i].nativeButton == SDL_GAMEPAD_BUTTON_BACK) return true;
            }
        }
    }
    if (RuntimeConfigFile::XboxOriginalLayout()) {
        for (size_t action = 0; action < 2; ++action) {
            const NativeButtonItem* native = FindNativeButton(RuntimeConfigFile::XboxButton(action));
            if (native != nullptr && native->nativeButton == SDL_GAMEPAD_BUTTON_BACK) return true;
        }
    }
    return false;
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (!g_rebind.active) no100f_devmenu::HandleSdlEvent(ev->sdl);
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        // F11 toggles between a window and borderless fullscreen (saved like the F10 bar's
        // Display mode).
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F11)) {
            const AuroraDisplayMode mode = aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED
                                               ? AURORA_DISPLAY_MODE_BORDERLESS
                                               : AURORA_DISPLAY_MODE_WINDOWED;
            aurora_set_display_mode(mode);
            g_displayMode = static_cast<int>(aurora_get_display_mode());
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F1)) {
            if (!SDL_OpenURL(kProjectUrl)) ShowNotice(std::string("Couldn't open ") + kProjectUrl);
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F2)) {
            TakeScreenshot();
        }
        const bool backButton = ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && !controller_mapping_wizard::IsActive() &&
                                !BackButtonBound();
        if (!g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_F10) || backButton)) {
            g_panelOpenedByPad = backButton;
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        // Escape is the game's + (its pause menus, Keybinds); here it only closes the F10 bar
        // or the quit prompt.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            } else if (g_hdrCalibrationOpen) {
                g_hdrCalibrationOpen = false;
                g_pageFocusPending = true;
            } else if (g_topBarVisible) {
                SetTopBarVisible(false);
            }
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

bool g_spinningCoins = RuntimeConfigFile::SpinningCoins(true);

void SetSpinningCoins(bool enabled) noexcept {
    g_spinningCoins = enabled;
    AuroraSetTexturePatches(enabled);
    RuntimeConfigFile::SetSpinningCoins(enabled);
}

bool SpinningCoins() noexcept {
    return g_spinningCoins;
}

void SetShowFps(bool show) noexcept {
    g_showFps = show;
    RuntimeConfigFile::SetShowFps(show);
}

bool ShowFps() noexcept {
    return g_showFps;
}

void SetVSync(bool enabled) noexcept {
    g_vsync.store(enabled);
    aurora_set_vsync(enabled);
    RuntimeConfigFile::SetVSync(enabled);
}

bool VSync() noexcept {
    return g_vsync.load(std::memory_order_relaxed);
}

void SetFrameGraph(bool show) noexcept {
    g_frameGraph = show;
    RuntimeConfigFile::SetFrameGraph(show);
}

bool FrameGraph() noexcept {
    return g_frameGraph;
}

void SetRenderScale(float scale) noexcept {
    SetResolutionScale(scale);
}

float RenderScale() noexcept {
    return g_resolutionScale;
}

void SetAspect(int32_t mode) noexcept {
    SetAspectMode(mode);
}

int32_t Aspect() noexcept {
    return DisplaySettings::Latest().aspect;
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

namespace {

// The hotkey guide: a small board in the bottom-right corner while the game shows its title screen and menus, so
// players find F10 and the other keys. Fades out for the attract demo, movies (intro, credits, trailers), in play,
// and while F10 or the dev menu is open. A menu like the others: in F2/F3 captures only with [game] capture_overlay.
float g_hotkeyGuideAlpha = 0.0f;

// Scales the alpha of everything drawn into `list` since vertex `first` (fading a panel in and out).
void FadeVertices(ImDrawList* list, int first, float alpha) {
    if (alpha >= 1.0f) return;
    for (int i = first; i < list->VtxBuffer.Size; ++i) {
        ImU32& col = list->VtxBuffer[i].col;
        const auto a = static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

// A controller button picture from the button icon pack in use (button_icons/<family>/<config name>.png), cropped to
// its visible part; no id when the pack has none for that button. Loaded once per file (ImGui textures can't be
// freed, but there are only a few: three families, a handful of buttons).
struct ButtonIcon {
    ImTextureID id{};
    ImVec2 uv0{0, 0}, uv1{1, 1};
    float aspect = 1.0f;  // width / height of the visible part
};

const ButtonIcon& LoadButtonIcon(const std::string& folder, const char* button) {
    static std::unordered_map<std::string, ButtonIcon> cache;
    const std::string path = folder + "/" + button + ".png";
    if (const auto it = cache.find(path); it != cache.end()) return it->second;
    ButtonIcon icon;
    uint32_t w = 0, h = 0;
    if (uint8_t* rgba = aurora_load_png_rgba(path.c_str(), &w, &h)) {
        uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                if (rgba[(static_cast<size_t>(y) * w + x) * 4 + 3] > 16) {
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
                }
        if (x1 > x0 && y1 > y0) {
            icon.id = aurora_imgui_add_texture(w, h, rgba);
            icon.uv0 = ImVec2(float(x0) / float(w), float(y0) / float(h));
            icon.uv1 = ImVec2(float(x1) / float(w), float(y1) / float(h));
            icon.aspect = float(x1 - x0) / float(y1 - y0);
        }
        std::free(rgba);
    }
    return cache.emplace(path, icon).first->second;
}

// One row of a guide card: its key or button pictures (side by side), then what it does. Without all the pictures:
// a key shows `key` in gold lettering, a controller button shows `key` on a little button-shaped chip.
struct GuideRow {
    std::string key;
    std::string what;
    bool button = false;
    std::vector<ButtonIcon> icons;

    bool HasIcons() const {
        return !icons.empty() && std::all_of(icons.begin(), icons.end(), [](const ButtonIcon& i) { return i.id != ImTextureID{}; });
    }
};

float IconsWidth(const GuideRow& row, float iconSize) {
    float width = 0;
    for (const ButtonIcon& icon : row.icons) width += iconSize * icon.aspect;
    return width + OverlayTheme::Px(4) * static_cast<float>(row.icons.size() - 1);
}

struct GuideCard {
    const char* title = "";
    std::vector<GuideRow> rows;
    // Filled by MeasureCard.
    float keyWidth = 0, rowHeight = 0, iconSize = 0;
    ImVec2 size;
};

void MeasureCard(GuideCard& card) {
    using namespace OverlayTheme;
    ImFont* heading = Heading();
    ImFont* body = Body();
    ImFont* small = Small();
    const bool pictures = std::any_of(card.rows.begin(), card.rows.end(),
                                      [](const GuideRow& r) { return r.button || r.HasIcons(); });
    card.iconSize = std::round(body->FontSize + Px(8));
    card.rowHeight = pictures ? card.iconSize + Px(5) : body->FontSize + Px(5);
    float whatWidth = 0;
    card.keyWidth = 0;
    for (const GuideRow& row : card.rows) {
        float keyWidth;
        if (row.HasIcons()) keyWidth = IconsWidth(row, card.iconSize);
        else if (!row.button) keyWidth = heading->CalcTextSizeA(body->FontSize, FLT_MAX, 0.0f, row.key.c_str()).x;
        else keyWidth = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, row.key.c_str()).x + Px(14);
        card.keyWidth = std::max(card.keyWidth, keyWidth);
        whatWidth = std::max(whatWidth, body->CalcTextSizeA(body->FontSize, FLT_MAX, 0.0f, row.what.c_str()).x);
    }
    const ImVec2 title = heading->CalcTextSizeA(heading->FontSize, FLT_MAX, 0.0f, card.title);
    const float pad = Px(14), gap = Px(14);
    card.size = ImVec2(pad * 2 + std::max(title.x, card.keyWidth + gap + whatWidth),
                       pad * 2 + title.y + Px(6) + card.rowHeight * static_cast<float>(card.rows.size()));
}

void DrawCard(ImDrawList* list, const GuideCard& card, ImVec2 min) {
    using namespace OverlayTheme;
    ImFont* heading = Heading();
    ImFont* body = Body();
    ImFont* small = Small();
    const float pad = Px(14), gap = Px(14);
    Board(list, min, ImVec2(min.x + card.size.x, min.y + card.size.y), Px(12));
    OutlinedText(list, heading, ImVec2(min.x + pad, min.y + pad), Col(Colour::Lime), Col(Colour::Ink), Px(2), card.title);
    float y = min.y + pad + heading->FontSize + Px(6);
    for (const GuideRow& row : card.rows) {
        const float x = min.x + pad, mid = y + card.rowHeight * 0.5f;
        if (row.HasIcons()) {
            float left = x;
            for (const ButtonIcon& icon : row.icons) {
                const float w = card.iconSize * icon.aspect;
                list->AddImage(icon.id, ImVec2(left, mid - card.iconSize * 0.5f),
                               ImVec2(left + w, mid + card.iconSize * 0.5f), icon.uv0, icon.uv1);
                left += w + Px(4);
            }
        } else if (!row.button) {
            list->AddText(heading, body->FontSize, ImVec2(x, mid - body->FontSize * 0.5f + Px(1)), Col(Colour::Gold),
                          row.key.c_str());
        } else {
            const ImVec2 text = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, row.key.c_str());
            const ImVec2 chipMin(x, mid - card.iconSize * 0.42f), chipMax(x + text.x + Px(14), mid + card.iconSize * 0.42f);
            list->AddRectFilled(chipMin, chipMax, Col(Colour::Purple), card.iconSize * 0.3f);
            list->AddRect(chipMin, chipMax, Col(Colour::Ink), card.iconSize * 0.3f, 0, Px(2));
            list->AddText(small, small->FontSize, ImVec2(x + Px(7), mid - text.y * 0.5f), Col(Colour::Cream), row.key.c_str());
        }
        list->AddText(body, body->FontSize, ImVec2(x + card.keyWidth + gap, mid - body->FontSize * 0.5f),
                      Col(Colour::Cream), row.what.c_str());
        y += card.rowHeight;
    }
}

// The pad's Back button and left bumper by family (the pack folder ends in xbox, playstation or nintendo), for when
// the pack has no picture of them.
bool FolderIs(const std::string& folder, std::string_view family) {
    return folder.size() >= family.size() && folder.compare(folder.size() - family.size(), family.size(), family) == 0;
}
const char* BackLabel(const std::string& folder) {
    if (FolderIs(folder, "playstation")) return "Share";
    if (FolderIs(folder, "nintendo")) return "Minus";
    return "View";
}
const char* LeftBumperLabel(const std::string& folder) {
    if (FolderIs(folder, "playstation")) return "L1";
    if (FolderIs(folder, "nintendo")) return "L";
    return "LB";
}

// The guide cards in the bottom-right corner on the title screen and main menu: HOTKEYS (keyboard), and CONTROLLER
// to its left while a controller is connected, drawn with that controller's button icons. Both fade out for the
// attract demo, movies (intro, credits, trailers), in play, and while F10 or the dev menu is open. Menus like the
// others: in F2/F3 captures only with [game] capture_overlay.
void DrawHotkeyGuide() {
    using namespace OverlayTheme;
    uint32_t mode = 0, state = 0, movie = 0;
    const bool show = !g_topBarVisible && !no100f_devmenu::IsOpen() &&
                      Memory::TryRead32(no100f_game_state::kModeAddress, mode) &&
                      Memory::TryRead32(no100f_game_state::kStateAddress, state) &&
                      Memory::TryRead32(no100f_game_state::kMovieAddress, movie) &&
                      no100f_game_state::IsMainMenu(mode, state, movie);
    const float step = ImGui::GetIO().DeltaTime / 0.3f;
    g_hotkeyGuideAlpha = std::clamp(g_hotkeyGuideAlpha + (show ? step : -step), 0.0f, 1.0f);
    if (g_hotkeyGuideAlpha <= 0.0f) return;

    const bool dev = no100f_devmenu::Enabled();
    // Keycaps from button_icons/keyboard/<key>.png beside the game (shipped with the controller icons); gold names
    // without them.
    static const std::string keyFolder = RuntimeConfigFile::PathToUtf8(
        RuntimeConfigFile::ExecutableDirectory().value_or(std::filesystem::path{}) / "button_icons" / "keyboard");
    const auto key = [](const char* name, const char* what, std::initializer_list<const char*> files) {
        GuideRow row{name, what};
        for (const char* file : files) row.icons.push_back(LoadButtonIcon(keyFolder, file));
        return row;
    };
    GuideCard keys;
    keys.title = "Hotkeys";
    keys.rows = {key("F10 / Esc", "Settings", {"f10", "escape"}), key("F11", "Fullscreen or window", {"f11"}),
                 key("F2", "Screenshot", {"f2"}), key("F1", "Project page", {"f1"})};
    if (dev) {
        keys.rows.push_back(key("F8", "Dev menu", {"f8"}));
        keys.rows.push_back(key("F3", "Record a video", {"f3"}));
    }
    MeasureCard(keys);

    char folderBuffer[1024] = {};
    const size_t folderLength = aurora_get_button_icon_folder(folderBuffer, sizeof(folderBuffer));
    GuideCard pad;
    pad.title = "Controller";
    if (folderLength > 0 && folderLength < sizeof(folderBuffer)) {
        const std::string folder(folderBuffer, folderLength);
        if (!BackButtonBound()) pad.rows.push_back({BackLabel(folder), "Settings", true, {LoadButtonIcon(folder, "back")}});
        if (dev) {
            pad.rows.push_back({LeftBumperLabel(folder), "Dev menu focus on / off", true,
                                {LoadButtonIcon(folder, "left_shoulder")}});
        }
    }
    if (!pad.rows.empty()) MeasureCard(pad);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = Px(16), rise = (1.0f - g_hotkeyGuideAlpha) * Px(12);  // slides up a little as it fades in
    const float bottom = viewport->Pos.y + viewport->Size.y - margin + rise;
    const ImVec2 keysMin(viewport->Pos.x + viewport->Size.x - margin - keys.size.x, bottom - keys.size.y);

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##HotkeyGuide", nullptr, kFlags)) {
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->PushClipRectFullScreen();
        const int firstVertex = list->VtxBuffer.Size;
        DrawCard(list, keys, keysMin);
        if (!pad.rows.empty()) DrawCard(list, pad, ImVec2(keysMin.x - Px(14) - pad.size.x, bottom - pad.size.y));
        FadeVertices(list, firstVertex, g_hotkeyGuideAlpha);
        list->PopClipRect();
    }
    ImGui::End();
}

// The credits pill: while the game's credits movie plays (from the main menu, the title screen's idle loop or the end
// of the game), "FrightWorks by ..." in the bottom-right corner, over the black bar beside the 4:3 movie.
float g_creditsPillAlpha = 0.0f;

void DrawCreditsPill() {
    using namespace OverlayTheme;
    uint32_t movie = 0;
    const bool show = !g_topBarVisible &&
                      no100f_game_state::g_creditsMovie.load(std::memory_order_relaxed) &&
                      Memory::TryRead32(no100f_game_state::kMovieAddress, movie) && movie != 0;
    const float step = ImGui::GetIO().DeltaTime / 0.5f;
    g_creditsPillAlpha = std::clamp(g_creditsPillAlpha + (show ? step : -step), 0.0f, 1.0f);
    if (g_creditsPillAlpha <= 0.0f) return;

    // FRIGHTWORKS  Made by <author>
    //              with the help of <helper 1>
    //                               <helper 2> ...
    constexpr const char* kName = "FrightWorks";
    constexpr const char* kHelp = "with the help of ";
    static const std::string by = std::string("Made by ") + no100f_credits::kAuthor;
    ImFont* heading = Heading();
    ImFont* body = Body();
    const auto textWidth = [body](const char* text) { return body->CalcTextSizeA(body->FontSize, FLT_MAX, 0.0f, text).x; };
    const ImVec2 nameSize = heading->CalcTextSizeA(heading->FontSize, FLT_MAX, 0.0f, kName);
    const float padX = Px(14), padY = Px(7), gap = Px(8), lineHeight = body->FontSize + Px(3);
    const float helpWidth = textWidth(kHelp);
    float textBlock = textWidth(by.c_str());
    for (const char* helper : no100f_credits::kHelpers) textBlock = std::max(textBlock, helpWidth + textWidth(helper));
    const size_t lines = 1 + std::size(no100f_credits::kHelpers);
    const float firstLine = std::max(nameSize.y, lineHeight);
    const float height = padY * 2 + firstLine + lineHeight * static_cast<float>(lines - 1);
    const float width = padX * 2 + nameSize.x + gap + textBlock;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = Px(16);
    const ImVec2 min(viewport->Pos.x + viewport->Size.x - margin - width, viewport->Pos.y + viewport->Size.y - margin - height);
    const ImVec2 max(min.x + width, min.y + height);

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##CreditsPill", nullptr, kFlags)) {
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->PushClipRectFullScreen();
        const int firstVertex = list->VtxBuffer.Size;
        const float rounding = Px(14);
        list->AddRectFilled(min, max, Col(Colour::Ink, 0.72f), rounding);
        list->AddRect(min, max, Col(Colour::Lime, 0.35f), rounding, 0, Px(1));
        OutlinedText(list, heading, ImVec2(min.x + padX, min.y + padY + (firstLine - nameSize.y) * 0.5f + Px(1)),
                     Col(Colour::Lime), Col(Colour::Ink), Px(2), kName);
        const float textX = min.x + padX + nameSize.x + gap;
        float y = min.y + padY + (firstLine - body->FontSize) * 0.5f;
        list->AddText(body, body->FontSize, ImVec2(textX, y), Col(Colour::Cream), by.c_str());
        bool first = true;
        for (const char* helper : no100f_credits::kHelpers) {
            y += lineHeight;
            if (first) list->AddText(body, body->FontSize, ImVec2(textX, y), Col(Colour::CreamDim), kHelp);
            list->AddText(body, body->FontSize, ImVec2(textX + helpWidth, y), Col(Colour::Cream), helper);
            first = false;
        }
        FadeVertices(list, firstVertex, g_creditsPillAlpha);
        list->PopClipRect();
    }
    ImGui::End();
}

} // namespace

void Draw() noexcept {
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    g_hdrCalibrationFrame = {};
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    DrawShaderCompilationStatus();
    NetplayUi::Draw();
    DrawFpsOverlay();
    if (no100f_devmenu::Enabled()) {
        no100f_recorder::Update();
    }
    DrawNotice();
    DrawDevMark();
    DrawHotkeyGuide();
    DrawCreditsPill();
    no100f_recorder::DrawIndicator();
    DrawTopBar();
    no100f_devmenu::Draw();
    DrawExitPrompt();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    aurora_set_hdr_calibration(&g_hdrCalibrationFrame);
}
} // namespace settings_overlay
