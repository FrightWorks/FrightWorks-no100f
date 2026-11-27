#include "no100f_discord_status.h"

#include "discord_presence.h"
#include "hle/no100f_xbox_layout.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "no100f_devmenu_progress.h"
#include "no100f_game_state.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <chrono>

namespace no100f_discord_status {
namespace {

// The completion a save writes (the load screen's "NN%"): the game works it out only while saving, from progress
// that is rebuilt for the save, so it is taken from the game rather than worked out here.
std::atomic<int> g_savedPercent{-1};

// The load screen's slot records (3 of 0x64 bytes; +0x60 the completion read from that save) and the slot in use
// (r13-0x7E40), as the game's own save code reads them.
constexpr uint32_t kSlotRecords = 0x802486F8, kSlotRecordSize = 0x64, kSlotPercent = 0x60, kSlotCount = 3;
constexpr uint32_t kCurrentSlot = 0x80270440;
// Snacks collected over the whole game (one of the completion's inputs; the game divides it by 6748, every snack).
// 0 on a new game, whose slot record may still hold the save it replaces.
constexpr uint32_t kSnacksCollected = 0x80245E3C;

// The completion to show: the last save's, else the loaded save's from the slot list, latched when play starts.
bool ReadPercent(bool menu, uint32_t& percent) {
    static int s_percent = -1;
    static bool s_fromMenu = true;
    if (menu) {
        s_percent = -1;
        s_fromMenu = true;
        g_savedPercent.store(-1, std::memory_order_relaxed);
        return false;
    }
    if (const int saved = g_savedPercent.exchange(-1, std::memory_order_relaxed); saved >= 0) {
        s_percent = saved;
        s_fromMenu = false;
    } else if (s_fromMenu) {
        s_fromMenu = false;
        uint32_t slot = 0, loaded = 0, collected = 0;
        if (Memory::TryRead32(kCurrentSlot, slot) && slot < kSlotCount &&
            Memory::TryRead32(kSlotRecords + slot * kSlotRecordSize + kSlotPercent, loaded) && loaded <= 100 &&
            Memory::TryRead32(kSnacksCollected, collected) && collected != 0) {
            s_percent = int(loaded);
        }
    }
    if (s_percent < 0) return false;
    percent = uint32_t(s_percent);
    return true;
}

}  // namespace

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
        const bool menu = snapshot.mode == kModeTitle || snapshot.mode == kModeLoad;
        const bool level = snapshot.mode == kModeInPlay || snapshot.mode == kModePause || snapshot.mode == kModeSave;
        if (menu || level) snapshot.percentKnown = ReadPercent(menu, snapshot.percent);
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

// A wrapper (REPO.md, Build facts): the game's completion function (called only by its save, 0x800CA190) runs
// unchanged; its result is kept for the status.
extern "C" void func_800CCF24(CpuContext* ctx);
extern "C" void No100f_CompletionPercent(CpuContext* ctx) {
    func_800CCF24(ctx);
    const uint32_t percent = static_cast<uint32_t>(ctx->gpr[3]);
    if (percent <= 100) no100f_discord_status::g_savedPercent.store(int(percent), std::memory_order_relaxed);
    RT_LOGF(RT_TAG_RUNTIME, "[save] completion %u%%\n", percent);
}
REGISTER_NATIVE_FUNCTION_AS(0x800CCF24, No100f_CompletionPercent, "No100f_CompletionPercent");
