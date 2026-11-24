// NO100F: the player's Xbox sound in place of the GameCube's DSP-ADPCM.
//
// The Xbox disc has the same sounds as the GameCube (same assets, rates and lengths) as plain 16-bit PCM, where the
// GameCube's 4-bit ADPCM adds a flat hiss (51-62 dB under the music, 26-37 dB under the 22 kHz voice clips). The
// launcher writes them from the player's own Xbox disc into <UserData>/xbox/audio (launcher/XboxAudio.cs): audio.pcm
// (the samples) and audio.idx (per asset: GameCube ID, size, sample count, where its samples are; per 12000-byte chunk
// of the GameCube asset: the hash of its first 64 bytes and of the whole chunk).
//
// The game still loads and plays its own ADPCM; this only changes what a decoded sample comes out as. It keeps a map
// of ARAM, one entry per 8-byte ADPCM frame (14 samples): which sound and which of its frames is there.
//  - Every copy into ARAM (gc_hw.cpp) clears the map over what it wrote, then looks for whole chunks from the copy's
//    start: streams (music and speech) arrive as chunks, 0x2EE0 bytes or several at once.
//  - Resident sounds (effects, short clips) arrive in 72000-byte pieces that don't line up with them, so a voice that
//    starts on an unknown frame is recognised from its loop or start address: the chunk heads there, then every chunk
//    of that sound verified in ARAM.
//  - The decoder (ax_internal.h, AcceleratorT::ReadSample) keeps decoding the ADPCM (its predictor state stays the
//    game's) and returns the Xbox sample where the map knows the frame.
// Without the folder, or with a damaged one, nothing changes.
#include "hle/audio/xbox_audio.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" const uint8_t* GcAramHostBase();  // hle/os/gc_hw.cpp

namespace XboxAudio {

std::atomic<bool> g_active{false};

namespace {

constexpr uint32_t kAramSize = 16u << 20;
constexpr uint32_t kFrames = kAramSize / 8;
constexpr uint32_t kFrameBits = 21;  // an asset of up to 16 MB of ADPCM; the asset number goes above
constexpr uint32_t kHead = 64;

struct Asset {
    uint32_t id, gcBytes, samples, firstChunk;
    uint64_t pcmOffset;
};
struct Chunk {
    uint64_t head, hash;
    uint32_t asset, offset, length, pad;
};
static_assert(sizeof(Asset) == 24 && sizeof(Chunk) == 32, "audio.idx record sizes");

std::vector<Asset> g_assets;
std::vector<Chunk> g_chunks;
std::unordered_multimap<uint64_t, uint32_t> g_heads;  // head hash -> chunk
const uint8_t* g_pcm = nullptr;
uint64_t g_pcmSize = 0;
std::unique_ptr<std::atomic<uint32_t>[]> g_map;      // per ARAM frame: (asset + 1) << kFrameBits | frame, 0 = unknown
std::mutex g_mutex;
uint32_t g_generation = 1;                            // bumped by every ARAM copy
std::unordered_map<uint32_t, uint32_t> g_tried;       // voice start byte -> generation it was last looked up in
uint64_t g_streamChunks = 0, g_voices = 0;

const uint8_t* MapFile(const std::filesystem::path& path, uint64_t& size) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0) { CloseHandle(file); return nullptr; }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return nullptr;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);  // the view keeps the mapping alive
    size = uint64_t(length.QuadPart);
    return static_cast<const uint8_t*>(view);
#else
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return nullptr; }
    void* view = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (view == MAP_FAILED) return nullptr;
    size = uint64_t(st.st_size);
    return static_cast<const uint8_t*>(view);
#endif
}

