#include <aurora/imgui.h>
#include <unordered_map>
#include <string>
#include <cstdlib>
extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);
#include "netplay_lobby.h"
#include "netplay_session.h"
#include "netplay_ui.h"
#include "settings_overlay.h"
#include "overlay_theme.h"
#include "menu_rows.h"
#include "build_info.h"
#include "no100f_credits.h"
#include "no100f_devmenu.h"
#include "no100f_game_state.h"
#include "no100f_recorder.h"
#include "no100f_speedrun.h"
#include "memory.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
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
    const std::string mode = RuntimeConfigFile::StartDisplayMode();
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
// F10 > Speedrun (no100f_speedrun.h).
bool g_speedrunOverlay = RuntimeConfigFile::SpeedrunOverlay();
int g_speedrunCategory = no100f_speedrun::CategoryIndex(RuntimeConfigFile::SpeedrunCategory());
int g_speedrunSplits = RuntimeConfigFile::SpeedrunSplits();
bool g_speedrunLoadless = RuntimeConfigFile::SpeedrunLoadless();
bool g_speedrunSaveRuns = RuntimeConfigFile::SpeedrunSaveRuns();
constexpr std::array<const char*, 4> kSpeedrunCornerIds = {"top-left", "top-right", "bottom-left", "bottom-right"};
constexpr std::array<const char*, 4> kSpeedrunCornerNames = {"Top left", "Top right", "Bottom left", "Bottom right"};
int g_speedrunCorner = [] {
    const std::string corner = RuntimeConfigFile::SpeedrunCorner();
    for (size_t i = 0; i < kSpeedrunCornerIds.size(); ++i)
        if (corner == kSpeedrunCornerIds[i]) return static_cast<int>(i);
    return 0;
}();
bool g_frameGraph = RuntimeConfigFile::FrameGraph(false);
std::atomic<bool> g_vsync{RuntimeConfigFile::VSync(false)};
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
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
// HDR calibration is a page inside the Video page: the row that opened it gets the selection back when it closes.
int g_hdrCalibrationReturnRow = 0;
// The controller button that closed the board is still down: the game mustn't take it for a press (B = dig).
bool g_gameInputHeldBack = false;

void CloseHdrCalibration() {
    if (!g_hdrCalibrationOpen) return;
    g_hdrCalibrationOpen = false;
    g_hdrCalibrationFrame = {};
    MenuRows::Select(g_hdrCalibrationReturnRow);
}

void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
    if (!visible) CloseHdrCalibration();
    if (visible) {
        g_panelOpenedAt = Clock::now();
        g_panelFocusPending = true;
        MenuRows::SetPadInput(g_panelOpenedByPad);
    } else {
        g_gameInputHeldBack = true;
        MenuRows::Release();
    }
}

// F10 pauses the game with the game's own pause: Start, pressed for it (PressedButtons, ORed into pad 1 after the input
// block), opens the pause menu (mode 7) and stops the world. Only in play with the player in control, so a cutscene or
// camera pan plays to its end first and the game pauses as soon as control comes back. Closing F10 presses Start again
// to resume, but only if F10 made the pause: a pause menu the player opened stays open. Not in LAN play, where the
// other players' game would stop too.
int g_pausePressReads = 0;          // PADReads that still see Start held
Clock::time_point g_pauseLastPress{};
bool g_pauseAttempted = false;      // Start pressed for this F10 opening
bool g_pausedByF10 = false;         // ... and the game took it

void PressStartForPause() {
    g_pausePressReads = 3;  // a few reads, so the game sees one clean press and release
    g_pauseLastPress = Clock::now();
}

void UpdateF10Pause() {
    namespace gs = no100f_game_state;
    uint32_t mode = 0, control = 0, movie = 0;
    const bool read = Memory::TryRead32(gs::kModeAddress, mode) && Memory::TryRead32(gs::kPlayerControlAddress, control) &&
                      Memory::TryRead32(gs::kMovieAddress, movie);
    if (g_pauseAttempted && !g_pausedByF10 && read && mode == gs::kModePause) {
        g_pausedByF10 = true;
        RT_LOGF(RT_TAG_RUNTIME, "[f10] game paused\n");
    }
    if (g_pausePressReads > 0) return;
    using std::chrono::milliseconds;
    const auto sinceLastPress = Clock::now() - g_pauseLastPress;
    if (g_topBarVisible && !NetplaySession::Active()) {
        // Retried slowly while the game doesn't take it.
        if (!g_pausedByF10 && sinceLastPress >= milliseconds(1500) && read && mode == gs::kModeInPlay && control == 1 &&
            movie == 0) {
            PressStartForPause();
            g_pauseAttempted = true;
            RT_LOGF(RT_TAG_RUNTIME, "[f10] pressing Start to pause\n");
        }
        return;
    }
    // Closed: the game may still be taking the pause press, and the resume press needs a release before it.
    if (g_pauseAttempted && sinceLastPress < milliseconds(g_pausedByF10 ? 200 : 500)) return;
    if (g_pausedByF10 && read && mode == gs::kModePause) {
        PressStartForPause();
        RT_LOGF(RT_TAG_RUNTIME, "[f10] resuming the game\n");
    }
    g_pausedByF10 = false;
    g_pauseAttempted = false;
}

