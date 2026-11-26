#include "system_report.h"

#include "runtime_log.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <SDL3/SDL_video.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace SystemReport {
namespace {

bool HasEnv(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

// Adds "name=value" for a set environment variable whose value is a fixed word (never a path or a user name).
void AddEnvWord(std::ostringstream& out, const char* name) {
    if (!HasEnv(name)) return;
    std::string value = Env(name);
    if (value.size() > 40) value.resize(40);
    out << ' ' << name << '=' << value;
}

// Adds "name" for a set variable whose value may hold a path: only that it is set.
void AddEnvFlag(std::ostringstream& out, const char* name) {
    if (HasEnv(name)) out << ' ' << name;
}

#if !defined(_WIN32)
// PRETTY_NAME from an os-release file, "" when there is none.
std::string OsReleaseName(const char* path) {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string value = line.substr(12);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.substr(1, value.size() - 2);
        return value;
    }
    return {};
}
#endif

}  // namespace

void LogHostSystem() {
    std::ostringstream os;
    std::ostringstream launch;
#if defined(_WIN32)
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (const auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        rtlGetVersion != nullptr && rtlGetVersion(&version) == 0) {
        os << "Windows " << version.dwMajorVersion << '.' << version.dwMinorVersion << " build " << version.dwBuildNumber;
    } else {
        os << "Windows (version unknown)";
    }
    // Wine and Proton (a Linux player running the Windows build) answer to these ntdll exports.
    using WineVersionFn = const char*(CDECL*)();
    using WineHostFn = void(CDECL*)(const char**, const char**);
    if (const auto wineVersion = ntdll ? reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr) {
        os << ", Wine " << wineVersion();
        if (const auto wineHost = reinterpret_cast<WineHostFn>(GetProcAddress(ntdll, "wine_get_host_version"))) {
            const char* sysname = nullptr;
            const char* release = nullptr;
            wineHost(&sysname, &release);
            if (sysname) os << " on " << sysname << ' ' << (release ? release : "");
        }
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) os << ", " << (memory.ullTotalPhys >> 20) << " MiB RAM";
    AddEnvFlag(launch, "STEAM_COMPAT_DATA_PATH");  // Proton
#else
    utsname name{};
    if (uname(&name) == 0) {
        os << name.sysname << ' ' << name.release << ' ' << name.machine;
    }
    // Inside the Steam Runtime or a Flatpak, /etc/os-release is the container's; the player's own system is in
    // /run/host/os-release.
    const std::string distro = OsReleaseName("/etc/os-release");
    const std::string hostDistro = OsReleaseName("/run/host/os-release");
    if (!distro.empty()) os << ", " << distro;
    if (!hostDistro.empty() && hostDistro != distro) os << " (host: " << hostDistro << ')';
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) os << ", " << ((static_cast<unsigned long long>(pages) * pageSize) >> 20) << " MiB RAM";

    AddEnvWord(launch, "XDG_SESSION_TYPE");
    AddEnvWord(launch, "XDG_CURRENT_DESKTOP");
    AddEnvFlag(launch, "WAYLAND_DISPLAY");
    AddEnvFlag(launch, "DISPLAY");
    AddEnvFlag(launch, "GAMESCOPE_WAYLAND_DISPLAY");  // set inside a gamescope session (Steam Deck / Bazzite game mode)
    AddEnvWord(launch, "SteamDeck");
    AddEnvWord(launch, "SteamOS");
    AddEnvWord(launch, "SteamGamepadUI");  // Big Picture / game mode
    AddEnvFlag(launch, "APPIMAGE");
    AddEnvWord(launch, "FLATPAK_ID");
    AddEnvFlag(launch, "PRESSURE_VESSEL_RUNTIME");  // the Steam Linux Runtime container
    AddEnvWord(launch, "SDL_VIDEODRIVER");
    AddEnvWord(launch, "SDL_VIDEO_DRIVER");
    AddEnvWord(launch, "SDL_AUDIO_DRIVER");
#endif
    os << ", " << std::thread::hardware_concurrency() << " threads";
    AddEnvWord(launch, "SteamAppId");
    AddEnvWord(launch, "RECOMPLAUNCHER");
    AddEnvWord(launch, "RECOMPLAUNCHER_MODE");
    AddEnvWord(launch, "RECOMPLAUNCHER_VERSION");

    // One fprintf per line: other threads log at the same time, and a C++ stream writes each << separately.
    const std::string launchText = launch.str();
    RT_LOGF(RT_TAG_SYSTEM, "os: %s\n", os.str().c_str());
    RT_LOGF(RT_TAG_SYSTEM, "launch:%s\n", launchText.empty() ? " plain (no launcher variables set)" : launchText.c_str());
}

void LogVideoDriver() {
    const char* driver = SDL_GetCurrentVideoDriver();
    RT_LOGF(RT_TAG_SYSTEM, "SDL video driver: %s\n", driver ? driver : "none");
}

}  // namespace SystemReport
