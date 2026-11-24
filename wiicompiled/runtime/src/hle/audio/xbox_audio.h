// NO100F: the player's Xbox sound in place of the GameCube's DSP-ADPCM (hle/audio/xbox_audio.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace XboxAudio {

// Set once the player's Xbox sound is loaded; until then (and without it) nothing here changes a sample.
extern std::atomic<bool> g_active;

// After the game copies `length` bytes into ARAM at `aramAddr` (main RAM to ARAM): forget what was there and
// recognise the GameCube chunks the copy holds (streamed music and speech come in whole chunks).
void OnAramWrite(uint32_t aramAddr, uint32_t length);

// Before a GameCube voice decodes: a resident sound (not a stream) is recognised from its start in ARAM.
void PrepareVoice(uint32_t curNibble, uint32_t loopNibble, bool stream, uint16_t format);

// The sample to play for the ADPCM nibble at `nibble` (an ARAM nibble address): the Xbox sample when that ARAM frame
// holds a recognised sound, else `decoded`.
int16_t SampleSlow(uint32_t nibble, int16_t decoded);
inline int16_t Sample(uint32_t nibble, int16_t decoded) {
    return g_active.load(std::memory_order_acquire) ? SampleSlow(nibble, decoded) : decoded;
}

}  // namespace XboxAudio
