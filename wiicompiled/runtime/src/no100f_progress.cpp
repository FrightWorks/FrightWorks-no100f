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
    // The log gets every change of the completion, with what it is made of.
    static uint32_t s_logged = ~0u;
    if (progress.valid && progress.percent != s_logged) {
        s_logged = progress.percent;
        const auto& in = progress.inputs;
        RT_LOGF(RT_TAG_RUNTIME, "[progress] completion %u%%: flags %u, saved states %u, snacks collected %u, tokens %u%s\n",
                progress.percent, in.progressFlags, in.savedStates, in.snacksCollected, in.tokens,
                in.finished ? ", finished" : "");
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latest = progress;
}

Progress Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

}  // namespace no100f_progress
