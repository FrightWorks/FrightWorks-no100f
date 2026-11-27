#include "no100f_discord_status.h"

#include "discord_presence.h"
#include "hle/no100f_xbox_layout.h"
#include "hle_stubs.h"
#include "memory.h"
#include "no100f_game_state.h"
#include "no100f_progress.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <chrono>

namespace no100f_discord_status {

void SampleGame() {
    if (!RuntimeConfigFile::DiscordPresenceEnabled() || RuntimeConfigFile::DiscordClientId().empty()) {
        return;
    }
    static std::chrono::steady_clock::time_point s_next{};
    static bool s_sent = false;
    static Status s_last;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next) {
        return;
    }
    s_next = now + std::chrono::seconds(1);

    namespace gs = no100f_game_state;
    Snapshot snapshot;
    snapshot.layoutVerified = gs::g_diagnosticLayoutVerified.load(std::memory_order_relaxed);
    uint32_t movie = 0;
    if (snapshot.layoutVerified && Memory::TryRead32(gs::kModeAddress, snapshot.mode) &&
        Memory::TryRead32(gs::kStateAddress, snapshot.state) && Memory::TryRead32(gs::kMovieAddress, movie)) {
        snapshot.movie = movie != 0;
        snapshot.credits = gs::g_creditsMovie.load(std::memory_order_relaxed);
        uint32_t scenePtr = 0;
        if (Memory::TryRead32(No100fXboxLayout::kScenePointer, scenePtr) && scenePtr != 0) {
            Memory::TryRead32(scenePtr + No100fXboxLayout::kSceneIdOffset, snapshot.scene);
        }
        if (const no100f_progress::Progress progress = no100f_progress::Latest(); progress.valid) {
            snapshot.snacksKnown = snapshot.percentKnown = true;
            snapshot.snacks = progress.snacks;
            snapshot.percent = progress.percent;
        }
    } else {
        snapshot.layoutVerified = false;
    }

    const Status status = Describe(snapshot);
    if (s_sent && status == s_last) {
        return;
    }
    s_sent = true;
    s_last = status;
    DiscordPresence::Activity activity;
    activity.details = status.details;
    activity.state = status.state;
    activity.largeImageKey = kLargeImage;
    activity.largeImageText = "Night of 100 Frights";
    activity.smallImageKey = kSmallImage;
    activity.smallImageText = "FrightWorks";
    activity.buttons = {{{kButton1Label, kButton1Url}, {kButton2Label, kButton2Url}}};
    DiscordPresence::SetBasicActivity(std::move(activity));
}

}  // namespace no100f_discord_status

// A wrapper (REPO.md, Build facts) on the game's completion function, which only its save calls (0x800CA190): the
// game's code runs unchanged, and the log gets its result next to the live one (no100f_progress.h), so a save shows
// whether the two agree.
extern "C" void func_800CCF24(CpuContext* ctx);
extern "C" void No100f_CompletionPercent(CpuContext* ctx) {
    func_800CCF24(ctx);
    const no100f_progress::Progress live = no100f_progress::ReadNow();
    const auto& in = live.inputs;
    RT_LOGF(RT_TAG_RUNTIME,
            "[save] completion %u%%, live %u%% (%s): flags %u, saved states %u, snacks collected %u, tokens %u%s\n",
            static_cast<uint32_t>(ctx->gpr[3]), live.percent,
            !live.valid ? "unreadable" : live.percent == static_cast<uint32_t>(ctx->gpr[3]) ? "same" : "DIFFERENT",
            in.progressFlags, in.savedStates, in.snacksCollected, in.tokens, in.finished ? ", finished" : "");
}
REGISTER_NATIVE_FUNCTION_AS(0x800CCF24, No100f_CompletionPercent, "No100f_CompletionPercent");
