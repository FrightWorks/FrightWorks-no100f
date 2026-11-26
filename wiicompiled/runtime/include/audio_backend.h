#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL3/SDL_audio.h>

class AudioBackend {
public:
    static AudioBackend& Instance();

    bool Init(uint32_t sampleRate, uint32_t channels);
    void Shutdown();

    // Wii AI DMA frames are big-endian and ordered right, left. SDL expects
    // native-endian interleaved left, right samples.
    bool PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes);
    bool PushSamplesLE16(const int16_t* samples, size_t sampleCount);

    // Applied to the final host output, covering both AX and direct AI DMA.
    void SetMasterVolume(float volume);
    void SetMuted(bool muted);

    // Gets a copy of every block sent to the output (interleaved left/right S16, before the volume and mute), also
    // the blocks a full queue drops. nullptr = none. Called on the audio thread with the backend's lock held.
    using CaptureFn = void (*)(const int16_t* samples, size_t frames, uint32_t channels, uint32_t sampleRate);
    void SetCapture(CaptureFn capture);

private:
    AudioBackend() = default;
    ~AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels);
    bool QueueHasCapacityLocked(int incomingBytes);
    uint32_t QueueLimitBytesLocked() const;
    float EffectiveGainLocked() const;
    void ApplyGainLocked();

    mutable std::mutex m_mutex;
    SDL_AudioStream* m_stream = nullptr;
    SDL_AudioSpec m_spec{};
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    bool m_initialized = false;
    float m_masterVolume = 1.0f;
    bool m_muted = false;
    bool m_reportedDroppedBlock = false;
    uint32_t m_failedInits = 0;  // failed output opens since the last success; retried every 5 s
    std::chrono::steady_clock::time_point m_lastFailedInit{};
    std::vector<int16_t> m_convertBuffer;
    CaptureFn m_capture = nullptr;
};
