#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<bool> forceAspect169;
    std::optional<int32_t> aspectMode;  // 0 original 4:3, 1 16:9, 2 fill the window (DisplaySettings)
    std::optional<int32_t> motionBlur;  // [video] motion_blur: strength in percent, 0 = off (MotionBlurShutter)
    std::optional<int32_t> ambientOcclusion;  // [video] ambient_occlusion: strength in percent, 0 = off
    std::optional<int32_t> inkOutlines;  // [video] ink_outlines: 0 off, 1 thin, 2 bold
    std::optional<int32_t> gpuCompat;  // 0 auto (by graphics adapter), 1 on, 2 off (AuroraGpuCompat)
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<std::string> fullscreenDisplay;  // "auto" or an AuroraMonitor id
    std::optional<uint32_t> frameInterpolationFps;
    std::optional<bool> metalFxSpatialUpscaling;
    std::optional<bool> skipUnreadyPipelines;
    std::optional<bool> disableCopyFilter;
    std::optional<bool> textureReplacements;
    std::optional<int> cutoutMode;  // [video] cutout_mode, see CutoutMode()
    std::optional<bool> textureDumps;
    std::optional<bool> showFps;
    std::optional<bool> skipIntro;
    std::optional<bool> fps60;
    std::optional<std::string> holiday;  // NO100F [game] holiday: "auto" (today), "off", "confetti" or "MM-DD"
    std::optional<bool> devTools;        // NO100F [game] dev_tools: F8 dev menu + DEV build mark (launcher Extras)
    std::optional<bool> recordAudio;     // NO100F [game] record_audio: F3 videos (dev tools) include the game's sound
    std::optional<bool> captureOverlay;  // NO100F [game] capture_overlay: F2/F3 captures show the F8/F10 menus (dev tools)
    std::optional<uint32_t> msaa;
    std::optional<bool> spinningCoins;  // [video] spinning_coins, animated tiles
    std::optional<bool> frameGraph;     // [video] frame_graph, frame times under the FPS counter
    std::optional<bool> hdr;             // [video] hdr: scRGB HDR output (startup only)
    std::optional<float> hdrPaperWhiteNits;
    std::optional<float> hdrPeakNits;
    std::optional<bool> vsync;          // [video] vsync, presents wait for the display (Windows)
    std::optional<uint32_t> disabledPostProcessingPaths;
    std::optional<float> audioVolume;
    std::optional<float> audioMusicVolume;
    std::optional<float> audioSoundEffectsVolume;
    std::optional<float> audioUiVolume;
    std::optional<float> audioVoicesVolume;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    std::optional<bool> attenuateMusicWhenMediaPlays;
    // Real Wii Remotes (with or without Nunchuk / Classic Controller) and Wii U Pro
    // Controllers paired over Bluetooth, driven by SDL's HIDAPI Wii driver. The driver
    // is opt-in on SDL's side, so this decides whether the runtime turns it on.
    std::optional<bool> wiiRemotes;
    // Keep re-enumerating Bluetooth HID devices while no Wii controller is connected
    // (Dolphin's "continuous scanning"), so a remote that dropped or was switched on
    // after launch shows up without restarting.
    std::optional<bool> wiiContinuousScan;
    // Accelerometer zero-point correction for the Bluetooth Wii Remote, in g and in
    // SDL's sensor frame (x right, y out of the button face, z towards the user).
    // SDL's Wii driver falls back to a nominal zero point when its read of the
    // remote's calibration block times out (common over Bluetooth), so this is
    // measured in the overlay with the remote at rest.
    std::optional<double> wiiAccelOffsetX;
    std::optional<double> wiiAccelOffsetY;
    std::optional<double> wiiAccelOffsetZ;
    // Debugging aid: append every KPAD sample of the Bluetooth remote (raw and
    // corrected accelerometer, buttons) to wii_accel_trace.csv next to Config.toml.
    std::optional<bool> wiiAccelTrace;
    std::optional<bool> networkEnabled;
    std::optional<bool> discordPresenceEnabled;
    // The application ID of the WiiCompiled Discord application. This is only
    // used by the base product; a mod supplies its own ID through the
    // standard Dolphin /dev/dolphin interface.
    std::optional<std::string> discordClientId;
    std::optional<std::string> nandRoot;
    std::optional<std::string> dvdRoot;
    // [paths] saves_dir: where the saves go instead of the default (SavesDirectory).
    std::optional<std::string> savesDir;
    // The one canonical mod installation, owned and updated by the frontend. Setup records
    // it here instead of copying the pack, so an asset-only update is visible on the next launch.
    std::optional<std::string> modInstallRoot;
    std::vector<std::string> overlayRoots;
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    std::optional<bool> rumbleEnabled;
    // [controller] layout: "gamecube" (default) or "xbox_original" (hle/no100f_xbox_layout.h).
    std::optional<std::string> controllerLayout;
    // [controller] xbox_black / xbox_white: the pad button for Xbox Black (sneak) / White (soap bubble) with the
    // original Xbox layout, as a controller_button_names.h name ("right_shoulder", "north", "disabled", ...).
    std::array<std::optional<std::string>, 2> xboxButtons;
    std::optional<int32_t> muteHotkey;
    std::map<std::string, std::string> controllerExpressions;
    // [keyboard] action = "comma-separated SDL scancode names" (keybinds.h).
    std::map<std::string, std::string> keyboardBindings;
    std::map<std::string, std::string> gamepadBindings;  // [gamepad]: type + per-action overrides
};