// Loads the index and maps the samples, once (on the first ARAM copy, on the game thread).
void Load() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    std::error_code ec;
    const auto dir = RuntimeConfigFile::ApplicationDataDirectory() / "xbox" / "audio";
    if (!std::filesystem::is_regular_file(dir / "audio.idx", ec)) return;
    std::ifstream in(dir / "audio.idx", std::ios::binary);
    char magic[4];
    uint32_t version = 0, assets = 0, chunks = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&assets), 4);
    in.read(reinterpret_cast<char*>(&chunks), 4);
    if (!in || std::memcmp(magic, "FWXA", 4) != 0 || version != 1 || assets == 0 || assets >= (1u << (32 - kFrameBits)) - 1 ||
        chunks > (1u << 24)) {
        RT_LOG(RT_TAG_AUDIO) << "[xbox-audio] " << RuntimeConfigFile::PathToUtf8(dir / "audio.idx") << " is damaged: the GameCube's sound plays\n";
        return;
    }
    g_assets.resize(assets);
    g_chunks.resize(chunks);
    in.read(reinterpret_cast<char*>(g_assets.data()), std::streamsize(assets * sizeof(Asset)));
    in.read(reinterpret_cast<char*>(g_chunks.data()), std::streamsize(chunks * sizeof(Chunk)));
    g_pcm = in ? MapFile(dir / "audio.pcm", g_pcmSize) : nullptr;
    bool ok = g_pcm != nullptr;
    for (const Asset& a : g_assets)
        ok = ok && a.pcmOffset + uint64_t(a.samples) * 2 <= g_pcmSize && a.gcBytes / 8 < (1u << kFrameBits);
    for (const Chunk& c : g_chunks) ok = ok && c.asset < assets && c.length > 0 && c.offset + c.length <= g_assets[c.asset].gcBytes;
    if (!ok) {
        RT_LOG(RT_TAG_AUDIO) << "[xbox-audio] " << RuntimeConfigFile::PathToUtf8(dir) << " doesn't match itself: the GameCube's sound plays\n";
        g_assets.clear();
        g_chunks.clear();
        return;
    }
    for (uint32_t i = 0; i < chunks; ++i)
        if (g_chunks[i].length >= kHead) g_heads.emplace(g_chunks[i].head, i);
    g_map.reset(new std::atomic<uint32_t>[kFrames]);
    for (uint32_t i = 0; i < kFrames; ++i) g_map[i].store(0, std::memory_order_relaxed);
    RT_LOG(RT_TAG_AUDIO) << "[xbox-audio] " << assets << " Xbox sounds (" << (g_pcmSize >> 20) << " MB) in "
                         << RuntimeConfigFile::PathToUtf8(dir) << "\n";
    g_active.store(true, std::memory_order_release);
}

// Marks ARAM [at, at + chunk.length) as that chunk of its asset, and touches its samples (page them in now, on the
// game thread, rather than while mixing).
void MarkChunk(uint32_t at, const Chunk& c) {
    const uint32_t value = (c.asset + 1) << kFrameBits;
    const uint32_t first = c.offset / 8, n = c.length / 8;
    for (uint32_t i = 0; i < n && at / 8 + i < kFrames; ++i) g_map[at / 8 + i].store(value | (first + i), std::memory_order_relaxed);
    const Asset& a = g_assets[c.asset];
    const uint64_t from = a.pcmOffset + uint64_t(first) * 28, to = std::min(a.pcmOffset + uint64_t(a.samples) * 2, from + uint64_t(n) * 28);
    volatile uint8_t sink = 0;
    for (uint64_t p = from; p < to; p += 4096) sink = sink + g_pcm[p];
}

// The chunk whose bytes are in ARAM at `at` (and that ends by `limit`), if `pred` accepts it: by head, then whole.
template <typename Pred>
const Chunk* FindChunk(const uint8_t* aram, uint32_t at, uint32_t limit, Pred pred) {
    if (at + kHead > limit) return nullptr;
    const uint64_t head = XXH64(aram + at, kHead, 0);
    auto [b, e] = g_heads.equal_range(head);
    for (auto it = b; it != e; ++it) {
        const Chunk& c = g_chunks[it->second];
        if (at + c.length > limit || !pred(c)) continue;
        if (XXH64(aram + at, c.length, 0) == c.hash) return &c;
    }
    return nullptr;
}

}  // namespace

