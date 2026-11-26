#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

// Night of 100 Frights (GIHE78 rev 1): where the game keeps what it is doing, for overlays that depend on it.
namespace no100f_game_state {

// Game mode (r13-0x7468): 0/1 boot and the intro movies, 2 title screen and its menus, 4 load game, 6 the Mystery
// Machine (save) menu, 7 pause, 11 in play.
inline constexpr uint32_t kModeAddress = 0x80270E18;
// Game state (r13-0x7EB8). In mode 2: 0 the title screen and its menus, 1 the attract demo (after ~35 s idle).
inline constexpr uint32_t kStateAddress = 0x802703C8;
// The open Bink movie (r13-0x7230), 0 when none plays: the movie player stores BinkOpen's handle (0x800DABC4) and
// clears it when the movie closes (0x800DAD48). Movies play inside the current mode (the credits and trailers in 2).
inline constexpr uint32_t kMovieAddress = 0x80271050;
// 1 while the player has control (globals + 0x3B0); 0 during a recorded cutscene or an in-world camera pan (the i001
// key). In mode 11 with this at 1, Start opens the game's pause menu (mode 7).
inline constexpr uint32_t kPlayerControlAddress = 0x80245540;
inline constexpr uint32_t kModeInPlay = 11;
inline constexpr uint32_t kModePause = 7;

// Set only after the loader verifies a known layout, not merely an arbitrary build's generated hash.
inline std::atomic<bool> g_diagnosticLayoutVerified{false};

// The title screen and its menus (Press Start, New Game / Load Game / Options / ...), not the attract demo or a
// movie (credits, trailers).
constexpr bool IsMainMenu(uint32_t mode, uint32_t state, uint32_t movie) { return mode == 2 && state == 0 && movie == 0; }

// The last Bink movie the game opened is a credits roll (Credits1.bik / Credits2.bik): set by the DVD read hook on
// each movie's first read. With a movie open (kMovieAddress), the credits are on screen.
inline std::atomic<bool> g_creditsMovie{false};

// The file name of the last Bink movie the game opened (the DVD read hook), for the log's movie lines (vi.cpp).
inline std::mutex g_lastMovieMutex;
inline std::string g_lastMovieName;
inline void SetLastMovieName(std::string name) {
    std::lock_guard<std::mutex> lock(g_lastMovieMutex);
    g_lastMovieName = std::move(name);
}
inline std::string LastMovieName() {
    std::lock_guard<std::mutex> lock(g_lastMovieMutex);
    return g_lastMovieName.empty() ? "?" : g_lastMovieName;
}

// File name -> is it one of the credits movies (any case).
inline bool IsCreditsMovie(const std::string_view name) {
    constexpr std::string_view prefix = "credits";
    if (name.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if ((name[i] | 0x20) != prefix[i]) return false;
    return true;
}

} // namespace no100f_game_state