namespace RuntimeConfigFile {

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline constexpr const char* kConfigFileName = "Config.toml";
inline constexpr const char* kApplicationDirectoryName = "WiiCompiled";

// Portable layout. A directory holding kPortableMarkerFileName is a portable root; every piece of
// runtime user state (Config.toml, NAND, Cache, Logs) lives in <root>/UserData instead of
// %LOCALAPPDATA%. The marker is searched for from the executable's directory upwards, which is what
// makes an installation survive being moved or carried on removable media.
inline constexpr const char* kPortableMarkerFileName = "portable.txt";
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

// The installed layout puts products two levels below the root (<root>/Install/Base/game.exe). The
// bound is deliberately small so an unrelated marker far up a drive can never capture an ordinary
// installation.
inline constexpr int kPortableSearchDepth = 4;

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline std::string RemoveComment(std::string_view line) {
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inDouble && ch == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle && !escaped) {
            inDouble = !inDouble;
        } else if (ch == '#' && !inSingle && !inDouble) {
            return std::string(line.substr(0, i));
        }
        escaped = false;
    }
    return std::string(line);
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.25f, 3.0f, 4.0f, 4.5f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

// NO100F [game] holiday: "auto", "off", "confetti" (multicoloured fireworks), or a forced date "MM-DD".
inline bool IsSupportedHoliday(std::string_view value) {
    if (value == "auto" || value == "off" || value == "confetti") return true;
    if (value.size() != 5 || value[2] != '-') return false;
    for (const size_t i : {size_t{0}, size_t{1}, size_t{3}, size_t{4}})
        if (value[i] < '0' || value[i] > '9') return false;
    const int month = (value[0] - '0') * 10 + (value[1] - '0');
    const int day = (value[3] - '0') * 10 + (value[4] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// 240 was offered by an early build and is no longer supported; a saved 240 is
// migrated to 180 at the parse site.
inline bool IsSupportedFrameInterpolationFps(uint32_t value) {
    return value == 0 || value == 120 || value == 180;
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__) || defined(__ANDROID__)
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// The portable root this executable lives under, or nullopt for a normal installation. The answer
// cannot change while the process runs, so it is resolved exactly once: every user-state path
// derives from it and they must not disagree with each other.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
    static const std::optional<std::filesystem::path> root = []() -> std::optional<std::filesystem::path> {
        const auto executableDirectory = ExecutableDirectory();
        if (!executableDirectory) {
            return std::nullopt;
        }
        std::filesystem::path current = *executableDirectory;
        for (int level = 0; level <= kPortableSearchDepth; ++level) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(current / kPortableMarkerFileName, ec)) {
                return current;
            }
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }();
    return root;
}

#ifdef _WIN32
// FrightWorks without the portable marker is the installed game (Velopack, in %LOCALAPPDATA%\FrightWorks, which an
// update replaces and an uninstall deletes, so nothing of the player's may live there): settings, logs, caches, the
// extracted disc and mods go to %LOCALAPPDATA%\FrightWorks-Data, saves to Saved Games\FrightWorks (SavesDirectory),
// F2 / F3 to Pictures\FrightWorks and Videos\FrightWorks (CaptureDirectory). launcher/DataPaths.cs uses the same
// folders.
inline constexpr const wchar_t* kInstalledDataDirectoryName = L"FrightWorks-Data";
inline constexpr const wchar_t* kInstalledUserFolderName = L"FrightWorks";

inline std::optional<std::filesystem::path> KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::optional<std::filesystem::path> path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        path = std::filesystem::path(raw);
    }
    CoTaskMemFree(raw);
    return path;
}
#endif

inline std::filesystem::path ApplicationDataDirectory() {
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
#ifdef _WIN32
    static const std::optional<std::filesystem::path> localAppData = KnownFolder(FOLDERID_LocalAppData);
    if (localAppData) {
        return *localAppData / kInstalledDataDirectoryName;
    }
#elif defined(__APPLE__) || defined(__ANDROID__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    // XDG Base Directory spec equivalent of FOLDERID_LocalAppData: $XDG_DATA_HOME if set and
    // non-empty, otherwise its default of $HOME/.local/share.
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome) {
        return std::filesystem::path(xdgDataHome) / kApplicationDirectoryName;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / kApplicationDirectoryName;
    }