// The buttons only (a resting finger on a trigger or a drifting stick doesn't count). Start closes the board too.
bool PadButtonDown() {
    if ((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) == 0) return false;
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadR1; ++key) {
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
    char text[160];
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        std::snprintf(text, sizeof(text), "x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        MenuRows::Info("Accelerometer", text);
    } else {
        MenuRows::Info("Accelerometer", "No data yet");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth and it falls back to a nominal zero
    // point, leaving a small per-axis bias; measured here with the remote at rest.
    const bool calibrating = WiiRemoteInput::IsAccelCalibrating();
    if (calibrating) {
        std::snprintf(text, sizeof(text), "Hold still... %d%%",
                      static_cast<int>(WiiRemoteInput::AccelCalibrationProgress() * 100.0f));
    }
    ImGui::BeginDisabled(calibrating);
    if (MenuRows::Action("Calibrate the accelerometer", calibrating ? text : nullptr)) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    ImGui::EndDisabled();
    MenuRows::Describe("Put the remote down on a flat surface with the buttons facing up and don't touch it for about two "
                       "seconds. Corrects the steering offset of a remote held sideways.\n\nFlat with the buttons up reads "
                       "(0, -1, 0); sideways as a wheel (1, 0, 0), and z follows the turn.");
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || calibrating);
    if (MenuRows::Action("Clear the calibration")) WiiRemoteInput::ClearAccelCalibration();
    ImGui::EndDisabled();
    MenuRows::Describe("Goes back to SDL's own zero point.");
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        MenuRows::Note(message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        std::snprintf(text, sizeof(text), "x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
        MenuRows::Info("Stored offset", text);
    } else {
        MenuRows::Note("Not calibrated: SDL's zero point is used (console.log says \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth): driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    MenuRows::Section("Wii Remotes (Bluetooth)");
    if (MenuRows::Toggle("Use Wii Remotes and Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    MenuRows::Describe("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.\n\nPairing: Windows "
                       "Settings > Bluetooth > Add device, then press 1+2 (or the red SYNC button) on the remote. Leave the "
                       "PIN empty. A remote that was paired before also needs to be turned on with 1+2 or SYNC.");
    if (MenuRows::Toggle("Keep scanning for Wii Remotes", &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    MenuRows::Describe("Like Dolphin's Continuous Scanning: while no Wii controller is connected, Bluetooth is checked every "
                       "2 seconds, so a remote that dropped out (\"Communications with the controller have been "
                       "interrupted\") or was turned on after launch comes back by itself.");
    // The driver hint is only read at launch, so a rescan after the user turned the setting off would still
    // re-enumerate Wii devices in this session.
    char status[96];
    if (WiiRemoteInput::IsScanning()) {
        std::snprintf(status, sizeof(status), "Scanning (%u so far)", WiiRemoteInput::ScanCount());
    } else {
        std::snprintf(status, sizeof(status), "Not scanning");
    }
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (MenuRows::Action("Rescan now", status)) WiiRemoteInput::RescanNow();
    ImGui::EndDisabled();
    MenuRows::Describe("Looks for Wii Remotes again: press 1+2 on the remote while it scans.");

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    const std::string portLabel = "Port " + std::to_string(selectedGamePort + 1);
    MenuRows::Info(portLabel.c_str(), WiiRemoteInput::KindLabel(kind));
    char text[200];
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            std::snprintf(text, sizeof(text), "%s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                          held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                          held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                          held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                          held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            MenuRows::Info("Classic buttons", text);
            std::snprintf(text, sizeof(text), "%+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0], sample.clLStick[1],
                          static_cast<int>(sample.clLStickRaw[0]), static_cast<int>(sample.clLStickRaw[1]));
            MenuRows::Info("Left stick", text);
            std::snprintf(text, sizeof(text), "%+.2f %+.2f (WPAD %+d %+d)", sample.clRStick[0], sample.clRStick[1],
                          static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            MenuRows::Info("Right stick", text);
            MenuRows::Note("Capitals = held. The game reads this Classic Controller through KPAD, as on the console: its "
                           "buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the SDL_GAMEPAD_BUTTON_DPAD_* values) while
            // its default HIDAPI mapping expects a hat, so SDL_GetGamepadButton never sees them; read the joystick
            // directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) { return joystick != nullptr && SDL_GetJoystickButton(joystick, index); };
            std::snprintf(text, sizeof(text), "%s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                          rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                          rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                          rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            MenuRows::Info("Raw D-pad", text);
            std::snprintf(text, sizeof(text), "%s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                          rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                          rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            MenuRows::Info("Raw face buttons", text);
            std::snprintf(text, sizeof(text), "%d / %d (pressed above 0)",
                          SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                          SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            MenuRows::Info("Raw ZL / ZR", text);
            MenuRows::Note("Capitals = held. A button that never turns to capitals while held isn't reaching SDL at all (a "
                           "driver problem, not a mapping one). This pad uses Nintendo's own layout (a/b/x/y as labelled); "
                           "the button mapping above doesn't apply to it.");
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

// The look of the menu's prompts (quit, rebind): a flat card in the middle of the window.
void PushPromptStyle() {
    using namespace OverlayTheme;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(24)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, Px(1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Vec(Colour::BoardBottom, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, Vec(Colour::Purple, 0.8f));
}

void PopPromptStyle() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void DrawRebindPrompt() {
    using namespace OverlayTheme;
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    // A controller's every button is a binding here, A and B too: no navigation (the buttons below are for the mouse).
    const ImGuiWindowFlags navFlags = g_rebind.kind == RebindKind::Controller ? ImGuiWindowFlags_NoNav : 0;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    PushPromptStyle();
    const bool open = ImGui::BeginPopupModal("Rebind input", &g_rebind.active,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | navFlags);
    PopPromptStyle();
    if (!open) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::PushFont(Tab());
        ImGui::TextColored(Vec(Colour::Lime), "%s", g_rebind.label.c_str());
        ImGui::PopFont();
        ImGui::PushFont(Row());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a key."
                : "Press a key or click a mouse button.");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::CreamDim));
        ImGui::TextUnformatted("Let go of anything you're holding first. Backspace or Delete clears the binding.");
        ImGui::TextUnformatted("Escape can be bound. F1, F2 and F10 are reserved.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Cleared in %d seconds if nothing is pressed", std::max(0, static_cast<int>(std::ceil(remaining))));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Px(8)));
        const ImVec2 size(Px(150), Px(42));
        const bool clear = FlatButton("CLEAR##rebind", false, size);
        ImGui::SameLine(0.0f, Px(12));
        if (FlatButton("CANCEL##rebind", true, size)) g_rebind.active = false;
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

// A key binding: the key's name on the right; A or a click waits for the new key.
void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target) {
    if (MenuRows::Action(label, KeyBindingName(scancode))) BeginRebind(kind, target, label);
}

void DrawPortChoice(const char* description) {
    static constexpr const char* kPorts[] = {"Port 1", "Port 2", "Port 3", "Port 4"};
    static_assert(std::size(kPorts) == PAD_MAX_CONTROLLERS);
    MenuRows::Choice("Port", &g_controllerPort, kPorts, PAD_MAX_CONTROLLERS);
    MenuRows::Describe(description);
}

// Keyboard page: the port's keys, on top of its gamepad (aurora PADRead adds the keys to the gamepad).
void DrawKeyboardSettings() {
    MenuRows::Section("Keyboard and mouse");
    DrawPortChoice("The keyboard drives this port together with its gamepad.");
    const auto port = static_cast<uint32_t>(g_controllerPort);
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (MenuRows::Toggle("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    MenuRows::Describe("Play with the keyboard and mouse on this port, together with a gamepad. F10 opens these settings.");
    if (!enabled) return;
    if (MenuRows::Action("Use the WASD + mouse preset") || usePreset) {
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
    MenuRows::Describe("Sets every key at once: WASD moves, IJKL turns the camera, and the mouse buttons and the keys around "
                       "WASD do the rest.");
    constexpr const char* kKeyHelp = "Select it, then press a key or click a mouse button. Backspace or Delete clears it.";
    MenuRows::Section("Buttons");
    for (uint32_t i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), buttons[i].scancode, RebindKind::KeyboardButton,
                       buttons[i].padButton);
        MenuRows::Describe(kKeyHelp);
        ImGui::PopID();
    }
    MenuRows::Section("Stick and trigger keys");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = direction != nullptr && direction[0] != '\0'
            ? std::string(PADGetAxisName(axes[i].padAxis)) + " (" + direction + ")"
            : std::string(PADGetAxisName(axes[i].padAxis));
        DrawKeyBinding(label.c_str(), axes[i].scancode, RebindKind::KeyboardAxis, axes[i].padAxis);
        MenuRows::Describe(kKeyHelp);
        ImGui::PopID();
    }
}

// Expressions in Dolphin's syntax, combined with the button mapping.
void DrawExpressionSettings() {
    MenuRows::Section("Expressions (Dolphin syntax)");
    MenuRows::Note("Optional. An expression's result is combined with the button mapping: the operators ! & | ^ and the "
                   "functions if, min, max, clamp, timer, toggle, hold, tap, pulse, smooth and deadzone work as in Dolphin.");

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

    if (MenuRows::Action("Import from Dolphin", importStatus.empty() ? nullptr : importStatus.c_str())) {
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
    char where[200];
    std::snprintf(where, sizeof(where),
                  "Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini, or GCPadNew.ini next to the game.",
                  g_controllerPort + 1);
    MenuRows::Describe(where);
    if (!importStatus.empty()) MenuRows::Describe(importStatus.c_str());

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        if (MenuRows::TextField(InputBindings::kControls[i].label, text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        MenuRows::Describe("Select it and type an expression (A or a click starts typing), then press Enter.");
        if (InputBindings::IsActive(port, i)) MenuRows::Describe("Active now.");
        if (!errors[i].empty()) MenuRows::Note(("Error: " + errors[i]).c_str());
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    if (MenuRows::Toggle("Vibration", &g_rumbleEnabled)) {
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
    MenuRows::Describe("Controller vibration, for every port.");
}

// The Xbox's Black / White buttons ([controller] xbox_black / xbox_white): any pad button, trigger or stick direction.
// The pad button sends only Black / White, not the GameCube button it is mapped to.
void DrawXboxButtonSettings() {
    static const std::vector<const NativeButtonItem*> items = [] {
        std::vector<const NativeButtonItem*> list;
        for (const auto& item : kNativeButtons) {
            if (item.nativeButton != PAD_NATIVE_BUTTON_INVALID) list.push_back(&item);  // "Default" means nothing here
        }
        return list;
    }();
    static constexpr const char* kActions[] = {"Black (sneak)", "White (soap bubble)"};
    for (size_t action = 0; action < 2; ++action) {
        const std::string current = RuntimeConfigFile::XboxButton(action);
        const NativeButtonItem* currentItem = ControllerNames::FindNativeButton(current);
        int index = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i] == currentItem) index = static_cast<int>(i);
        }
        const auto name = [&](int i) -> const char* {
            if (i < 0) return current.c_str();
            return items[static_cast<size_t>(i)]->nativeButton == PAD_NATIVE_BUTTON_DISABLED ? "Off"
                                                                                            : items[static_cast<size_t>(i)]->label;
        };
        ImGui::PushID(static_cast<int>(action));
        if (MenuRows::Choice(kActions[action], &index, static_cast<int>(items.size()), name)) {
            RuntimeConfigFile::SetXboxButton(action, items[static_cast<size_t>(index)]->configName);
        }
        ImGui::PopID();
        MenuRows::Describe("The pad button that works as the Xbox's Black or White button: it sends only that, not the "
                           "GameCube button it is mapped to. Hints in the game name it from the next level load.");
    }
}

void DrawGamepadSettings(uint32_t selectedGamePort);

// Controller page: the port, its controller and its buttons.
void DrawControllerSettings() {
    MenuRows::Section("Controller");
    DrawPortChoice("Every connected gamepad controls Scooby (player 1), mapped for its type. The settings on this page "
                   "are for the port chosen here.");
    DrawGamepadSettings(static_cast<uint32_t>(g_controllerPort));
}

void DrawGamepadSettings(uint32_t selectedGamePort) {
    const uint32_t controllerCount = PADCount();
    {
        const int32_t index = PADGetIndexForPort(selectedGamePort);
        int assigned = index < 0 ? 0 : index < static_cast<int32_t>(controllerCount) ? index + 1 : -1;
        const char* portName = PADGetName(selectedGamePort);
        const auto name = [&](int i) -> const char* {
            if (i < 0) return portName != nullptr ? portName : "Unknown controller";
            if (i == 0) return "None";
            const char* controller = PADGetNameForControllerIndex(static_cast<uint32_t>(i - 1));
            return controller != nullptr ? controller : "Unknown controller";
        };
        if (MenuRows::Choice("Controller", &assigned, static_cast<int>(controllerCount) + 1, name)) {
            if (assigned == 0) {
                PADClearPort(selectedGamePort);
            } else {
                PADSetPortForIndex(static_cast<uint32_t>(assigned - 1), selectedGamePort);
            }
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
            ApplyConfiguredMappings();
        }
        MenuRows::Describe(controllerCount == 0 ? "No controller is connected."
                                                : "The connected controller that drives this port. None leaves it free.");
    }
    // hle/no100f_xbox_layout.h: works on top of the button mapping below, which stays as it is.
    if (bool xboxLayout = RuntimeConfigFile::XboxOriginalLayout(); MenuRows::Toggle("Original Xbox controls", &xboxLayout)) {
        RuntimeConfigFile::SetXboxOriginalLayout(xboxLayout);
    }
    MenuRows::Describe("On: A jump, X head smash, B dig / enter / talk and back in menus, Y on-screen display, LT gum, RT run, "
                       "White soap bubble (LB), Black sneak (RB), Start pause; Black and White work together with the "
                       "triggers. The hints say so too from the next level load.\n\nOff (GameCube): B head smash and back, X "
                       "dig, Z + L bubble, Z + R sneak.\n\nGamepads only; the buttons below stay in GameCube buttons either way.");
    if (RuntimeConfigFile::XboxOriginalLayout()) {
        DrawXboxButtonSettings();
    }
    DrawRumbleSettings();  // shown with no controller connected too
    controller_mapping_wizard::DrawSetupList();
    if (controllerCount == 0) {
        MenuRows::Note("Connect a controller to change its buttons.");
        DrawWiiRemoteSettings(selectedGamePort);
        return;
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(selectedGamePort, &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        MenuRows::Note("Choose a controller for this port to change its buttons.");
        DrawWiiRemoteSettings(selectedGamePort);
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings = PADGetAltButtonMappings(selectedGamePort, &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    MenuRows::Section("Button presets");
    // GameCube and Original Xbox both restore the default mapping; Original Xbox then switches the layout on.
    const auto restoreDefaults = [&](bool xboxLayout) {
        RuntimeConfigFile::SetXboxOriginalLayout(xboxLayout);
        PADRestoreDefaultMapping(selectedGamePort);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(selectedGamePort, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        PADSerializeMappings();
        mappings = PADGetButtonMappings(selectedGamePort, &mappingCount);
    };
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(selectedGamePort, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(selectedGamePort,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        PADSerializeMappings();
        mappings = PADGetButtonMappings(selectedGamePort, &mappingCount);
    };
    if (MenuRows::Action("GameCube", "Apply")) restoreDefaults(false);
    MenuRows::Describe("The default buttons, with the GameCube layout.");
    if (MenuRows::Action("Original Xbox", "Apply")) restoreDefaults(true);
    MenuRows::Describe("The default buttons, with Original Xbox controls turned on.");
    if (MenuRows::Action("Classic Controller Pro", "Apply")) applyPreset(kClassicProPreset);
    MenuRows::Describe("The SNES-style diamond (A right, B bottom, X top, Y left), the shoulders as the GameCube triggers "
                       "and Z on Back / Select.");
    if (MenuRows::Action("PlayStation", "Apply")) applyPreset(kPlayStationPreset);
    MenuRows::Describe("The shoulders as the GameCube triggers and Z on Create / Share.");

    MenuRows::Section("Buttons");
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

        ImGui::PushID(static_cast<int>(i));
        const bool altBound = altIt != nullptr && altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
        const int which = MenuRows::Binding(kControllerButtons[i].label, NativeButtonForValue(mappingIt->nativeButton).label,
                                            altBound ? NativeButtonForValue(altIt->nativeButton).label : nullptr,
                                            altIt != nullptr);
        if (which == 1) BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        if (which == 2) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
        }
        MenuRows::Describe("Select it, then press a controller button, pull a trigger or move a stick. A second input (the "
                           "button shown below, or a click on + Second) works as well as the first. With no input for 10 "
                           "seconds the binding is cleared.\n\nLT / L2 and RT / R2 are the triggers, LB / L1 and RB / R1 "
                           "the shoulders.");
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            const std::string label = std::string(kControllerButtons[i].label) +
                                      (secondary ? ": second input's threshold" : ": threshold") +
                                      (secondary ? "##altThreshold" : "##primaryThreshold");
            if (MenuRows::SliderInt(label.c_str(), &threshold, 5, 100, 5, "%d%%")) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
                writeBinding(i, mappingIt->nativeButton, altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
            MenuRows::Describe("How far the trigger or stick has to go before it counts as a press.");
        };
        drawThreshold(mappingIt, false);
        if (altBound) drawThreshold(altIt, true);
        ImGui::PopID();
    }
    DrawWiiRemoteSettings(selectedGamePort);
    DrawExpressionSettings();
}

void DrawAudioSettings() {
    MenuRows::Section("Volume");
    const auto volume = [](const char* label, int& percent, const char* description, auto apply) {
        if (MenuRows::SliderInt(label, &percent, 0, 100, 5, "%d%%")) apply(static_cast<float>(percent) / 100.0f);
        MenuRows::Describe(description);
    };
    volume("Master", g_audioVolumePercent, "Everything the game plays.", [](float v) {
        AudioBackend::Instance().SetMasterVolume(v);
        RuntimeConfigFile::SetAudioVolume(v);
    });
    volume("Music", g_musicVolumePercent, "The music in the levels and menus.", [](float v) {
        MusicAttenuation::SetMusicVolume(v);
        RuntimeConfigFile::SetMusicVolume(v);
    });
    volume("Sound effects", g_soundEffectsVolumePercent, "Footsteps, monsters, power-ups and the rest of the world.", [](float v) {
        MusicAttenuation::SetSoundEffectsVolume(v);
        RuntimeConfigFile::SetSoundEffectsVolume(v);
    });
    volume("Menus", g_uiVolumePercent, "The game's menu sounds.", [](float v) {
        MusicAttenuation::SetUiVolume(v);
        RuntimeConfigFile::SetUiVolume(v);
    });
    volume("Voices", g_voicesVolumePercent, "Scooby, Shaggy and the rest of the gang talking.", [](float v) {
        MusicAttenuation::SetVoicesVolume(v);
        RuntimeConfigFile::SetVoicesVolume(v);
    });

    MenuRows::Section("Mute");
    if (MenuRows::Toggle("Mute all sound", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    MenuRows::Describe("Silences the game. The mute shortcut does the same at any time.");
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0);
    MenuRows::Describe("The key that mutes and unmutes the game at any time. Select it, then press a key.");
    if (MenuRows::Toggle("Mute music while other media plays", &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
#if defined(__APPLE__)
    MenuRows::Describe("Detects other apps with active audio output on macOS 14.2 or later. Apps that keep an output stream "
                       "running silently may keep game music muted.");
#else
    MenuRows::Describe("The game's music goes quiet while another app plays music or video.");
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            MenuRows::Describe("Other media is playing now, so the game's music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            MenuRows::Describe("Checking for other media...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            MenuRows::Describe("Unavailable: needs macOS 14.2 or later.");
#else
            MenuRows::Describe("Unavailable: media controls can't be read on this system.");
#endif
        } else {
            MenuRows::Describe("No other media is playing now.");
        }
    }

    MenuRows::Section("Advanced");
    if (MenuRows::Toggle("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    MenuRows::Describe("Mixes the game's sound off the game thread. Turn this off if you suspect an audio problem; the mix "
                       "then runs inline as it used to.");
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
// HDR calibration: a page inside the Video page (B, Esc or Back go back to it), with live test patches the renderer
// fills in HDR (g_hdrCalibrationFrame).
void DrawHdrCalibration() {
    using namespace OverlayTheme;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    MenuRows::Section("HDR calibration");
    bool hdr = RuntimeConfigFile::Hdr();
    float paper = RuntimeConfigFile::HdrPaperWhiteNits();
    float peak = RuntimeConfigFile::HdrPeakNits();
    static bool saveFailed = false;
    bool brightnessChanged = false;
    if (MenuRows::Toggle("HDR on next start", &hdr)) saveFailed = !RuntimeConfigFile::SetHdr(hdr);
    MenuRows::Describe("Turning HDR on or off takes effect after a restart. Windows HDR settings are not changed.");
    MenuRows::Info("Display", aurora_get_hdr_display_name());
    const auto status = aurora_get_hdr_calibration_status();
    const bool ready = status == AURORA_HDR_CALIBRATION_READY;
    switch (status) {
    case AURORA_HDR_CALIBRATION_READY:
        MenuRows::Note("Live HDR test patches below. The normal picture should feel comfortable; bright highlights should "
                       "keep the checker detail.");
        break;
    case AURORA_HDR_CALIBRATION_OUTPUT_OFF:
        MenuRows::Note("No HDR preview: turn HDR on and restart the game with Windows HDR on.");
        break;
    case AURORA_HDR_CALIBRATION_UNSUPPORTED_BACKEND:
        MenuRows::Note("HDR calibration needs the Direct3D HDR output.");
        break;
    case AURORA_HDR_CALIBRATION_DISPLAY_OFF:
        MenuRows::Note("No HDR preview: Windows HDR is off for the display the game is on.");
        break;
    case AURORA_HDR_CALIBRATION_DISPLAY_UNKNOWN:
        MenuRows::Note("No HDR preview: the display's HDR state couldn't be confirmed.");
        break;
    case AURORA_HDR_CALIBRATION_STAYED_SDR:
        MenuRows::Note("HDR is on in the settings, but Windows HDR was off for the game's display when it started, so the "
                       "game shows the standard picture. Turn Windows HDR on and restart the game.");
        break;
    }
    if (MenuRows::Slider("Paper white", &paper, 80.f, 1000.f, 10.f, "%.0f nits")) {
        saveFailed = !RuntimeConfigFile::SetHdrPaperWhiteNits(paper);
        brightnessChanged = !saveFailed;
    }
    MenuRows::Describe("The brightness of the normal picture: set it for comfortable viewing.");
    if (MenuRows::Slider("Peak highlights", &peak, 80.f, 10000.f, 50.f, "%.0f nits")) {
        saveFailed = !RuntimeConfigFile::SetHdrPeakNits(peak);
        brightnessChanged = brightnessChanged || !saveFailed;
    }
    MenuRows::Describe("The brightest highlights (sparkles, lamps). They never fall below paper white.");
    if (brightnessChanged && !aurora_set_hdr_brightness(RuntimeConfigFile::HdrPaperWhiteNits(), RuntimeConfigFile::HdrPeakNits()))
        saveFailed = true;
    if (peak < paper) MenuRows::Note("Highlights never fall below paper white: the game's peak is the paper-white value.");
    ImGui::Dummy(ImVec2(0, Px(6)));
    const float gap = Px(16);
    const float indent = Px(22);
    const float patchWidth = std::max(Px(80), (ImGui::GetContentRegionAvail().x - indent * 2 - gap) * 0.5f);
    const float patchHeight = std::clamp(viewport->Size.y * 0.16f, Px(70), Px(125));
    auto Patch = [&](const char* title, float nits, float* rect) {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(title);
        ImGui::TextDisabled("%.0f nits target", nits);
        ImGui::Dummy(ImVec2(patchWidth, patchHeight));
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        auto* list = ImGui::GetWindowDrawList();
        list->AddRectFilled(min, max, IM_COL32(0, 0, 0, 255), Px(6));
        if (ready && g_hdrCalibrationOpen && ImGui::IsItemVisible()) {
            const auto clipMin = list->GetClipRectMin();
            const auto clipMax = list->GetClipRectMax();
            rect[0] = (std::max(min.x, clipMin.x) - viewport->Pos.x) / viewport->Size.x;
            rect[1] = (std::max(min.y, clipMin.y) - viewport->Pos.y) / viewport->Size.y;
            rect[2] = (std::min(max.x, clipMax.x) - viewport->Pos.x) / viewport->Size.x;
            rect[3] = (std::min(max.y, clipMax.y) - viewport->Pos.y) / viewport->Size.y;
        } else {
            list->AddText(ImVec2(min.x + Px(10), min.y + Px(10)), UiCol(Colour::CreamDim), "No HDR preview");
        }
        ImGui::EndGroup();
    };
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    Patch("Normal picture", paper, g_hdrCalibrationFrame.paperRect);
    ImGui::SameLine(0.f, gap);
    Patch("Bright highlights", std::max(paper, peak), g_hdrCalibrationFrame.peakRect);
    g_hdrCalibrationFrame.paperWhiteNits = paper;
    g_hdrCalibrationFrame.peakNits = std::max(paper, peak);
    MenuRows::Note("The targets are encoded output values, not measured panel brightness; Windows or the display may "
                   "compress highlights, so this is a visual guide.");
    if (saveFailed) MenuRows::Note("Couldn't apply or save the HDR settings. Check the log and the settings folder's permissions.");
}
#endif

void DrawVideoSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());

    MenuRows::Section("Picture");
    int resolution = -1;
    for (size_t i = 0; i < kResolutions.size(); ++i) {
        if (std::fabs(kResolutions[i].scale - g_resolutionScale) < 0.001f) resolution = static_cast<int>(i);
    }
    if (MenuRows::Choice(
            "Resolution", &resolution, static_cast<int>(kResolutions.size()),
            [](int i) { return i >= 0 ? kResolutions[static_cast<size_t>(i)].label : "Custom"; },
            [](int i) { return !(IsHighFrameRateMode() && IsHighResolutionScale(kResolutions[static_cast<size_t>(i)].scale)); })) {
        SetResolutionScale(kResolutions[static_cast<size_t>(resolution)].scale);
    }
    if (MenuRows::RowSelected()) {
        uint32_t pictureWidth = 0;
        uint32_t pictureHeight = 0;
        AuroraGetPictureSize(&pictureWidth, &pictureHeight);
        char text[200];
        std::snprintf(text, sizeof(text),
                      "How sharply the game is drawn: higher is sharper and harder on the graphics card. Auto matches the "
                      "window or screen.\n\nDrawing at %ux%u now.", pictureWidth, pictureHeight);
        MenuRows::Describe(text);
        if (IsHighFrameRateMode()) MenuRows::Describe("6x and 8x aren't available with frame interpolation on.");
    }

    const DisplaySettings::Settings display = DisplaySettings::Current();
    int aspect = -1;
    for (size_t i = 0; i < kAspects.size(); ++i) {
        if (kAspects[i].mode == display.aspect) aspect = static_cast<int>(i);
    }
    if (MenuRows::Choice("Picture shape", &aspect, static_cast<int>(kAspects.size()),
                         [](int i) { return i >= 0 ? kAspects[static_cast<size_t>(i)].label : "Custom"; })) {
        SetAspectMode(kAspects[static_cast<size_t>(aspect)].mode);
    }
    MenuRows::Describe("Original 4:3 is the picture the game was made for. 16:9 is widescreen, Fill Window stretches to the "
                       "window, and Cinematic is a 2.39:1 letterbox.");
    // The game reads 4:3 versus widescreen once, at boot.
    if ((display.aspect != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()) {
        MenuRows::Note("Switching between 4:3 and widescreen takes effect after a restart.");
    }

    static constexpr const char* kDisplayModes[] = {
        "Window",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (MenuRows::Choice("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    MenuRows::Describe("A window, borderless fullscreen (F11 switches), or exclusive fullscreen, which asks the display for "
                       "the closest mode at the game's frame rate (60 Hz, or the frame interpolation target).");
#if defined(_WIN32)
    // Same row as the launcher's Settings > Display. The list is read each time F10 opens.
    static std::vector<AuroraMonitor> monitors;
    static Clock::time_point monitorsReadFor{};
    const auto readMonitors = [] {
        std::array<AuroraMonitor, 16> found{};
        const uint32_t count = aurora_get_monitors(found.data(), static_cast<uint32_t>(found.size()));
        monitors.assign(found.begin(), found.begin() + count);
    };
    if (monitorsReadFor != g_panelOpenedAt) {
        readMonitors();
        monitorsReadFor = g_panelOpenedAt;
    }
    const bool automaticMonitor = g_fullscreenMonitor.empty() || g_fullscreenMonitor == "auto";
    int monitor = automaticMonitor ? 0 : -1;
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (!automaticMonitor && monitors[i].selected) monitor = static_cast<int>(i) + 1;
    }
    const auto monitorName = [](int i) -> const char* {
        if (i < 0) return "Saved monitor (not connected)";
        return i == 0 ? "Automatic" : monitors[static_cast<size_t>(i - 1)].label;
    };
    if (MenuRows::Choice("Fullscreen monitor", &monitor, static_cast<int>(monitors.size()) + 1, monitorName)) {
        g_fullscreenMonitor = monitor == 0 ? std::string("auto") : std::string(monitors[static_cast<size_t>(monitor - 1)].id);
        aurora_set_fullscreen_monitor(g_fullscreenMonitor.c_str());
        RuntimeConfigFile::SetFullscreenDisplay(g_fullscreenMonitor);
        readMonitors();
    }
    MenuRows::Describe("The screen fullscreen uses (F11 or Display mode). Automatic uses the one the game window is on.");

    MenuRows::Section("HDR");
    if (MenuRows::Action("HDR calibration")) {
        g_hdrCalibrationReturnRow = MenuRows::Selected();
        g_hdrCalibrationOpen = true;
        MenuRows::Select(0);
    }
    MenuRows::Describe("Brightness controls and real HDR test patterns. Turning HDR on or off takes effect after a restart.");
#endif

    MenuRows::Section("Performance");
    if (MenuRows::Toggle("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    MenuRows::Describe("A frame counter in the corner of the screen.");
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (MenuRows::Toggle("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        MenuRows::Describe("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        MenuRows::Describe("Choose a lower resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        MenuRows::Describe("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        MenuRows::Describe("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        MenuRows::Describe(metalFxSupported ? "Render below the output resolution for sharper lower-cost output."
                                            : "MetalFX spatial upscaling is unavailable on this device.");
        break;
    }
#endif

    // The same group as the launcher's Graphics > Experimental.
    MenuRows::Section("Experimental");
    MenuRows::Note("These might show visual glitches.");
    static constexpr const char* kFrameInterpolationModes[] = {"Off", "120 FPS", "180 FPS"};
    // At 30 fps the mode is Off (RuntimeConfigFile::FrameInterpolationFps). fps60 can't change while the game runs.
    const bool frameInterpolationLocked = !RuntimeConfigFile::Fps60();
    ImGui::BeginDisabled(frameInterpolationLocked);
    if (MenuRows::Choice("Frame interpolation", &g_frameInterpolationMode, kFrameInterpolationModes,
                         static_cast<int>(std::size(kFrameInterpolationModes)))) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    ImGui::EndDisabled();
    MenuRows::Describe(frameInterpolationLocked
                           ? "Only works at 60 fps, so it stays off at 30 fps. The launcher sets the frame rate."
                           : "Draws extra frames between the game's own for smoother motion.");
    // A strength set by hand in Config.toml that is none of the levels shows as its percentage.
    const auto levelChoice = [](const char* label, int percent, const auto& levels, const char* description, auto apply) {
        int index = -1;
        for (size_t i = 0; i < std::size(levels); ++i) {
            if (levels[i].percent == percent) index = static_cast<int>(i);
        }
        const std::string custom = std::to_string(percent) + "%";
        if (MenuRows::Choice(label, &index, static_cast<int>(std::size(levels)),
                             [&](int i) { return i >= 0 ? levels[static_cast<size_t>(i)].name : custom.c_str(); })) {
            apply(levels[static_cast<size_t>(index)].percent);
        }
        MenuRows::Describe(description);
    };
    levelChoice("Motion blur", std::clamp(RuntimeConfigFile::MotionBlur(), 0, 100), RuntimeConfigFile::kMotionBlurLevels,
                "Blurs fast motion a little, like a film camera.", [](int percent) {
                    aurora_set_motion_blur(RuntimeConfigFile::MotionBlurShutter(percent));
                    RuntimeConfigFile::SetMotionBlur(percent);
                });
#ifdef AURORA_HAS_INK_OUTLINES
    {
        int level = std::clamp(RuntimeConfigFile::InkOutlines(), 0, 2);
        if (MenuRows::Choice("Ink outlines", &level, RuntimeConfigFile::kInkOutlineLevels, 3)) {
            aurora_set_ink_outlines(level);
            RuntimeConfigFile::SetInkOutlines(level);
        }
        MenuRows::Describe("Cartoon lines around characters and objects. Bold also draws sharp edges inside them.");
    }
#endif
#ifdef AURORA_HAS_SCENE_STAGE
    levelChoice("Ambient occlusion", std::clamp(RuntimeConfigFile::AmbientOcclusion(), 0, 100),
                RuntimeConfigFile::kAmbientOcclusionLevels, "Soft shade where surfaces meet: in corners, creases and under things.",
                [](int percent) {
                    aurora_set_ambient_occlusion(static_cast<float>(percent) / 100.0f);
                    RuntimeConfigFile::SetAmbientOcclusion(percent);
                });
#endif
#ifdef AURORA_HAS_BLOOM
    levelChoice("Bloom", std::clamp(RuntimeConfigFile::Bloom(), 0, 100), RuntimeConfigFile::kBloomLevels,
                "A soft glow around lit windows, lamp light, fire and sparkles.", [](int percent) {
                    aurora_set_bloom(static_cast<float>(percent) / 100.0f);
                    RuntimeConfigFile::SetBloom(percent);
                });
#endif

    MenuRows::Section("Advanced");
    if (MenuRows::Toggle("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    MenuRows::Describe("Skips the console's blur filter on copied frames, for a sharper picture.");
    if (MenuRows::Toggle("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    MenuRows::Describe("Leaves out what isn't ready to draw yet instead of waiting for it, so the game doesn't stutter while "
                       "new shaders compile.");
    MenuRows::Info("Graphics API", GraphicsApiDisplayName());
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

// The speedrun overlay: the run's time (big), the time without loads, the last splits. Before a run: the category and
// how it starts; the save's play time under it.
void DrawSpeedrunOverlay() {
    if (!g_speedrunOverlay || g_topBarVisible) return;  // hidden behind the F10 menu
    using namespace OverlayTheme;
    const no100f_speedrun::View view = no100f_speedrun::CurrentView(static_cast<size_t>(g_speedrunSplits));
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const bool right = g_speedrunCorner == 1 || g_speedrunCorner == 3;
    const bool bottom = g_speedrunCorner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - kMargin : kMargin, bottom ? io.DisplaySize.y - kMargin : kMargin),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Speedrun Overlay", nullptr, kFlags)) {
        ImGui::PushFont(Small());
        ImGui::TextColored(Vec(Colour::CreamDim), "%s", view.category.c_str());
        ImGui::PopFont();
        if (!view.lastSplits.empty() && ImGui::BeginTable("splits", 2)) {
            ImGui::PushFont(Body());
            for (const auto& split : view.lastSplits) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(split.name.c_str());
                ImGui::TableNextColumn();
                const std::string time = no100f_speedrun::FormatTime(split.realTime);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() -
                                     ImGui::CalcTextSize(time.c_str()).x);
                ImGui::TextUnformatted(time.c_str());
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
        ImGui::PushFont(Row());
        ImGui::SetWindowFontScale(2.0f);
        const ImVec4 colour = view.finished ? Vec(Colour::Gold) : view.running ? Vec(Colour::Lime) : Vec(Colour::CreamDim);
        ImGui::TextColored(colour, "%s", no100f_speedrun::FormatTime(view.realTime).c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::PushFont(Small());
        if (g_speedrunLoadless && view.started) {
            ImGui::TextColored(Vec(Colour::CreamDim), "Without loads %s",
                               no100f_speedrun::FormatTime(view.withoutLoads).c_str());
        }
        if (!view.started) ImGui::TextColored(Vec(Colour::CreamDim), "Starts when you press New Game");
        if (view.playKnown) {
            ImGui::TextColored(Vec(Colour::CreamDim), "Play time on this save %s",
                               no100f_speedrun::FormatClock(view.playSeconds).c_str());
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
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
    PushPromptStyle();
    const bool open = ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    PopPromptStyle();
    if (!open) return;
    // Opened from the pad: show where the cursor is (on Cancel).
    if (ImGui::IsWindowAppearing() && MenuRows::PadInput()) ImGui::SetNavCursorVisible(true);
    ImGui::PushFont(Tab());
    ImGui::TextColored(Vec(Colour::Lime), "QUIT THE GAME?");
    ImGui::PopFont();
    ImGui::PushFont(Row());
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::CreamDim));
    ImGui::TextUnformatted("Progress since your last save is lost.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, Px(8)));
    const ImVec2 size(Px(160), Px(44));
    // Unwind at the next guest event boundary, not through the noexcept draw callback.
    if (FlatButton("QUIT##confirm", false, size)) aurora::window_owner::request_close();
    ImGui::SameLine(0.0f, Px(12));
    if (FlatButton("CANCEL##cancel", true, size)) g_exitPromptOpen = false;
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

// The settings menu (F10), made for a controller: the page tabs across the top (LB / RB, Q / E, Page Up / Down or a
// click change the page), the page's rows on the left (menu_rows.h), the selected row's details on the right, and the
// buttons that do something at the bottom. It fills the window over the dimmed, paused game.
enum class Page { Video, Audio, Controller, Keyboard, LanPlay, Speedrun, About };
struct PageItem {
    Page page;
    const char* label;
};
// LAN Play ({Page::LanPlay, "LAN Play"}) is hidden until network play is worked on; its page code stays.
constexpr std::array<PageItem, 6> kPages = {{
    {Page::Video, "Video"}, {Page::Audio, "Audio"}, {Page::Controller, "Controller"}, {Page::Keyboard, "Keyboard"},
    {Page::Speedrun, "Speedrun"}, {Page::About, "About"},
}};
Page g_page = Page::Video;
// Each page's selected row, so coming back to a page finds it where it was.
std::array<int, 7> g_pageRows{};

void SetPage(Page page) {
    if (page == g_page && !g_hdrCalibrationOpen) return;
    g_pageRows[static_cast<size_t>(g_page)] = MenuRows::Selected();
    if (g_hdrCalibrationOpen) {
        g_hdrCalibrationOpen = false;
        g_hdrCalibrationFrame = {};
        g_pageRows[static_cast<size_t>(Page::Video)] = g_hdrCalibrationReturnRow;
    }
    g_page = page;
    MenuRows::Select(g_pageRows[static_cast<size_t>(page)]);
}

void StepPage(int direction) {
    const int count = static_cast<int>(kPages.size());
    int index = 0;  // the page's place in the list (not its enum value: a hidden page leaves a gap)
    while (index < count && kPages[index].page != g_page) ++index;
    if (index == count) index = 0;
    SetPage(kPages[((index + direction) % count + count) % count].page);
}

void DrawSpeedrunPage() {
    namespace sr = no100f_speedrun;
    MenuRows::Section("Speedrun timer");
    if (MenuRows::Toggle("Speedrun overlay", &g_speedrunOverlay)) RuntimeConfigFile::SetSpeedrunOverlay(g_speedrunOverlay);
    MenuRows::Describe("The run's time in a corner of the screen, with your last splits. As on speedrun.com, a run starts "
                       "when you press New Game; a boss splits when its defeat movie starts. The timer runs whether the "
                       "overlay shows or not.");
    if (MenuRows::Choice("Category", &g_speedrunCategory, static_cast<int>(sr::kCategories.size()),
                         [](int i) { return i < 0 ? "?" : sr::kCategories[static_cast<size_t>(i)].name; })) {
        RuntimeConfigFile::SetSpeedrunCategory(sr::kCategories[static_cast<size_t>(g_speedrunCategory)].id);
        sr::SetNextCategory(g_speedrunCategory);
    }
    static std::string categoryText;
    categoryText = std::string(sr::kCategories[static_cast<size_t>(g_speedrunCategory)].rules) +
                   " The categories are speedrun.com's. A change counts from the next New Game. Warping out of a boss "
                   "before its movie (allowed in All Bosses and Reverse Boss Order) skips its split.";
    MenuRows::Describe(categoryText.c_str());
    if (MenuRows::SliderInt("Splits shown", &g_speedrunSplits, 0, 8, 1, "%d")) {
        RuntimeConfigFile::SetSpeedrunSplits(g_speedrunSplits);
    }
    MenuRows::Describe("How many of the last splits the overlay lists above the time.");
    if (MenuRows::Toggle("Time without loads", &g_speedrunLoadless)) RuntimeConfigFile::SetSpeedrunLoadless(g_speedrunLoadless);
    MenuRows::Describe("A second, smaller time under the run's time, less the loading between scenes. speedrun.com times "
                       "runs in real time; this one is for comparing your own runs.");
    int corner = g_speedrunCorner;
    if (MenuRows::Choice("Corner", &corner, kSpeedrunCornerNames.data(), static_cast<int>(kSpeedrunCornerNames.size()))) {
        g_speedrunCorner = corner;
        RuntimeConfigFile::SetSpeedrunCorner(kSpeedrunCornerIds[static_cast<size_t>(corner)]);
    }
    MenuRows::Describe("Where on the screen the overlay goes.");
    if (MenuRows::Toggle("Save each run", &g_speedrunSaveRuns)) RuntimeConfigFile::SetSpeedrunSaveRuns(g_speedrunSaveRuns);
    MenuRows::Describe("When a run ends, its splits go to the Speedruns folder beside your saves: a spreadsheet (CSV) and "
                       "a LiveSplit splits file, both named by the date and the category.");
    if (MenuRows::Action("Open the Speedruns folder")) {
        const std::string folder = sr::RunsFolder();
        if (!SDL_OpenURL(sr::FileUrl(folder).c_str())) ShowNotice("Couldn't open " + folder);
    }
    MenuRows::Describe("Opens the folder with your saved runs.");

    const sr::View view = sr::CurrentView(1);
    MenuRows::Section(view.finished ? "Last run" : "This run");
    if (!view.started) {
        MenuRows::Note("No run yet: it starts when you press New Game on the title screen.");
    } else {
        MenuRows::Info("Category", view.category.c_str());
        MenuRows::Info(view.finished ? "Time" : "Time so far", sr::FormatTime(view.realTime).c_str());
        MenuRows::Info("Without loads", sr::FormatTime(view.withoutLoads).c_str());
        if (!view.lastSplits.empty()) {
            const auto& split = view.lastSplits.back();
            MenuRows::Info(("Last split: " + split.name).c_str(), sr::FormatTime(split.realTime).c_str());
        }
        if (!view.lastRunFile.empty()) MenuRows::Note(("Saved as " + view.lastRunFile).c_str());
    }

    MenuRows::Section("Play time");
    const std::string played = view.playKnown ? sr::FormatPlayTime(view.playSeconds) : "Load a save or start a new game";
    MenuRows::Info("On this save", played.c_str());
    MenuRows::Note("Counted while you play or pause, not during loads or movies, and kept when you save. Saves from before "
                   "this version start at 0. Your Discord status shows it too.");
}

void DrawAboutPage() {
    MenuRows::Section("FrightWorks: Night of 100 Frights");
    MenuRows::Note(no100f_credits::Line().c_str());
    MenuRows::Note("A native PC port of Scooby-Doo! Night of 100 Frights (GameCube), built on WiiCompiled and Aurora. It runs "
                   "your own copy of the game.");
    if (MenuRows::Action("Project page", "F1")) {
        if (!SDL_OpenURL(kProjectUrl)) ShowNotice(std::string("Couldn't open ") + kProjectUrl);
    }
    MenuRows::Describe("Opens the project's page in your browser: news, help and bug reports.");
    MenuRows::Section("This build");
    MenuRows::Info("Version", BuildInfo::Version());
    if (!BuildInfo::Release() && BuildInfo::Commit()[0] != '\0') {
        MenuRows::Info("Branch", BuildInfo::Branch());
        MenuRows::Info("Commit", BuildInfo::Commit());
        if (BuildInfo::Changes()[0] != '\0') {
            MenuRows::Info("Uncommitted changes", (std::string("+") + BuildInfo::Changes()).c_str());
        }
        MenuRows::Note("A test build: mention its branch and commit when you report a bug.");
    }
    MenuRows::Info("Graphics API", GraphicsApiDisplayName());
    if (no100f_devmenu::Enabled()) {
        MenuRows::Note("Developer tools are on: F8 opens the dev menu, F3 records a video.");
    }
    MenuRows::Section("Keys");
    // Pictures\FrightWorks (Windows) or Pictures/FrightWorks (Linux) when installed.
    static const std::string screenshotHint = [] {
        if (RuntimeConfigFile::PortableRootDirectory()) {
            return std::string("Screenshot (Screenshots folder beside the launcher)");
        }
        std::string folder = RuntimeConfigFile::CaptureDirectoryLabel(false);
        folder.pop_back();  // the label ends in a separator
        return "Screenshot (" + folder + ")";
    }();
    MenuRows::Info("Settings, and back to the game", "F10 / Esc / Back");
    MenuRows::Info("Fullscreen or window", "F11");
    MenuRows::Info(screenshotHint.c_str(), "F2");
    MenuRows::Info("Project page in your browser", "F1");
    MenuRows::Info("Dev menu (developer tools on)", "F8");
    MenuRows::Info("Video recording (developer tools on, needs ffmpeg)", "F3");
    MenuRows::Info("Change the page here", "LB / RB, Q / E");
    MenuRows::Info("Back to the game from here", "B / Start / Esc");
}

void DrawPage(Page page) {
    switch (page) {
    case Page::Video:
#if defined(_WIN32)
        if (g_hdrCalibrationOpen) {
            DrawHdrCalibration();
            break;
        }
#endif
        DrawVideoSettings();
        break;
    case Page::Audio: DrawAudioSettings(); break;
    case Page::Controller: DrawControllerSettings(); break;
    case Page::Keyboard: DrawKeyboardSettings(); break;
    case Page::LanPlay: DrawLanPlaySettings(); break;
    case Page::Speedrun: DrawSpeedrunPage(); break;
    case Page::About: DrawAboutPage(); break;
    }
}

// The menu's background over the dimmed game: see-through enough that the paused game shows behind it, darker towards
// the bottom.
constexpr float kMenuAlphaTop = 0.55f;
constexpr float kMenuAlphaBottom = 0.75f;
// The widest the menu gets (before the UI scale); a wider window centres it.
constexpr float kMenuMaxWidth = 1320.0f;

// A button prompt at the bottom of the menu (or beside the tabs): the button's pictures, then what it does. Without
// the pictures the button's name sits on a little chip. A click on it does the same as the button.
enum class PromptAction { None, Resume, Page, Quit };
struct Prompt {
    std::vector<ButtonIcon> icons;
    std::string chip;
    const char* label = "";
    PromptAction action = PromptAction::None;

    bool HasIcons() const {
        return !icons.empty() && std::all_of(icons.begin(), icons.end(), [](const ButtonIcon& i) { return i.id != ImTextureID{}; });
    }
};

float PromptWidth(const Prompt& prompt, float height) {
    using namespace OverlayTheme;
    float width = 0.0f;
    if (prompt.HasIcons()) {
        for (const ButtonIcon& icon : prompt.icons) width += height * icon.aspect + Px(3);
        width -= Px(3);
    } else if (!prompt.chip.empty()) {
        width = Small()->CalcTextSizeA(Small()->FontSize, FLT_MAX, 0.0f, prompt.chip.c_str()).x + Px(16);
    }
    if (prompt.label[0] != '\0') {
        width += (width > 0.0f ? Px(8) : 0.0f) + Body()->CalcTextSizeA(Body()->FontSize, FLT_MAX, 0.0f, prompt.label).x;
    }
    return width;
}

// Draws a prompt at `pos`, `height` tall; true when it was clicked.
bool DrawPrompt(ImDrawList* list, ImVec2 pos, float height, const Prompt& prompt, const char* id) {
    using namespace OverlayTheme;
    float x = pos.x;
    const float mid = pos.y + height * 0.5f;
    if (prompt.HasIcons()) {
        for (const ButtonIcon& icon : prompt.icons) {
            const float w = height * icon.aspect;
            list->AddImage(icon.id, ImVec2(x, pos.y), ImVec2(x + w, pos.y + height), icon.uv0, icon.uv1,
                           ImGui::GetColorU32(IM_COL32_WHITE));
            x += w + Px(3);
        }
        x -= Px(3);
    } else if (!prompt.chip.empty()) {
        ImFont* small = Small();
        const ImVec2 text = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, prompt.chip.c_str());
        const ImVec2 chipMin(x, mid - height * 0.42f), chipMax(x + text.x + Px(16), mid + height * 0.42f);
        list->AddRectFilled(chipMin, chipMax, UiCol(Colour::Cream, 0.92f), height * 0.3f);
        list->AddText(small, small->FontSize, ImVec2(x + Px(8), std::round(mid - text.y * 0.5f)), UiCol(Colour::Ink),
                      prompt.chip.c_str());
        x = chipMax.x;
    }
    if (prompt.label[0] != '\0') {
        if (x > pos.x) x += Px(8);
        ImFont* body = Body();
        list->AddText(body, body->FontSize, ImVec2(x, std::round(mid - body->FontSize * 0.5f)), UiCol(Colour::Cream),
                      prompt.label);
    }
    if (prompt.action == PromptAction::None) return false;
    const float width = PromptWidth(prompt, height);
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(width, 1.0f), height));
    if (ImGui::IsItemHovered()) {
        list->AddLine(ImVec2(pos.x, pos.y + height + Px(3)), ImVec2(pos.x + width, pos.y + height + Px(3)),
                      UiCol(Colour::Lime, 0.8f), Px(2));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return clicked;
}

// The controller's button names by family (the icon pack folder ends in xbox, playstation or nintendo), for chips when
// the pack has no picture.
const char* PadButtonName(const std::string& folder, const char* button) {
    const bool ps = FolderIs(folder, "playstation");
    const bool nin = FolderIs(folder, "nintendo");
    const std::string_view b = button;
    if (b == "south") return ps ? "Cross" : nin ? "B" : "A";
    if (b == "east") return ps ? "Circle" : nin ? "A" : "B";
    if (b == "west") return ps ? "Square" : nin ? "Y" : "X";
    if (b == "north") return ps ? "Triangle" : nin ? "X" : "Y";
    if (b == "left_shoulder") return ps ? "L1" : nin ? "L" : "LB";
    if (b == "right_shoulder") return ps ? "R1" : nin ? "R" : "RB";
    return "D-pad";
}

struct PromptSources {
    bool pad = false;
    std::string padFolder;
    std::string keyFolder;
};

PromptSources CurrentPromptSources() {
    PromptSources sources;
    sources.pad = MenuRows::PadInput();
    static const std::string iconRoot = RuntimeConfigFile::PathToUtf8(
        RuntimeConfigFile::ExecutableDirectory().value_or(std::filesystem::path{}) / "button_icons");
    sources.keyFolder = iconRoot + "/keyboard";
    char folder[1024] = {};
    const size_t length = aurora_get_button_icon_folder(folder, sizeof(folder));
    sources.padFolder = length > 0 && length < sizeof(folder) ? std::string(folder, length) : iconRoot + "/xbox";
    return sources;
}

Prompt PadPrompt(const PromptSources& sources, std::initializer_list<const char*> buttons, const char* label,
                 PromptAction action = PromptAction::None) {
    Prompt prompt;
    prompt.label = label;
    prompt.action = action;
    for (const char* button : buttons) {
        prompt.icons.push_back(LoadButtonIcon(sources.padFolder, button));
        if (!prompt.chip.empty()) prompt.chip += " / ";
        prompt.chip += PadButtonName(sources.padFolder, button);
    }
    return prompt;
}

Prompt KeyPrompt(const PromptSources& sources, std::initializer_list<const char*> keys, const char* chip, const char* label,
                 PromptAction action = PromptAction::None) {
    Prompt prompt;
    prompt.label = label;
    prompt.action = action;
    prompt.chip = chip;
    for (const char* key : keys) prompt.icons.push_back(LoadButtonIcon(sources.keyFolder, key));
    return prompt;
}

std::vector<Prompt> FooterPrompts(const PromptSources& sources) {
    std::vector<Prompt> prompts;
    const MenuRows::Kind kind = MenuRows::SelectedKind();
    const char* back = g_hdrCalibrationOpen ? "Back" : "Resume";
    if (sources.pad) {
        switch (kind) {
        case MenuRows::Kind::Choice: prompts.push_back(PadPrompt(sources, {"dpad_horizontal"}, "Change")); break;
        case MenuRows::Kind::Slider: prompts.push_back(PadPrompt(sources, {"dpad_horizontal"}, "Adjust")); break;
        case MenuRows::Kind::Toggle: prompts.push_back(PadPrompt(sources, {"south"}, "Switch")); break;
        case MenuRows::Kind::Action: prompts.push_back(PadPrompt(sources, {"south"}, "Select")); break;
        case MenuRows::Kind::Binding:
            prompts.push_back(PadPrompt(sources, {"south"}, "Change"));
            prompts.push_back(PadPrompt(sources, {"west"}, "Second input"));
            break;
        case MenuRows::Kind::Text: prompts.push_back(PadPrompt(sources, {"south"}, "Type")); break;
        case MenuRows::Kind::None: break;
        }
        prompts.push_back(PadPrompt(sources, {"east"}, back, PromptAction::Resume));
        prompts.push_back(PadPrompt(sources, {"left_shoulder", "right_shoulder"}, "Page", PromptAction::Page));
        prompts.push_back(PadPrompt(sources, {"north"}, "Quit game", PromptAction::Quit));
    } else {
        prompts.push_back(KeyPrompt(sources, {"arrows_vertical"}, "Up / Down", "Move"));
        switch (kind) {
        case MenuRows::Kind::Choice: prompts.push_back(KeyPrompt(sources, {"arrows_horizontal"}, "Left / Right", "Change")); break;
        case MenuRows::Kind::Slider: prompts.push_back(KeyPrompt(sources, {"arrows_horizontal"}, "Left / Right", "Adjust")); break;
        case MenuRows::Kind::Toggle: prompts.push_back(KeyPrompt(sources, {"enter"}, "Enter", "Switch")); break;
        case MenuRows::Kind::Action: prompts.push_back(KeyPrompt(sources, {"enter"}, "Enter", "Select")); break;
        case MenuRows::Kind::Binding: prompts.push_back(KeyPrompt(sources, {"enter"}, "Enter", "Change")); break;
        case MenuRows::Kind::Text: prompts.push_back(KeyPrompt(sources, {"enter"}, "Enter", "Type")); break;
        case MenuRows::Kind::None: break;
        }
        prompts.push_back(KeyPrompt(sources, {"escape"}, "Esc", back, PromptAction::Resume));
        prompts.push_back(KeyPrompt(sources, {"q", "e"}, "Q / E", "Page", PromptAction::Page));
        Prompt quit;
        quit.label = "Quit game";
        quit.action = PromptAction::Quit;
        prompts.push_back(quit);
    }
    return prompts;
}

void RunPromptAction(PromptAction action, int direction = 1) {
    switch (action) {
    case PromptAction::Resume:
        if (g_hdrCalibrationOpen) CloseHdrCalibration();
        else SetTopBarVisible(false);
        break;
    case PromptAction::Page: StepPage(direction); break;
    case PromptAction::Quit: g_exitPromptOpen = true; break;
    case PromptAction::None: break;
    }
}

void DrawTopBar() {
    using namespace OverlayTheme;
    if (!g_topBarVisible) {
        return;
    }
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

    // The menu fills the window (maintainer, 2026-11-25). It fades in without sliding: a slide would uncover a strip of
    // the window's edge.
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    if (g_panelFocusPending) {
        ImGui::SetNextWindowFocus();
        g_panelFocusPending = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(20)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // The menu reads the pad and keys itself (menu_rows.h): no ImGui navigation here.
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs;
    const bool open = ImGui::Begin("FrightWorks settings", nullptr, kFlags);
    ImGui::PopStyleVar(2);
    if (open) {
        ImGuiContext& g = *GImGui;
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
        list->AddRectFilledMultiColor(p0, p1, UiCol(Colour::BoardTop, kMenuAlphaTop), UiCol(Colour::BoardTop, kMenuAlphaTop),
                                      UiCol(Colour::BoardBottom, kMenuAlphaBottom), UiCol(Colour::BoardBottom, kMenuAlphaBottom));

        // The list has the pad and keys unless a prompt, a popup or a text field has them.
        const bool input = !g_rebind.active && !g_exitPromptOpen && !controller_mapping_wizard::IsActive() &&
                           !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive();
        if (input) {
            if (MenuRows::Pressed(ImGuiKey_GamepadL1) || MenuRows::Pressed(ImGuiKey_Q) || MenuRows::Pressed(ImGuiKey_PageUp)) {
                StepPage(-1);
            } else if (MenuRows::Pressed(ImGuiKey_GamepadR1) || MenuRows::Pressed(ImGuiKey_E) ||
                       MenuRows::Pressed(ImGuiKey_PageDown)) {
                StepPage(1);
            }
            if (MenuRows::Pressed(ImGuiKey_NavGamepadCancel)) RunPromptAction(PromptAction::Resume);
            if (MenuRows::Pressed(ImGuiKey_GamepadStart)) SetTopBarVisible(false);
            if (MenuRows::Pressed(ImGuiKey_GamepadFaceUp)) g_exitPromptOpen = true;
        }
        const PromptSources sources = CurrentPromptSources();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float bodyWidth = std::min(avail, Px(kMenuMaxWidth));
        const float left = std::round(origin.x + (avail - bodyWidth) * 0.5f);
        const float right = left + bodyWidth;

        // Header: the title, and the page tabs between the shoulder buttons (centred in the window when they fit beside
        // the title, else under it).
        ImFont* title = Title();
        constexpr const char* kTitle = "SETTINGS";
        const ImVec2 titleSize = title->CalcTextSizeA(title->FontSize, FLT_MAX, 0.0f, kTitle);
        OutlinedText(list, title, ImVec2(left, origin.y), UiCol(Colour::Lime), UiCol(Colour::Ink), Px(3), kTitle);
        ImFont* tabFont = Tab();
        const float tabGap = Px(30);
        float tabsWidth = 0.0f;
        for (const auto& item : kPages) {
            std::string label = item.label;
            std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::toupper(c); });
            tabsWidth += tabFont->CalcTextSizeA(tabFont->FontSize, FLT_MAX, 0.0f, label.c_str()).x;
        }
        tabsWidth += tabGap * static_cast<float>(kPages.size() - 1);
        const float shoulderHeight = std::round(tabFont->FontSize + Px(2));
        Prompt previous = sources.pad ? PadPrompt(sources, {"left_shoulder"}, "", PromptAction::Page)
                                      : KeyPrompt(sources, {"q"}, "Q", "", PromptAction::Page);
        Prompt next = sources.pad ? PadPrompt(sources, {"right_shoulder"}, "", PromptAction::Page)
                                  : KeyPrompt(sources, {"e"}, "E", "", PromptAction::Page);
        const float shoulderGap = Px(22);
        const float stripWidth = PromptWidth(previous, shoulderHeight) + PromptWidth(next, shoulderHeight) + shoulderGap * 2 + tabsWidth;
        float stripX = std::round(p0.x + (ImGui::GetWindowWidth() - stripWidth) * 0.5f);
        float stripY = std::round(origin.y + (titleSize.y - tabFont->FontSize) * 0.5f);
        if (stripX < left + titleSize.x + Px(28)) {
            stripX = left + titleSize.x + Px(28);
            if (stripX + stripWidth > right) {
                stripX = left;
                stripY = std::round(origin.y + titleSize.y + Px(10));
            }
        }
        float x = stripX;
        if (DrawPrompt(list, ImVec2(x, stripY + (tabFont->FontSize - shoulderHeight) * 0.5f), shoulderHeight, previous, "##tabprev")) {
            StepPage(-1);
        }
        x += PromptWidth(previous, shoulderHeight) + shoulderGap;
        for (const auto& item : kPages) {
            std::string label = item.label;
            std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::toupper(c); });
            const ImVec2 size = tabFont->CalcTextSizeA(tabFont->FontSize, FLT_MAX, 0.0f, label.c_str());
            ImGui::SetCursorScreenPos(ImVec2(x - Px(6), stripY - Px(6)));
            ImGui::PushID(static_cast<int>(item.page));
            if (ImGui::InvisibleButton("##tab", ImVec2(size.x + Px(12), size.y + Px(16)))) SetPage(item.page);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool selected = item.page == g_page;
            list->AddText(tabFont, tabFont->FontSize, ImVec2(x, stripY),
                          selected ? UiCol(Colour::Lime) : hovered ? UiCol(Colour::Cream) : UiCol(Colour::CreamDim, 0.75f),
                          label.c_str());
            if (selected) {
                list->AddRectFilled(ImVec2(x, stripY + size.y + Px(4)), ImVec2(x + size.x, stripY + size.y + Px(8)),
                                    UiCol(Colour::Lime), Px(2));
            }
            x += size.x + tabGap;
        }
        x += shoulderGap - tabGap;
        if (DrawPrompt(list, ImVec2(x, stripY + (tabFont->FontSize - shoulderHeight) * 0.5f), shoulderHeight, next, "##tabnext")) {
            StepPage(1);
        }
        const float headerBottom = std::max(origin.y + titleSize.y, stripY + tabFont->FontSize + Px(8));
        const float headerRule = std::round(headerBottom + Px(14));
        list->AddLine(ImVec2(left, headerRule), ImVec2(right, headerRule), UiCol(Colour::Purple, 0.6f), Px(1));

        // Footer: the prompts, and the build on the right. With Developer tools on, the DEV mark (DrawDevMark) takes the
        // window's bottom-left corner: the footer stays above it.
        const float devMark = no100f_devmenu::Enabled() ? Heading()->FontSize + Px(14) : 0.0f;
        const float promptHeight = std::round(Body()->FontSize + Px(12));
        const float footerTop = std::round(p1.y - Px(20) - devMark - promptHeight);
        const float footerRule = footerTop - Px(14);
        list->AddLine(ImVec2(left, footerRule), ImVec2(right, footerRule), UiCol(Colour::Purple, 0.6f), Px(1));

        // Body: the rows, and the details of the selected one beside them (under them in a narrow window).
        const float bodyTop = headerRule + Px(14);
        const float bodyBottom = footerRule - Px(14);
        const bool sideDetails = bodyWidth >= Px(820);
        const float gap = Px(24);
        const float detailsWidth = sideDetails ? std::round(std::clamp(bodyWidth * 0.32f, Px(260), Px(420))) : bodyWidth;
        const float stripHeight = sideDetails ? 0.0f : std::round(Body()->FontSize * 4 + Px(36));
        const ImRect listRect(ImVec2(left, bodyTop),
                              ImVec2(sideDetails ? right - detailsWidth - gap : right,
                                     sideDetails ? bodyBottom : bodyBottom - stripHeight - Px(12)));
        const ImRect detailsRect = sideDetails ? ImRect(ImVec2(right - detailsWidth, bodyTop), ImVec2(right, bodyBottom))
                                               : ImRect(ImVec2(left, bodyBottom - stripHeight), ImVec2(right, bodyBottom));

        ImGui::SetCursorScreenPos(listRect.Min);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(4), Px(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8), Px(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        if (ImGui::BeginChild("##page", listRect.GetSize(), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoBackground)) {
            MenuRows::Begin(input);
            ImGui::PushID(static_cast<int>(g_page) + (g_hdrCalibrationOpen ? 100 : 0));
            DrawPage(g_page);
            ImGui::PopID();
            MenuRows::End();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        // Details of the selected row.
        Card(list, detailsRect.Min, detailsRect.Max, Px(14), 0.85f);
        if (!MenuRows::DetailTitle().empty()) {
            const ImVec2 pad(Px(20), Px(16));
            ImGui::SetCursorScreenPos(ImVec2(detailsRect.Min.x + pad.x, detailsRect.Min.y + pad.y));
            const ImVec2 size(detailsRect.GetWidth() - pad.x * 2, detailsRect.GetHeight() - pad.y * 2);
            if (size.x > 0.0f && size.y > 0.0f &&
                ImGui::BeginChild("##details", size, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoBackground)) {
                ImGui::PushFont(Heading());
                ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::Lime));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(MenuRows::DetailTitle().c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (!MenuRows::DetailText().empty()) {
                    ImGui::Dummy(ImVec2(0, Px(4)));
                    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Colour::Cream, 0.88f));
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(MenuRows::DetailText().c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            }
            if (size.x > 0.0f && size.y > 0.0f) ImGui::EndChild();
        }

        // Footer prompts, left to right, as many as fit beside the build.
        const char* build = BuildInfo::Id();
        ImFont* small = Small();
        const float buildWidth = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, build).x;
        float promptX = left;
        const std::vector<Prompt> prompts = FooterPrompts(sources);
        for (size_t i = 0; i < prompts.size(); ++i) {
            const float width = PromptWidth(prompts[i], promptHeight);
            if (promptX + width > right) break;
            ImGui::PushID(static_cast<int>(i));
            if (DrawPrompt(list, ImVec2(promptX, footerTop), promptHeight, prompts[i], "##prompt")) {
                RunPromptAction(prompts[i].action);
            }
            ImGui::PopID();
            promptX += width + Px(26);
        }
        // With Developer tools on, the DEV mark under the footer already shows the build.
        if (!no100f_devmenu::Enabled() && promptX + Px(10) < right - buildWidth) {
            list->AddText(small, small->FontSize,
                          ImVec2(right - buildWidth, std::round(footerTop + (promptHeight - small->FontSize) * 0.5f)),
                          UiCol(Colour::CreamDim, 0.7f), build);
        }
        (void)g;
        DrawRebindPrompt();
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
        if (!g_rebind.active) no100f_devmenu::HandleSdlEvent(ev->sdl, g_topBarVisible);
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
                CloseHdrCalibration();
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

uint16_t PressedButtons() noexcept {
    if (g_pausePressReads <= 0) return 0;
    --g_pausePressReads;
    return PAD_BUTTON_START;
}

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
    DrawSpeedrunOverlay();
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
    UpdateF10Pause();
    aurora_set_hdr_calibration(&g_hdrCalibrationFrame);
}
} // namespace settings_overlay
