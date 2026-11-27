#include "no100f_progress.h"

#include "memory.h"
#include "no100f_game_state.h"
#include "runtime_log.h"

#include <chrono>
#include <mutex>

namespace no100f_progress {
namespace {

std::mutex g_mutex;
Progress g_latest;

}  // namespace

Progress ReadNow() {
    Progress progress;
    if (!no100f_game_state::g_diagnosticLayoutVerified.load(std::memory_order_relaxed)) return progress;
    ReadProgress([](uint32_t address, uint32_t& value) { return Memory::TryRead32(address, value); }, progress);
    return progress;
}

void Update() {
    static std::chrono::steady_clock::time_point s_next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next) return;
    s_next = now + std::chrono::milliseconds(250);
    const Progress progress = ReadNow();
    uint32_t mode = 0, control = 0;
    Memory::TryRead32(no100f_game_state::kModeAddress, mode);
    Memory::TryRead32(no100f_game_state::kPlayerControlAddress, control);
    // A load fills these in over several frames while the level loads (seen: 0%, then 8%, then the save's 30%, all
    // in play mode), so a reading counts only once the player has control, which comes back after the level loaded.
    // The menus clear it: the next game may be a new one or another save.
    const bool menu = mode == 2 || mode == 4;
    const bool settled = progress.valid && control == 1;
    // The log gets every change of the completion, with what it is made of.
    static uint32_t s_logged = ~0u;
    static bool s_loggedSettled = false;
    if (progress.valid && (progress.percent != s_logged || settled != s_loggedSettled)) {
        s_logged = progress.percent;
        s_loggedSettled = settled;
        const auto& in = progress.inputs;
        RT_LOGF(RT_TAG_RUNTIME,
                "[progress] completion %u%% (%s, mode %u): flags %u, saved states %u, snacks collected %u, tokens %u%s\n",
                progress.percent, settled ? "shown" : "waiting for control", mode, in.progressFlags, in.savedStates,
                in.snacksCollected, in.tokens, in.finished ? ", finished" : "");
    }
    if (!settled && !menu) return;  // keep the last settled reading
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latest = settled ? progress : Progress{};
}

Progress Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

}  // namespace no100f_progress