#endif
    return std::filesystem::current_path() / kApplicationDirectoryName;
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream output(path);
    if (!output) {
        return;
    }
    // Scooby-Doo! Night of 100 Frights defaults: borderless 1080p (2.25 x 480 lines; 4.5 = 4K, 0 = the screen's
    // own size), cinematic 2.39:1, no frame interpolation (the game runs at 30 fps, where interpolation stays off:
    // FrameInterpolationFps), HD textures on. The launcher writes the same keys.
    output << "# FrightWorks: Night of 100 Frights user configuration\n"
              "# paths.dvd_root is your Scooby-Doo! Night of 100 Frights (USA, GIHE78) extracted to a DATA folder;\n"
              "# the launcher fills it in from a .iso/.gcm/.ciso.\n\n"
              "[video]\n"
              "widescreen = true\n"
              "force_16_9 = false\n"
              "# render height = 480 x resolution_multiplier: 1.5 = 720p, 2.25 = 1080p, 4.5 = 4K, 0 = screen size\n"
              "resolution_multiplier = 2.25\n"
              "# 0 (off), 120 or 180; only with fps60 = true, it stays off at 30 fps\n"
              "frame_interpolation_fps = 0\n"
              "# true = 60 fps (the game times itself by the clock, so it keeps its speed)\n"
              "fps60 = false\n"
              "# motion blur strength in percent: 0 = off, 10 = extra low, 20 = low, 30 = medium, 40 = high, 50 = extra high\n"
              "motion_blur = 0\n"
              "# ambient occlusion (shade in creases, corners and under things) in percent: 0 = off, 35 = low, 65 = medium,\n"
              "# 100 = high\n"
              "ambient_occlusion = 0\n"
              "# cartoon ink outlines: 0 = off, 1 = thin, 2 = bold\n"
              "ink_outlines = 0\n"
              "# 0 = original 4:3, 1 = 16:9, 2 = fill the screen, 3 = cinematic 2.39:1\n"
              "aspect_mode = 3\n"
              "display_mode = \"borderless\"\n"
              "graphics_api = \"auto\"\n"
              "skip_unready_pipelines = true\n"
              "disable_copy_filter = true\n"
              "show_fps = false\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to Cache/texture_dumps under the name a\n"
              "# replacement would need. Both are read once, at startup.\n"
              "texture_replacements = true\n"
              "texture_dumps = false\n\n"
              "[game]\n"
              "# Holiday events in the hubs (fireworks, Halloween bats, ...): \"auto\" = by today's date,\n"
              "# \"off\" = never, \"confetti\" = fireworks with the colours mixed through each burst,\n"
              "# or \"MM-DD\" to pretend it is that day (e.g. \"10-31\").\n"
              "holiday = \"auto\"\n"
              "# Developer tools: the F8 dev menu (travel, spots, free camera, texture finder) and a DEV build mark.\n"
              "dev_tools = false\n"
              "# With the developer tools on, F3 records a video of the game (needs ffmpeg); this adds the game's sound.\n"
              "record_audio = true\n"
              "# With the developer tools on: F2 screenshots and F3 videos show the F8/F10 menus too (else only the DEV stamp).\n"
              "capture_overlay = false\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "music_volume = 1.0\n"
              "sound_effects_volume = 1.0\n"
              "ui_volume = 1.0\n"
              "voices_volume = 1.0\n"
              "muted = false\n"
              "attenuate_music_when_media_plays = false\n"
              "# Runs the AX/DSP voice mix on its own thread, joined before the\n"
              "# guest can observe it. Set to false to mix inline on the guest\n"
              "# thread exactly as the runtime did before.\n"
              "mix_worker = true\n\n"
              "[network]\n"
              "enabled = true\n\n"
              "[discord]\n"
              "# Rich Presence talks only to a locally-running Discord client.\n"
              "# Off until client_id names a Discord application: FrightWorks has none\n"
              "# of its own yet.\n"
              "enabled = true\n"
              "# client_id = \"123456789012345678\"\n\n"
              "[paths]\n"
              "# dvd_root = \"D:\\\\Games\\\\ScoobyDoo\\\\DATA\"\n"
              "# nand_root = \"D:\\\\WiiNand\"\n"
              "# retro_rewind_root = \"D:\\\\RetroRewind\\\\RetroRewind6\"\n"
              "# overlay_roots = [\"D:\\\\RetroRewind\"]\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