void OnAramWrite(uint32_t aramAddr, uint32_t length) {
    Load();
    if (!g_active.load(std::memory_order_relaxed) || aramAddr >= kAramSize || length == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_generation;
    const uint32_t end = std::min(kAramSize, aramAddr + length);
    for (uint32_t f = aramAddr / 8; f < (end + 7) / 8; ++f) g_map[f].store(0, std::memory_order_relaxed);
    if (aramAddr % 8 != 0) return;
    const uint8_t* aram = GcAramHostBase();
    for (uint32_t at = aramAddr; at + kHead <= end;) {
        const Chunk* c = FindChunk(aram, at, end, [](const Chunk&) { return true; });
        if (!c) {
            if (static const bool log = std::getenv("XBOX_AUDIO_LOG") != nullptr; log)
                if (static unsigned shown = 0; length == 0x2EE0 && shown++ < 40) {
                    char hex[64 * 2 + 1];
                    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", aram[at + i]);
                    RT_LOG(RT_TAG_AUDIO) << "[xbox-audio] unknown chunk at ARAM " << std::hex << at << std::dec << ": " << hex << "\n";
                }
            break;  // streams come in whole chunks from the copy's start; anything else is left to PrepareVoice
        }
        MarkChunk(at, *c);
        ++g_streamChunks;
        if (c->length != 0x2EE0) break;
        at += c->length;
    }
}

void PrepareVoice(uint32_t curNibble, uint32_t loopNibble, bool stream, uint16_t format) {
    if (!g_active.load(std::memory_order_relaxed) || stream || (format & 0xF) != 0) return;  // ADPCM only
    if (g_map[(curNibble >> 4) % kFrames].load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint8_t* aram = GcAramHostBase();
    for (const uint32_t nibble : {loopNibble, curNibble}) {
        const uint32_t start = ((nibble >> 4) * 8) % kAramSize;
        auto [it, fresh] = g_tried.try_emplace(start, g_generation);
        if (!fresh && it->second == g_generation) continue;
        it->second = g_generation;
        if (g_tried.size() > 4096) g_tried.clear();
        // The sound's first chunk, then each following one in place: all of it must be there.
        const Chunk* c = FindChunk(aram, start, kAramSize, [](const Chunk& k) { return k.offset == 0; });
        if (!c) continue;
        const Asset& a = g_assets[c->asset];
        std::vector<const Chunk*> found{c};
        for (uint32_t i = a.firstChunk + 1; i < g_chunks.size() && g_chunks[i].asset == c->asset; ++i) {
            const Chunk& k = g_chunks[i];
            const uint32_t at = start + k.offset;
            if (at + k.length > kAramSize || XXH64(aram + at, k.length, 0) != k.hash) { found.clear(); break; }
            found.push_back(&k);
        }
        if (found.empty()) continue;
        for (const Chunk* k : found) MarkChunk(start + k->offset, *k);
        if (++g_voices <= 3 || g_voices % 200 == 0)
            RT_LOG(RT_TAG_AUDIO) << "[xbox-audio] sound " << std::hex << a.id << std::dec << " at ARAM " << std::hex << start << std::dec
                                 << " (" << g_voices << " recognised, " << g_streamChunks << " stream chunks)\n";
        return;
    }
}

// Diagnostic (env XBOX_AUDIO_LOG): every 480000 ADPCM samples decoded (all voices), how many were swapped and how
// close the Xbox samples are to the ADPCM ones they replace (the offline comparison gives 26-62 dB; a sample-misaligned
// swap gives about 0 dB).
static void Stats(bool hit, int16_t decoded, int16_t xbox) {
    static const bool log = std::getenv("XBOX_AUDIO_LOG") != nullptr;
    if (!log) return;
    static uint64_t hits = 0, misses = 0;
    static double signal = 0, error = 0;
    if (hit) {
        ++hits;
        signal += double(xbox) * xbox;
        error += (double(xbox) - decoded) * (double(xbox) - decoded);
    } else {
        ++misses;
    }
    if (hits + misses >= 480000) {
        RT_LOGF(RT_TAG_AUDIO, "[xbox-audio] %.1f%% of ADPCM samples swapped, Xbox vs ADPCM %.1f dB, %llu stream chunks, %llu sounds\n",
                100.0 * double(hits) / double(hits + misses), error > 0 ? 10.0 * std::log10(signal / error) : 99.0,
                static_cast<unsigned long long>(g_streamChunks), static_cast<unsigned long long>(g_voices));
        hits = misses = 0;
        signal = error = 0;
    }
}

int16_t SampleSlow(uint32_t nibble, int16_t decoded) {
    const uint32_t e = g_map[(nibble >> 4) % kFrames].load(std::memory_order_relaxed);
    const uint32_t inFrame = nibble & 15;
    if (!e || inFrame < 2) { Stats(false, decoded, decoded); return decoded; }
    const Asset& a = g_assets[(e >> kFrameBits) - 1];
    const uint64_t s = uint64_t(e & ((1u << kFrameBits) - 1)) * 14 + (inFrame - 2);
    if (s >= a.samples) { Stats(false, decoded, decoded); return decoded; }
    const uint8_t* p = g_pcm + a.pcmOffset + s * 2;
    const int16_t xbox = static_cast<int16_t>(uint16_t(p[0] | (p[1] << 8)));
    Stats(true, decoded, xbox);
    return xbox;
}

}  // namespace XboxAudio
