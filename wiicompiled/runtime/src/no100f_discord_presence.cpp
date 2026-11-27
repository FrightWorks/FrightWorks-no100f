#include "no100f_discord_status.h"

#include "discord_presence.h"
#include "hle/no100f_xbox_layout.h"
#include "memory.h"
#include "no100f_devmenu_progress.h"
#include "no100f_game_state.h"
#include "runtime_config.h"

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
        uint32_t snacks = 0;
        if (Memory::TryRead32(no100f_devmenu::kPlayerSnacks, snacks) && snacks <= no100f_devmenu::kDiagnosticSnackLimit) {
            snapshot.snacksKnown = true;
            snapshot.snacks = snacks;
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
