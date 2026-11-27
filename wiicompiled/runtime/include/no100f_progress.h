#pragma once

#include "no100f_devmenu_progress.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

// Night of 100 Frights: the player's progress, worked out live from game memory (read only), for anything that shows
// it: the Discord status now, an on-screen display later. Sampled on the game thread (Update, vi.cpp); read with
// Latest from anywhere.
//
// The completion is the game's own number, the one its load screen shows for a save. The game works it out only while
// saving, from four things it keeps all the time (the addresses below), weighted 43/20/20/15, plus 2 once a flag is
// set, rounded down and capped at 100 (found in main.dol at 0x800CCF24; the save wrapper in
// no100f_discord_presence.cpp logs the game's own result next to this one at every save).
namespace no100f_progress {

// The progress word after the snack count: 15 flags, of which the completion counts any, out of 13.
inline constexpr uint32_t kProgressFlags = 0x80245748;
inline constexpr uint32_t kProgressFlagMask = 0x7FFF, kProgressFlagTotal = 13;
// Monster tokens: 21 flags from bit 0.
inline constexpr uint32_t kTokenFlags = 0x8024574C;
inline constexpr uint32_t kTokenTotal = 21;
// Snacks collected over the whole game, out of every snack in the levels.
inline constexpr uint32_t kSnacksCollected = 0x80245E3C;
inline constexpr uint32_t kSnackTotal = 6748;
// Nonzero once the game is finished (r13-0x7294): 2 more points.
inline constexpr uint32_t kFinished = 0x80270FEC;
// The game's table of saved object states (each object it remembers across scenes): +0x14 the records, +0x18 how
// many. A record: +0 the object's tag, +4 its state bits (words), +0xC their size in bytes. The completion counts the
// records whose first state bit is set, the player's ('PLYR') left out, out of 86. The bit mask table at kBitMasks is
// filled at boot: entry 0 is the first bit.
inline constexpr uint32_t kSavedStates = 0x801C4520;
inline constexpr uint32_t kBitMasks = 0x801C4544;
inline constexpr uint32_t kPlayerTag = 0x504C5952;  // 'PLYR'
inline constexpr uint32_t kSavedStateTotal = 86;
inline constexpr uint32_t kSavedStateLimit = 4096;  // more records than this: not the table we know

struct Inputs {
    uint32_t progressFlags = 0;  // set flags counted (of the 15 bits)
    uint32_t savedStates = 0;    // object states with their first bit set
    uint32_t snacksCollected = 0;
    uint32_t tokens = 0;
    bool finished = false;
};

inline uint32_t CountBits(uint32_t value) {
    uint32_t count = 0;
    for (; value; value &= value - 1) ++count;
    return count;
}

// The game's arithmetic, single precision as on the GameCube: each part divided by its total, the weighted sum built
// with fused multiply-adds in the game's order, then the bonus added and the result truncated.
inline uint32_t Completion(const Inputs& in) {
#pragma float_control(precise, on)
    const float flags = float(in.progressFlags) / float(kProgressFlagTotal);
    const float states = float(int32_t(in.savedStates)) / float(kSavedStateTotal);
    const float snacks = float(in.snacksCollected) / float(kSnackTotal);
    const float tokens = float(in.tokens) / float(kTokenTotal);
    float sum = 20.0f * states;
    sum = std::fmaf(43.0f, flags, sum);
    sum = std::fmaf(20.0f, snacks, sum);
    sum = std::fmaf(15.0f, tokens, sum);
    const float total = float(in.finished ? 2 : 0) + sum;
    uint32_t percent = total <= 0.0f ? 0u : uint32_t(total);
    // Any flag, snack or token counts as at least 1% (the game's own rule; the saved states are not in it).
    if (percent == 0 && (in.progressFlags || in.snacksCollected || in.tokens)) percent = 1;
    return percent > 100 ? 100 : percent;
}

struct LevelSnacks {
    uint32_t collected = 0, total = 0;
};

struct Progress {
    bool valid = false;              // read from a known layout; nothing below means anything otherwise
    uint32_t percent = 0;            // the completion, 0-100
    uint32_t snacks = 0;             // the Scooby Snacks the player holds now (spent at snack gates)
    Inputs inputs;                   // what the completion is made of
    bool levelsKnown = false;
    std::array<LevelSnacks, 30> levels{};  // snacks per level, the game's snack-area order (0 unused)
};

template <typename Read>
bool ReadSavedStates(Read read, uint32_t& count) {
    uint32_t records = 0, size = 0, mask = 0;
    if (!read(kSavedStates + 0x14, records) || !read(kSavedStates + 0x18, size) || !read(kBitMasks, mask)) return false;
    if (size > kSavedStateLimit || (size != 0 && !no100f_devmenu::GuestRange(records, size * 4)) || mask == 0)
        return false;
    count = 0;
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t record = 0, tag = 0, bytes = 0, bits = 0, word = 0;
        if (!read(records + i * 4, record) || !no100f_devmenu::GuestRange(record, 0x10) || !read(record, tag) ||
            !read(record + 0xC, bytes))
            return false;
        if (tag == kPlayerTag || bytes == 0) continue;
        if (!read(record + 4, bits) || !no100f_devmenu::GuestRange(bits, 4) || !read(bits, word)) return false;
        if (word & mask) ++count;
    }
    return true;
}

template <typename Read>
bool ReadProgress(Read read, Progress& out) {
    out = {};
    uint32_t flags = 0, tokens = 0, finished = 0;
    if (!read(no100f_devmenu::kPlayerSnacks, out.snacks) || !read(kProgressFlags, flags) ||
        !read(kTokenFlags, tokens) || !read(kSnacksCollected, out.inputs.snacksCollected) ||
        !read(kFinished, finished) || !ReadSavedStates(read, out.inputs.savedStates))
        return false;
    if (out.snacks > no100f_devmenu::kDiagnosticSnackLimit) return false;
    out.inputs.progressFlags = CountBits(flags & kProgressFlagMask);
    out.inputs.tokens = CountBits(tokens & ((1u << kTokenTotal) - 1));
    out.inputs.finished = finished != 0;
    out.percent = Completion(out.inputs);
    no100f_devmenu::SnackAreas areas;
    if (no100f_devmenu::ReadSnackAreas(read, areas)) {
        out.levelsKnown = true;
        for (size_t i = 0; i < areas.size(); ++i) out.levels[i] = {areas[i].collected, areas[i].target};
    }
    out.valid = true;
    return true;
}

// Game thread, every retrace: reads the progress about four times a second. A reading is kept only while the player
// has control (a load fills the values in over several frames); the menus clear it.
void Update();

// The newest reading (valid = false before the first, or outside a known layout).
Progress Latest();

// Reads it now (game thread only), for the save wrapper's comparison.
Progress ReadNow();

}  // namespace no100f_progress
