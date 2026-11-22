#pragma once
// Stream voices (music): the stale end-of-track check (issue 14), fixed before the game's DVD read callback runs.
//
// The refill callback fn_800D9EF8 detects the end of a track by comparing bytes read (entry +0x90) with +0x94 each time
// the voice crosses into the first half of its ARAM ring: equal means no read since the last crossing, so the track
// ended; otherwise it stores bytes read in +0x94. The stream start fn_800D8CA8 resets +0x90 but never +0x94. The first
// crossing is always at read 0xBB80, so a stream stopped after exactly one crossing leaves 0xBB80 behind, and the next
// stream in that entry looks finished at its first crossing. The restart hits the same check and the equal branch never
// stores, so the music repeats about a second of its start until the game is closed (issue 14).
//
// Bytes read are 0 only before a stream's first chunk, on every path through fn_800D8CA8, and all three read sites use
// the DVD read callback fn_800DA71C (which adds the chunk to +0x90 and never touches +0x94), so the wrapper on that
// callback clears +0x94 on a live voice's first chunk, then runs the game's callback. A real track end still compares
// equal at a later crossing.
#include <cstdint>

namespace No100fStreamRead {

constexpr uint32_t kBytesRead = 0x90, kPrevRead = 0x94;

// read32/write32 take guest addresses; entry is the stream table entry (+0 = 0: voice already freed).
// Returns true when it cleared the previous read.
template <class Read32, class Write32>
bool ClearPrevReadOnFirstChunk(uint32_t entry, Read32&& read32, Write32&& write32) {
    if (read32(entry) == 0 || read32(entry + kBytesRead) != 0) return false;
    write32(entry + kPrevRead, 0);
    return true;
}

}  // namespace No100fStreamRead