inline void AppendOverlayRoots(RuntimeUserConfig& config, const std::string& roots) {
    size_t begin = 0;
    while (begin < roots.size()) {
        const size_t end = roots.find(';', begin);
        std::string root = Trim(std::string_view(roots).substr(begin, end - begin));
        if (!root.empty()) {
            config.overlayRoots.push_back(std::move(root));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// Reads every supported setting out of a parsed Config.toml document.
inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    config.controllerLayout = FindConfigValue<std::string>(document, "controller", "layout");
    config.xboxButtons[0] = FindConfigValue<std::string>(document, "controller", "xbox_black");
    config.xboxButtons[1] = FindConfigValue<std::string>(document, "controller", "xbox_white");
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
            }
        }
    }

    if (const auto* section = document.contains("keyboard") ? &document.at("keyboard") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (value.is_string()) {
                config.keyboardBindings[key] = value.as_string();
            }
        }
    }
    if (const auto* section = document.contains("gamepad") ? &document.at("gamepad") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (value.is_string()) {
                config.gamepadBindings[key] = value.as_string();
            }
        }
    }

    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    config.forceAspect169 = FindConfigValue<bool>(document, "video", "force_16_9");
    if (auto value = FindConfigInt(document, "video", "aspect_mode"); value && *value >= 0 && *value <= 3) {
        config.aspectMode = *value;
    }
    if (auto value = FindConfigInt(document, "video", "motion_blur"); value && *value >= 0 && *value <= 100) {
        config.motionBlur = *value;
    }
    if (auto value = FindConfigInt(document, "video", "ambient_occlusion"); value && *value >= 0 && *value <= 100) {
        config.ambientOcclusion = *value;
    }
    if (auto value = FindConfigInt(document, "video", "ink_outlines"); value && *value >= 0 && *value <= 2) {
        config.inkOutlines = *value;
    }
    if (auto value = FindConfigInt(document, "video", "gpu_compat"); value && *value >= 0 && *value <= 2) {
        config.gpuCompat = *value;
    }
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value != 0) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value != 0) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    config.fullscreenDisplay = FindConfigValue<std::string>(document, "video", "fullscreen_display");
    if (auto value = FindConfigUint(document, "video", "frame_interpolation_fps")) {
        const uint32_t migrated = *value == 240u ? 180u : *value;
        if (IsSupportedFrameInterpolationFps(migrated)) {
            config.frameInterpolationFps = migrated;
        }
    }
    config.metalFxSpatialUpscaling =
        FindConfigValue<bool>(document, "video", "metalfx_spatial_upscaling");
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.skipIntro = FindConfigValue<bool>(document, "video", "skip_intro");
    config.fps60 = FindConfigValue<bool>(document, "video", "fps60");
    if (auto value = FindConfigValue<bool>(document, "game", "dev_tools")) {
        config.devTools = *value;
    }
    config.recordAudio = FindConfigValue<bool>(document, "game", "record_audio");
    config.captureOverlay = FindConfigValue<bool>(document, "game", "capture_overlay");
    if (auto value = FindConfigValue<std::string>(document, "game", "holiday"); value && IsSupportedHoliday(*value)) {
        config.holiday = *value;
    }
    if (auto value = FindConfigUint(document, "video", "msaa"); value && (*value == 1 || *value == 2 || *value == 4 || *value == 8)) {
        config.msaa = *value;
    }
    config.spinningCoins = FindConfigValue<bool>(document, "video", "spinning_coins");
    config.frameGraph = FindConfigValue<bool>(document, "video", "frame_graph");
    config.vsync = FindConfigValue<bool>(document, "video", "vsync");
    config.hdr = FindConfigValue<bool>(document, "video", "hdr");
    config.hdrPaperWhiteNits = FindConfigFloat(document, "video", "hdr_paper_white_nits");
    config.hdrPeakNits = FindConfigFloat(document, "video", "hdr_peak_nits");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    if (auto value = FindConfigValue<std::string>(document, "video", "cutout_mode")) {
        static constexpr std::string_view kModes[] = {"game", "cut", "twopass", "twopass_all"};
        for (int i = 0; i < 4; ++i)
            if (*value == kModes[i]) config.cutoutMode = i;
    }
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");
    if (auto value = FindConfigUint(document, "video", "disabled_post_processing_paths");
        value && (*value & ~0x10u) == 0) {
        config.disabledPostProcessingPaths = *value & 0x10u;
    }

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    config.audioMusicVolume = readVolume("music_volume");
    config.audioSoundEffectsVolume = readVolume("sound_effects_volume");
    config.audioUiVolume = readVolume("ui_volume");
    config.audioVoicesVolume = readVolume("voices_volume");
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.attenuateMusicWhenMediaPlays =
        FindConfigValue<bool>(document, "audio", "attenuate_music_when_media_plays");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiContinuousScan = FindConfigValue<bool>(document, "controller", "wii_continuous_scan");
    config.wiiAccelOffsetX = FindConfigValue<double>(document, "controller", "wii_accel_offset_x");
    config.wiiAccelOffsetY = FindConfigValue<double>(document, "controller", "wii_accel_offset_y");
    config.wiiAccelOffsetZ = FindConfigValue<double>(document, "controller", "wii_accel_offset_z");
    config.wiiAccelTrace = FindConfigValue<bool>(document, "controller", "wii_accel_trace");
    config.networkEnabled = FindConfigValue<bool>(document, "network", "enabled");
    config.discordPresenceEnabled = FindConfigValue<bool>(document, "discord", "enabled");
    config.discordClientId = FindConfigValue<std::string>(document, "discord", "client_id");

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    config.savesDir = FindConfigValue<std::string>(document, "paths", "saves_dir");
    config.modInstallRoot = FindConfigValue<std::string>(document, "paths", "retro_rewind_root");
    if (auto roots = FindConfigValue<std::vector<std::string>>(document, "paths", "overlay_roots")) {
        for (auto& root : *roots) {
            root = Trim(root);
            if (!root.empty()) {
                config.overlayRoots.push_back(std::move(root));
            }
        }
    } else if (auto roots = FindConfigValue<std::string>(document, "paths", "overlay_roots")) {
        AppendOverlayRoots(config, *roots);
    }

    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    // A UTF-8 byte order mark (some editors add one) is not TOML: skip it rather than lose every setting.
    if (input.peek() == 0xEF) {
        char bom[3] = {};
        input.read(bom, 3);
        if (static_cast<unsigned char>(bom[1]) != 0xBB || static_cast<unsigned char>(bom[2]) != 0xBF) {
            input.clear();
            input.seekg(0);
        }
    }
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    std::ifstream file(ResolveConfigPath(), std::ios::binary);
    return file ? ParseConfig(file, PathToUtf8(ResolveConfigPath())) : RuntimeUserConfig{};
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Update one TOML value without discarding comments, unrelated settings, or
// user-specific paths. This is used by the in-game F10 settings bar.
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    const std::string normalizedSection = Trim(section);
    const std::string normalizedKey = Trim(key);
    size_t sectionStart = lines.size();
    size_t sectionEnd = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(RemoveComment(lines[i]));
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string found = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            if (sectionStart != lines.size()) {
                sectionEnd = i;
                break;
            }
            if (found == normalizedSection) {
                sectionStart = i;
            }
        }
    }

    const std::string replacement = normalizedKey + " = " + std::string(value);
    if (sectionStart == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[" + normalizedSection + "]");
        lines.push_back(replacement);
    } else {
        bool replaced = false;
        for (size_t i = sectionStart + 1; i < sectionEnd; ++i) {
            const std::string uncommented = Trim(RemoveComment(lines[i]));
            const size_t equals = uncommented.find('=');
            if (equals != std::string::npos && Trim(std::string_view(uncommented).substr(0, equals)) == normalizedKey) {
                lines[i] = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // Append after the section's last real line rather than after the blank line that
            // separates it from the next header: this file is edited by hand and by the host
            // installer as well, and a key parked below the separator reads as if it belonged to
            // the next section. The host writer (Launcher/WiiCompiled.Setup/RuntimeConfiguration.cs)
            // applies exactly this rule.
            size_t insertAt = sectionEnd;
            while (insertAt > sectionStart + 1 && Trim(lines[insertAt - 1]).empty()) {
                --insertAt;
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), replacement);
        }
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    for (const auto& outputLine : lines) {
        output << outputLine << '\n';
    }
    return static_cast<bool>(output);
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameInterpolationFps(uint32_t value) {
    if (!IsSupportedFrameInterpolationFps(value)) {
        return false;
    }
    Mutable().frameInterpolationFps = value;
    return WriteSetting("video", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetMetalFxSpatialUpscaling(bool value) {
    Mutable().metalFxSpatialUpscaling = value;
    return WriteSetting("video", "metalfx_spatial_upscaling", value ? "true" : "false");
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

inline bool SetFullscreenDisplay(std::string value) {
    Mutable().fullscreenDisplay = value;
    return WriteSetting("video", "fullscreen_display", FormatString(value));
}

inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

inline bool SpinningCoins(bool fallback = true) {
    return Get().spinningCoins.value_or(fallback);
}

inline bool SetSpinningCoins(bool value) {
    Mutable().spinningCoins = value;
    return WriteSetting("video", "spinning_coins", value ? "true" : "false");
}

inline bool FrameGraph(bool fallback = false) {
    return Get().frameGraph.value_or(fallback);
}

inline bool SetFrameGraph(bool value) {
    Mutable().frameGraph = value;
    return WriteSetting("video", "frame_graph", value ? "true" : "false");
}

inline bool Hdr(const RuntimeUserConfig& config = Get()) { return config.hdr.value_or(false); }
inline float HdrPaperWhiteNits(const RuntimeUserConfig& config = Get()) { return std::clamp(config.hdrPaperWhiteNits.value_or(200.f), 80.f, 1000.f); }
inline float HdrPeakNits(const RuntimeUserConfig& config = Get()) { return std::clamp(config.hdrPeakNits.value_or(1000.f), 80.f, 10000.f); }

inline bool SetHdr(bool value) {
    if (!WriteSetting("video", "hdr", value ? "true" : "false")) return false;
    Mutable().hdr = value;
    return true;
}

inline bool SetHdrPaperWhiteNits(float value) {
    if (!std::isfinite(value) || value < 80.f || value > 1000.f) {
        std::cerr << "[runtime-config] Invalid HDR paper white: " << value << std::endl;
        return false;
    }
    if (!WriteSetting("video", "hdr_paper_white_nits", std::to_string(value))) return false;
    Mutable().hdrPaperWhiteNits = value;
    return true;
}

inline bool SetHdrPeakNits(float value) {
    if (!std::isfinite(value) || value < 80.f || value > 10000.f) {
        std::cerr << "[runtime-config] Invalid HDR peak: " << value << std::endl;
        return false;
    }
    if (!WriteSetting("video", "hdr_peak_nits", std::to_string(value))) return false;
    Mutable().hdrPeakNits = value;
    return true;
}

inline bool VSync(bool fallback = false) {
    return Get().vsync.value_or(fallback);
}

inline bool SetVSync(bool value) {
    Mutable().vsync = value;
    return WriteSetting("video", "vsync", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

inline bool SetDisabledPostProcessingPaths(uint32_t value) {
    Mutable().disabledPostProcessingPaths = value;
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::uppercase << value;
    return WriteSetting("video", "disabled_post_processing_paths", formatted.str());
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

// Config.toml [gamepad] (written by the launcher's Controls page); "" when the key is absent.
inline std::string GamepadBinding(const std::string& key) {
    const auto it = Get().gamepadBindings.find(key);
    return it == Get().gamepadBindings.end() ? std::string() : it->second;
}

inline std::string KeyboardBinding(const std::string& key) {
    const auto it = Get().keyboardBindings.find(key);
    return it == Get().keyboardBindings.end() ? std::string() : it->second;
}

inline bool SetKeyboardBinding(const std::string& key, const std::string& value) {
    Mutable().keyboardBindings[key] = value;
    return WriteSetting("keyboard", key, FormatString(value));
}

inline bool RumbleEnabled(bool fallback = true) {
    return Get().rumbleEnabled.value_or(fallback);
}

inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

inline bool XboxOriginalLayout() {
    const auto& layout = Get().controllerLayout;
    return layout && *layout == "xbox_original";
}

inline bool SetXboxOriginalLayout(bool value) {
    Mutable().controllerLayout = value ? "xbox_original" : "gamecube";
    // An explicit F10 choice, including the already-selected preset, ends launcher auto-selection.
    const bool wroteIntent = WriteSetting("launcher", "auto_controller_layout", "false");
    return WriteSetting("controller", "layout", FormatString(*Mutable().controllerLayout)) && wroteIntent;
}

// 0 = Black (sneak, default right bumper), 1 = White (soap bubble, default left bumper).
inline std::string XboxButton(size_t index) {
    const auto& value = Get().xboxButtons[index & 1];
    return value ? *value : (index == 0 ? "right_shoulder" : "left_shoulder");
}

inline bool SetXboxButton(size_t index, const std::string& value) {
    Mutable().xboxButtons[index & 1] = value;
    return WriteSetting("controller", (index & 1) == 0 ? "xbox_black" : "xbox_white", FormatString(value));
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

inline bool SetMusicVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioMusicVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "music_volume", formatted.str());
}

inline bool SetSoundEffectsVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioSoundEffectsVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "sound_effects_volume", formatted.str());
}

inline bool SetUiVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioUiVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "ui_volume", formatted.str());
}

inline bool SetVoicesVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVoicesVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "voices_volume", formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool SetAttenuateMusicWhenMediaPlays(bool value) {
    Mutable().attenuateMusicWhenMediaPlays = value;
    return WriteSetting("audio", "attenuate_music_when_media_plays", value ? "true" : "false");
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool ForceAspect169Enabled(bool fallback = false) {
    return Get().forceAspect169.value_or(fallback);
}

// Display aspect (DisplaySettings): 0 original 4:3, 1 16:9, 2 fill the window or screen, 3 cinematic 2.39:1.
inline int32_t AspectMode(int32_t fallback) {
    return Get().aspectMode.value_or(fallback);
}

inline int32_t GpuCompat() {
    return Get().gpuCompat.value_or(0);
}

inline bool SetGpuCompat(int32_t value) {
    Mutable().gpuCompat = value;
    return WriteSetting("video", "gpu_compat", std::to_string(value));
}

inline bool SetAspectMode(int32_t value) {
    Mutable().aspectMode = value;
    return WriteSetting("video", "aspect_mode", std::to_string(value));
}

// Motion blur strength in percent, 0 = off. The menus offer the levels below; any other value from 0 to 100 works too.
inline int32_t MotionBlur(int32_t fallback = 0) {
    return Get().motionBlur.value_or(fallback);
}

struct MotionBlurLevel {
    const char* name;
    int32_t percent;
};
inline constexpr MotionBlurLevel kMotionBlurLevels[] = {
    {"Off", 0}, {"Extra low", 10}, {"Low", 20}, {"Medium", 30}, {"High", 40}, {"Extra high", 50},
};

// How many frames of motion a strength smears each pixel over (aurora_set_motion_blur): 100% = four, 25% = one.
// Stylised above about 12%: a real camera's shutter (half a frame or less) is hard to see at 60 fps.
inline float MotionBlurShutter(int32_t percent) {
    return static_cast<float>(std::clamp(percent, 0, 100)) / 25.0f;
}

inline bool SetMotionBlur(int32_t value) {
    Mutable().motionBlur = value;
    return WriteSetting("video", "motion_blur", std::to_string(value));
}

// Ambient occlusion strength in percent, 0 = off (aurora_set_ambient_occlusion takes it as 0 to 1). The menus offer the
// levels below; any other value from 0 to 100 works too.
inline int32_t AmbientOcclusion(int32_t fallback = 0) {
    return Get().ambientOcclusion.value_or(fallback);
}

inline constexpr MotionBlurLevel kAmbientOcclusionLevels[] = {
    {"Off", 0}, {"Low", 35}, {"Medium", 65},
};

inline bool SetAmbientOcclusion(int32_t value) {
    Mutable().ambientOcclusion = value;
    return WriteSetting("video", "ambient_occlusion", std::to_string(value));
}

// Cartoon ink outlines: 0 off, 1 thin, 2 bold (aurora_set_ink_outlines).
inline int32_t InkOutlines(int32_t fallback = 0) {
    return Get().inkOutlines.value_or(fallback);
}

inline constexpr const char* kInkOutlineLevels[] = {"Off", "Thin", "Bold"};

inline bool SetInkOutlines(int32_t value) {
    Mutable().inkOutlines = value;
    return WriteSetting("video", "ink_outlines", std::to_string(value));
}

inline bool SetForceAspect169(bool value) {
    Mutable().forceAspect169 = value;
    return WriteSetting("video", "force_16_9", value ? "true" : "false");
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 1.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline bool MetalFxSpatialUpscaling(bool fallback = false) {
    return Get().metalFxSpatialUpscaling.value_or(fallback);
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float MusicVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioMusicVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float SoundEffectsVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioSoundEffectsVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float UiVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioUiVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float VoicesVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVoicesVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default on; false restores the fully synchronous mix.
inline bool AudioMixWorkerEnabled(bool fallback = true) {
    return Get().audioMixWorker.value_or(fallback);
}

// Whether background music should duck automatically for other media playback.
inline bool AttenuateMusicWhenMediaPlays(bool fallback = false) {
    return Get().attenuateMusicWhenMediaPlays.value_or(fallback);
}

// Bluetooth Wii Remotes / Wii U Pro Controllers. Read once before SDL's joystick
// subsystem comes up, so a change only takes effect on the next launch.
inline bool WiiRemotesEnabled(bool fallback = true) {
    return Get().wiiRemotes.value_or(fallback);
}

// Persists the Bluetooth Wii Remote driver switch.
inline bool SetWiiRemotesEnabled(bool value) {
    Mutable().wiiRemotes = value;
    return WriteSetting("controller", "wii_remotes", value ? "true" : "false");
}

// Whether to keep rescanning Bluetooth while no Wii controller is connected.
inline bool WiiContinuousScanEnabled(bool fallback = false) {
    return Get().wiiContinuousScan.value_or(fallback);
}

// Persists the continuous scanning switch.
inline bool SetWiiContinuousScanEnabled(bool value) {
    Mutable().wiiContinuousScan = value;
    return WriteSetting("controller", "wii_continuous_scan", value ? "true" : "false");
}

// Wii Remote accelerometer zero-point correction (g, SDL sensor frame); all zero
// when the remote has not been calibrated.
inline std::array<double, 3> WiiAccelOffset() {
    const RuntimeUserConfig& config = Get();
    return {config.wiiAccelOffsetX.value_or(0.0), config.wiiAccelOffsetY.value_or(0.0),
            config.wiiAccelOffsetZ.value_or(0.0)};
}

// Whether to write the per-frame accelerometer trace (off unless asked for).
inline bool WiiAccelTraceEnabled(bool fallback = false) {
    return Get().wiiAccelTrace.value_or(fallback);
}

// True while a non-zero correction is stored ("Clear calibration" writes zeros).
inline bool HasWiiAccelOffset() {
    const std::array<double, 3> offset = WiiAccelOffset();
    return offset[0] != 0.0 || offset[1] != 0.0 || offset[2] != 0.0;
}

// Persists the accelerometer correction measured by the overlay's calibration.
inline bool SetWiiAccelOffset(const std::array<double, 3>& offset) {
    Mutable().wiiAccelOffsetX = offset[0];
    Mutable().wiiAccelOffsetY = offset[1];
    Mutable().wiiAccelOffsetZ = offset[2];
    bool ok = true;
    const char* keys[3] = {"wii_accel_offset_x", "wii_accel_offset_y", "wii_accel_offset_z"};
    for (size_t i = 0; i < 3; ++i) {
        // Always a float literal, so a whole-number offset does not come back as a TOML integer.
        std::ostringstream formatted;
        formatted << std::fixed << std::setprecision(4) << offset[i];
        ok = WriteSetting("controller", keys[i], formatted.str()) && ok;
    }
    return ok;
}

// Target frame rate for frame interpolation, or 0 to disable it. Always 0 at 30 fps ([video] fps60 off), whatever the
// file says: vi.cpp would report every frame as late there, so it would do nothing. The saved value isn't touched.
inline uint32_t FrameInterpolationFpsIn(const RuntimeUserConfig& config, uint32_t fallback = 0) {
    return config.fps60.value_or(false) ? config.frameInterpolationFps.value_or(fallback) : 0;
}

inline uint32_t FrameInterpolationFps(uint32_t fallback = 0) {
    return FrameInterpolationFpsIn(Get(), fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

// Multisample anti-aliasing samples (1 = off, 2, 4 or 8); applies at the next start.
inline uint32_t Msaa(uint32_t fallback = 1) {
    return Get().msaa.value_or(fallback);
}

// NO100F: end the boot logo movies and the intro movie after their first frame (launcher "Skip intro movies").
inline bool Fps60(bool fallback = false) {
    return Get().fps60.value_or(fallback);
}

inline bool SkipIntro(bool fallback = false) {
    return Get().skipIntro.value_or(fallback);
}

// NO100F: the date the hubs' holiday events see (launcher "Holidays"): "auto" = today, "off" = none, "confetti" = Jan 1
// with confetti firework bursts, "MM-DD" = that day.
inline std::string Holiday() {
    return Get().holiday.value_or("auto");
}

// NO100F [game] dev_tools: the F8 dev menu and the DEV build mark. Read at startup: the dev menu reserves a page of
// guest memory before boot only when it is on. NO100F_DEV_TOOLS=1 (or 0) overrides the file: the debug kit's scripts
// (tools/debug) set it so their sessions have the dev menu without touching the player's settings.
inline bool DevTools() {
    if (const char* env = std::getenv("NO100F_DEV_TOOLS"); env != nullptr && *env != '\0') {
        return *env != '0';
    }
    return Get().devTools.value_or(false);
}

// NO100F [game] record_audio: F3 videos (developer tools) include the game's own sound (default on). Read when a
// recording starts; the dev menu's toggle writes it.
inline bool RecordAudio() {
    return Get().recordAudio.value_or(true);
}

inline bool SetRecordAudio(bool value) {
    Mutable().recordAudio = value;
    return WriteSetting("game", "record_audio", value ? "true" : "false");
}

// NO100F [game] capture_overlay: with the developer tools on, F2 screenshots and F3 videos show the F8/F10 menus and
// notices too; off (default) = only the DEV stamp. The dev menu's toggle writes it.
inline bool CaptureOverlay() {
    return Get().captureOverlay.value_or(false);
}

inline bool SetCaptureOverlay(bool value) {
    Mutable().captureOverlay = value;
    return WriteSetting("game", "capture_overlay", value ? "true" : "false");
}

inline bool ShowFps(bool fallback = true) {
    return Get().showFps.value_or(fallback);
}

inline bool TextureReplacements(bool fallback = true) {
    return Get().textureReplacements.value_or(fallback);
}

// [video] cutout_mode: how cut-out draws (leaves, grass, webs) are drawn, as aurora_set_cutout_mode takes it:
// "game" 0 = the game's own alpha test, "cut" 1 = alpha > 127 for HD replacements, "twopass" 2 = solid and faint parts
// in two passes for HD replacements, "twopass_all" 3 = two passes for original textures too.
inline int CutoutMode(int fallback = 3) {
    return Get().cutoutMode.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

inline uint32_t DisabledPostProcessingPaths(uint32_t fallback = 0) {
    return Get().disabledPostProcessingPaths.value_or(fallback) & 0x10u;
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

inline std::string DisplayMode(std::string fallback = "borderless") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline std::string FullscreenDisplay(std::string fallback = "auto") {
    return Get().fullscreenDisplay.value_or(std::move(fallback));
}

inline bool NetworkEnabled(bool fallback = true) {
    return Get().networkEnabled.value_or(fallback);
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory.
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

// The saves (MemoryCard, and NAND for Wii games): [paths] saves_dir when set (absolute, or relative to Config.toml's
// folder; the launcher's Extras > Folders sets it), else Saved Games\FrightWorks when installed, else UserData\Saves.
inline std::filesystem::path SavesDirectory() {
    const std::string configured = Get().savesDir.value_or("");
    if (!configured.empty()) {
        return ResolveRelativeToConfig(configured);
    }
#ifdef _WIN32
    if (!PortableRootDirectory()) {
        if (const auto savedGames = KnownFolder(FOLDERID_SavedGames)) {
            return *savedGames / kInstalledUserFolderName;
        }
    }
#endif
    return ApplicationDataDirectory() / "Saves";
}

// F2 screenshots and F3 videos. Installed: Pictures\FrightWorks and Videos\FrightWorks. Portable: Screenshots\ and
// Videos\ beside the launcher (the folder above data\ in a release, the game's own folder in a source build).
inline std::filesystem::path CaptureDirectory(bool videos) {
#ifdef _WIN32
    if (!PortableRootDirectory()) {
        if (const auto folder = KnownFolder(videos ? FOLDERID_Videos : FOLDERID_Pictures)) {
            return *folder / kInstalledUserFolderName;
        }
    }
#endif
    const auto executableDirectory = ExecutableDirectory();
    std::filesystem::path root = executableDirectory ? *executableDirectory : std::filesystem::current_path();
    if (root.filename() == "data") {
        root = root.parent_path();
    }
    return root / (videos ? "Videos" : "Screenshots");
}

// CaptureDirectory as the game's notices name it, ending in a backslash (Videos\, Pictures\FrightWorks\, ...).
inline std::string CaptureDirectoryLabel(bool videos) {
    if (PortableRootDirectory()) {
        return videos ? "Videos\\" : "Screenshots\\";
    }
    return videos ? "Videos\\FrightWorks\\" : "Pictures\\FrightWorks\\";
}

// A game folder for this run only, not saved (a "game" folder beside the program, so each
// install plays its own game with its own saves).
inline void UseDvdRootForThisRun(const std::filesystem::path& path) {
    Mutable().dvdRoot = PathToUtf8(path);
}

// The game folder the player picked at first start (GameFolder::EnsureConfigured).
inline bool SetDvdRoot(const std::filesystem::path& path) {
    const std::string value = PathToUtf8(path);
    Mutable().dvdRoot = value;
    return WriteSetting("paths", "dvd_root", FormatString(value));
}

/// The canonical mod installation the frontend owns, or "" when none is recorded.
inline std::string ModInstallRoot(std::string fallback = "") {
    return Get().modInstallRoot.value_or(std::move(fallback));
}

inline bool DiscordPresenceEnabled(bool fallback = true) {
    return Get().discordPresenceEnabled.value_or(fallback);
}

// No default application: FrightWorks has no Discord app of its own yet, so base-game presence only starts when
// [discord] client_id is set (WiiCompiled's app ID was 1543984562369990706).
inline std::string DiscordClientId(std::string fallback = "") {
    return Get().discordClientId.value_or(std::move(fallback));
}

inline const std::vector<std::string>& OverlayRoots() {
    return Get().overlayRoots;
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.frameInterpolationFps) {
                std::cout << " frame_interpolation_fps=" << *config.frameInterpolationFps;
            }
            if (config.motionBlur) {
                std::cout << " motion_blur=" << *config.motionBlur;
            }
            if (config.ambientOcclusion) {
                std::cout << " ambient_occlusion=" << *config.ambientOcclusion;
            }
            if (config.inkOutlines) {
                std::cout << " ink_outlines=" << *config.inkOutlines;
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.networkEnabled) {
                std::cout << " network_enabled=" << (*config.networkEnabled ? "true" : "false");
            }
            if (config.discordPresenceEnabled) {
                std::cout << " discord_enabled=" << (*config.discordPresenceEnabled ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
            if (config.modInstallRoot) {
                std::cout << " retro_rewind_root=" << *config.modInstallRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
