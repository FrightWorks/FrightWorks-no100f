// NO100F live cutscenes: plays the PS2 version's in-engine cutscenes (CSN assets) where the GameCube plays a Bink movie.
//
// The GameCube game kept every cutscene manager (CSNM), the cutscene_toc and the soundtracks, but plays a movie for
// each one: its CSN assets are gone and so is the code that played them. The player supplies the CSN assets from their
// own PS2 disc (tools/debug/game/ps2_cutscenes.py puts them back into the levels), and this file plays them with our
// own code (format and evaluator: hle/no100f_csn.h), drawing through the game's model functions. When a level has no
// CSN for a cutscene, nothing here acts and the game plays its movie as before.
//
// Wrappers (REGISTER_NATIVE_FUNCTION_AS; the game's functions stay translated as func_<address>):
//   0x80046054 cutscene manager event callback (r4 manager, r5 event): after the game's start event (0xBE), load the
//              CSN when the level has one; on reset (0xA) drop it.
//   0x80045F08 cutscene manager update (r3 manager): the game's update sends "play" (0x18), whose handler shows the
//              movie, and ends the cutscene in the same call. While a CSN plays, this sends "play" itself (the handler
//              still marks the cutscene unlocked for the gallery) and keeps the game's update from running until the
//              CSN ends or is skipped; then the game's update runs its own end (music, links, player, clip planes).
//   0x800ABBA4 movie player: skipped only when called from that handler (return address 0x8004613C) for a CSN.
//   0x8001E9DC camera update (r3 camera frame, f1 dt): after the game's update, the cutscene camera replaces its matrix
//              and field of view.
//   0x8006CA9C projectile pool render: its one caller (0x800CDE84) runs right after zRenderState(2), where the PS2
//              drew its cutscenes; the cutscene's models are drawn first.
//   0x80104724 / 0x80104820 OSAllocFromHeap / OSFreeToHeap: route the cutscene's memory to its own heap (see
//              RouteToOwnHeap).
// Models are built with the game's loader (RenderWare objects outlive the scene, so they are destroyed here) and drawn
// with its cull-free render call and our own matrix arrays (the game's instance pools are too small for 36-53 bone
// cutscene rigs). Faces and mouths: a morph block's two current targets go to the game's morph render (which still
// exists on the GameCube and reads the same target layout, big-endian), with the weight the PS2 used.
#include "abi_bridge.h"
#include "hle/no100f_csn.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath);

extern "C" void func_80046054(CpuContext* ctx);  // cutscene manager event callback
extern "C" void func_80045F08(CpuContext* ctx);  // cutscene manager update
extern "C" void func_800ABBA4(CpuContext* ctx);  // movie player (name, flags, volume)
extern "C" void func_8001E9DC(CpuContext* ctx);  // camera update
extern "C" void func_8006CA9C(CpuContext* ctx);  // projectile pool render
extern "C" void func_8000E978(CpuContext* ctx);  // model from a RenderWare stream (buffer, size) -> first atomic
extern "C" void func_8000E910(CpuContext* ctx);  // model unload (atomic)
extern "C" void func_8000E89C(CpuContext* ctx);  // next atomic of the model, 0 at the end
extern "C" void func_8000E828(CpuContext* ctx);  // bone count of the model
extern "C" void func_8000E39C(CpuContext* ctx);  // model render (atomic, matrices: [0] world, [1..] bones)
extern "C" void func_8000EBA4(CpuContext* ctx);  // morph render (atomic, matrices, s16* targets[4], s16 weights[4],
                                                 // normals, scale): blends the targets into the geometry and draws it
extern "C" void func_8000E4C0(CpuContext* ctx);  // bone matrices (atomic, quaternions, translations, out)
extern "C" void func_8002D3A0(CpuContext* ctx);  // quaternion to matrix (quaternion, out)
extern "C" void func_80007400(CpuContext* ctx);  // camera position (RwCamera, matrix)
extern "C" void func_80007394(CpuContext* ctx);  // camera field of view (RwCamera, degrees)
extern "C" void func_800072F4(CpuContext* ctx);  // near/far clip (0, 0 = defaults)
extern "C" void func_8003D074(CpuContext* ctx);  // sound play (id, vol l, vol r, pitch, priority, flags, parent)
extern "C" void func_8003CDEC(CpuContext* ctx);  // sound stop (handle)
extern "C" void func_8003D318(CpuContext* ctx);  // stop all sounds
extern "C" void func_80048B2C(CpuContext* ctx);  // entity event (from, to, event)
extern "C" void func_80048BE0(CpuContext* ctx);  // entity event (to, event)
extern "C" void func_8003F920(CpuContext* ctx);  // HIP path of the package holding an asset ID
extern "C" void func_8003FF84(CpuContext* ctx);  // find a loaded asset (id, size out)
extern "C" void func_8002FD90(CpuContext* ctx);  // OS heap allocation (size)
extern "C" void func_8002FD70(CpuContext* ctx);  // OS heap free
extern "C" void func_800C4A54(CpuContext* ctx);  // particle emitter by ID
extern "C" void func_80104724(CpuContext* ctx);  // OSAllocFromHeap (heap, size)
extern "C" void func_80104820(CpuContext* ctx);  // OSFreeToHeap (heap, block)
extern "C" void func_8010491C(CpuContext* ctx);  // OSCreateHeap (start, end) -> heap or -1

namespace No100fCutscenePlayer {

using namespace No100fCsn;

constexpr uint32_t kGlobals = 0x80245190;
constexpr uint32_t kCutsceneManager = kGlobals + 0x860;
constexpr uint32_t kPlayer = kGlobals + 0x1F8;   // the player entity sits inside the globals
constexpr uint32_t kCameraMatrix = kGlobals + 0xC0;
constexpr uint32_t kCameraFov = kGlobals + 0x110;
constexpr uint32_t kPad0 = kGlobals + 0x1F0;
constexpr uint32_t kFrameTime = 0x80270438;
constexpr uint32_t kSkippable = 0x80270AB0;     // the handler copies the cutscene's "can skip" flag here
constexpr uint32_t kMovieCallReturn = 0x8004613C;
constexpr uint32_t kSkipButtons = 0x50000;      // A or Y
constexpr float kSkipAfter = 4.0f;
constexpr uint32_t kMaxBones = 64;
constexpr uint32_t kMatrixBytes = 64;
constexpr uint32_t kUserDataOffset = 0x80278280 - 0x6D20;  // r13 - 0x6D20: the geometry's user-data plugin offset

enum : uint32_t { kEventReset = 0x0A, kEventPlay = 0x18, kEventStart = 0xBE };
enum : uint32_t { kPlayerControlOff = 8, kPlayerInvisible = 4 };

// Per-cutscene near/far clip planes for a frame window (30 fps frames), as the PS2 version set them.
struct ClipWindow { uint32_t csn; uint32_t frame0, frame1; float nearClip, farClip; };
constexpr std::array<ClipWindow, 4> kClipWindows{{
    {0xA1F375F2, 0, 1391, 1.0f, 200.0f},     // cin_1
    {0xA1F375F3, 2287, 2400, 2.0f, 200.0f},  // cin_2
    {0xA1F375F4, 0, 4000, 1.0f, 200.0f},     // cin_3
    {0xA1F375F8, 0, 5000, 0.5f, 100.0f},     // cin_7
}};

// A guest call made on the wrapper's own context (the scheduler tracks that context: a game function that blocks
// while running on a copy can't be switched away from), with the register state put back afterwards.
uint32_t Call(CpuContext* ctx, void (*fn)(CpuContext*), std::initializer_list<uint32_t> gprs,
              std::initializer_list<double> fprs = {}) {
    uint32_t gpr[32];
    PPC_FPR fpr[32];
    std::memcpy(gpr, ctx->gpr, sizeof gpr);
    std::memcpy(fpr, ctx->fpr, sizeof fpr);
    const uint32_t cr = ctx->cr, lr = ctx->lr, ctr = ctx->ctr, xer = ctx->xer;
    int r = 3;
    for (uint32_t v : gprs) ctx->gpr[r++] = v;
    int f = 1;
    for (double v : fprs) ctx->fpr[f++].d = v;
    fn(ctx);
    const uint32_t result = ctx->gpr[3];
    std::memcpy(ctx->gpr, gpr, sizeof gpr);
    std::memcpy(ctx->fpr, fpr, sizeof fpr);
    ctx->cr = cr;
    ctx->lr = lr;
    ctx->ctr = ctr;
    ctx->xer = xer;
    return result;
}

// The cutscene's own guest heap. A cutscene's RenderWare objects need 1-2 MB; the OS heap the game's RenderWare
// allocates from has about 1.1 MB free in a level (h001's cin_1 ran it dry at its 10th of 18 models, and RenderWare's
// failure path then wrote through a null pointer into the OS globals). The PS2 had 32 MB. So while this file's own
// guest calls run (load, draw, unload), the game's OS-heap allocations come from a second OS heap in MEM2, which the
// runtime maps but a GameCube game never uses, and every free goes to whichever heap owns the address.
constexpr uint32_t kHeapTable = 0x80271308;  // OS heap table pointer, heap count at +4; 12-byte entries, size < 0 = free
constexpr uint32_t kOsHeap = 0x80270788;     // the OS heap the game allocates from
constexpr uint32_t kOwnHeapBase = 0x92000000;
constexpr uint32_t kOwnHeapSize = 0x01000000;
int32_t g_ownHeap = -1;
int g_routeDepth = 0;  // > 0 while this file's guest calls run

struct RouteToOwnHeap {
    RouteToOwnHeap() { ++g_routeDepth; }
    ~RouteToOwnHeap() { --g_routeDepth; }
};

bool InOwnHeap(uint32_t p) { return p >= kOwnHeapBase && p < kOwnHeapBase + kOwnHeapSize; }

bool EnsureOwnHeap(CpuContext* ctx) {
    if (g_ownHeap >= 0) return true;
    if (!Memory::Contains(kOwnHeapBase, kOwnHeapSize)) return false;
    const uint32_t table = Memory::Read32(kHeapTable);
    const int32_t count = int32_t(Memory::Read32(kHeapTable + 4));
    if (count <= 0 || count > 64 || !Memory::Contains(table, uint32_t(count) * 12)) return false;
    bool freeSlot = false;
    for (int32_t i = 0; i < count; ++i) freeSlot |= int32_t(Memory::Read32(table + 12 * i)) < 0;
    uint32_t start = kOwnHeapBase;
    if (!freeSlot) {
        // The game made its heap table exactly as big as it needed: move it to the front of our region, one longer.
        for (int32_t i = 0; i < count * 3; ++i) Memory::Write32(kOwnHeapBase + 4 * i, Memory::Read32(table + 4 * i));
        Memory::Write32(kOwnHeapBase + 12 * count, 0xFFFFFFFF);
        Memory::Write32(kOwnHeapBase + 12 * count + 4, 0);
        Memory::Write32(kOwnHeapBase + 12 * count + 8, 0);
        Memory::Write32(kHeapTable, kOwnHeapBase);
        Memory::Write32(kHeapTable + 4, uint32_t(count + 1));
        start = kOwnHeapBase + 0x100;
    }
    const int32_t heap = int32_t(Call(ctx, func_8010491C, {start, kOwnHeapBase + kOwnHeapSize}));
    if (heap < 0) return false;
    g_ownHeap = heap;
    RT_LOG(RT_TAG_RUNTIME) << "[cutscene] own heap " << heap << " at " << std::hex << start << std::dec << "\n";
    return true;
}

uint32_t GuestAlloc(CpuContext* ctx, uint32_t size) {
    const uint32_t p = Call(ctx, func_8002FD90, {size + 32});
    return p && Memory::Contains(p, size + 32) ? p : 0;
}

uint32_t Align32(uint32_t p) { return (p + 31) & ~31u; }

void WriteMatrix(uint32_t at, const float rows[3][3], const Vec3& pos) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) Memory::WriteFloat32(at + 0x10 * r + 4 * c, rows[r][c]);
    }
    Memory::Write32(at + 0x0C, 3);  // orthonormal
    Memory::Write32(at + 0x1C, 0);
    Memory::Write32(at + 0x2C, 0);
    Memory::WriteFloat32(at + 0x30, pos.x);
    Memory::WriteFloat32(at + 0x34, pos.y);
    Memory::WriteFloat32(at + 0x38, pos.z);
    Memory::Write32(at + 0x3C, 0);
}

// Finds an asset's offset and size in a HIP file on the host (big-endian chunks: HIPA PACK DICT{ATOC{AINF AHDR..}}).
bool FindHipAsset(const std::string& path, uint32_t id, uint32_t& offset, uint32_t& size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
    uint8_t head[8];
    uint64_t pos = 0;
    auto readHead = [&](uint64_t at) {
        in.seekg(std::streamoff(at));
        return bool(in.read(reinterpret_cast<char*>(head), 8));
    };
    if (!readHead(0) || std::memcmp(head, "HIPA", 4) != 0) return false;
    pos = 8;
    while (readHead(pos)) {
        const uint32_t len = be32(head + 4);
        if (std::memcmp(head, "DICT", 4) == 0) {
            std::vector<uint8_t> dict(len);
            if (!in.read(reinterpret_cast<char*>(dict.data()), len)) return false;
            if (len < 8 || std::memcmp(dict.data(), "ATOC", 4) != 0) return false;
            const size_t atocEnd = std::min<size_t>(8 + be32(dict.data() + 4), len);
            size_t o = 8;
            while (o + 8 <= atocEnd) {
                const uint32_t clen = be32(dict.data() + o + 4);
                if (std::memcmp(dict.data() + o, "AHDR", 4) == 0 && o + 8 + 16 <= atocEnd && be32(dict.data() + o + 8) == id) {
                    offset = be32(dict.data() + o + 16);
                    size = be32(dict.data() + o + 20);
                    return true;
                }
                o += 8 + clen;
            }
            return false;
        }
        pos += 8 + len;
    }
    return false;
}

struct LoadedModel {
    uint32_t id = 0;
    uint32_t atomic = 0;
    bool owned = false;  // built here (destroyed here) rather than resident in the level
    uint32_t matrices = 0, matricesRaw = 0;
};

struct Session {
    uint32_t manager = 0, csn = 0, asset = 0;
    Cutscene cs;
    std::vector<uint8_t> data;
    std::vector<LoadedModel> models;
    // guest scratch: 64 quats, 64 vec3, 1 matrix, morph arguments (4 target pointers, 4 s16 weights)
    uint32_t scratchRaw = 0, quats = 0, trans = 0, mat = 0, morphArgs = 0;
    uint32_t morphRaw = 0, morphTargets = 0, morphBytes = 0;  // guest copy of the two targets being blended
    std::vector<std::pair<uint32_t, bool>> morphable;         // atomic -> whether the morph render may touch it
    bool playing = false;
    float time = 0;
    size_t chunkIndex = SIZE_MAX;
    std::optional<Chunk> chunk;
    std::vector<AnimationBlock> anims;
    std::vector<MorphBlock> morphs;
    std::optional<CameraBlock> camera;
    uint32_t sound = 0;
    bool loggedFirstFrame = false, loggedMorph = false;

    const LoadedModel* Model(uint32_t id) const {
        for (const auto& m : models) {
            if (m.id == id && m.atomic) return &m;
        }
        return nullptr;
    }

    bool SetChunk(size_t i) {
        if (i == chunkIndex) return chunk.has_value();
        chunkIndex = i;
        chunk = Chunk::Parse(data.data() + cs.chunkOffsets[i], cs.chunkOffsets[i + 1] - cs.chunkOffsets[i]);
        anims.clear();
        morphs.clear();
        camera.reset();
        if (!chunk) {
            RT_LOG(RT_TAG_RUNTIME) << "[cutscene] chunk " << i << " is damaged\n";
            return false;
        }
        anims = chunk->Animations();
        morphs = chunk->Morphs();
        camera = chunk->Camera();
        return true;
    }

    float End() const { return chunk && chunkIndex + 1 == cs.ChunkCount() ? chunk->end : 1e30f; }
};

std::unique_ptr<Session> g;

void Release(CpuContext* ctx, bool unloadModels) {
    if (!g) return;
    RouteToOwnHeap route;
    if (g->sound) Call(ctx, func_8003CDEC, {g->sound});
    for (auto& m : g->models) {
        if (m.owned && m.atomic && unloadModels) Call(ctx, func_8000E910, {m.atomic});
        if (m.matricesRaw) Call(ctx, func_8002FD70, {m.matricesRaw});
    }
    if (g->morphRaw) Call(ctx, func_8002FD70, {g->morphRaw});
    if (g->scratchRaw) Call(ctx, func_8002FD70, {g->scratchRaw});
    RT_LOG(RT_TAG_RUNTIME) << "[cutscene] released " << std::hex << g->csn << std::dec
                           << (unloadModels ? "" : " (scene gone: models left in place)") << "\n";
    g.reset();
}

// Emitter windows from the CSNM asset: start times at +0x14, end times at +0x50, emitter IDs at +0x8C (15 each).
void UpdateEmitters(CpuContext* ctx, bool allOff) {
    for (uint32_t i = 0; i < 15; ++i) {
        const uint32_t id = Memory::Read32(g->asset + 0x8C + 4 * i);
        if (!id) continue;
        const uint32_t e = Call(ctx, func_800C4A54, {id});
        if (!e || !Memory::Contains(e + 0x18)) continue;
        const float start = Memory::ReadFloat32(g->asset + 0x14 + 4 * i);
        const float end = Memory::ReadFloat32(g->asset + 0x50 + 4 * i);
        const uint8_t flags = Memory::Read8(e + 0x18);
        if (!allOff && g->time >= start && g->time <= end) {
            Memory::Write8(e + 0x18, flags | 1);
        } else if (allOff || g->time > end) {
            Memory::Write8(e + 0x18, flags & ~1);
        }
    }
}

void UpdateClip(CpuContext* ctx) {
    const uint32_t frame = uint32_t(std::max(0.0f, g->time * 30.0f));
    for (const auto& w : kClipWindows) {
        if (w.csn == g->csn && frame >= w.frame0 && frame <= w.frame1) {
            Call(ctx, func_800072F4, {}, {w.nearClip, w.farClip});
            return;
        }
    }
    Call(ctx, func_800072F4, {}, {0.0, 0.0});
}

bool Load(CpuContext* ctx, uint32_t manager) {
    const uint32_t asset = Memory::Read32(manager + 0x10);
    if (!asset || !Memory::Contains(asset, 0xC8)) return false;
    const uint32_t csn = Memory::Read32(asset + 8);
    const uint32_t hipName = Call(ctx, func_8003F920, {csn});
    if (!hipName || !Memory::Contains(hipName, 1)) return false;  // no CSN in the loaded levels: the movie plays
    std::string dvdPath(reinterpret_cast<const char*>(Memory::GetPointer(hipName, 1)),
                        strnlen(reinterpret_cast<const char*>(Memory::GetPointer(hipName, 1)), 256));
    std::string trimmed = dvdPath;
    for (auto& c : trimmed) if (c == '\\') c = '/';
    while (!trimmed.empty() && trimmed[0] == '/') trimmed.erase(0, 1);
    const char* host = DVDResolveHostPathForTest(trimmed.c_str());
    if (!host) host = DVDResolveHostPathForTest(dvdPath.c_str());
    if (!host) {
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] " << std::hex << csn << std::dec << ": can't find level file '" << dvdPath << "'\n";
        return false;
    }
    uint32_t offset = 0, size = 0;
    if (!FindHipAsset(host, csn, offset, size) || size < 0x50) return false;  // not injected: the movie plays

    auto s = std::make_unique<Session>();
    s->manager = manager;
    s->csn = csn;
    s->asset = asset;
    s->data.resize(size);
    {
        std::ifstream in(host, std::ios::binary);
        in.seekg(offset);
        if (!in.read(reinterpret_cast<char*>(s->data.data()), size)) return false;
    }
    auto cs = Cutscene::ParseHeader(s->data.data(), s->data.size());
    if (!cs || cs->id != csn || cs->ChunkCount() == 0 || cs->chunkOffsets.back() > size) {
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] " << std::hex << csn << std::dec << ": damaged CSN in " << host << "\n";
        return false;
    }
    s->cs = std::move(*cs);

    if (!EnsureOwnHeap(ctx)) {
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] no room for a cutscene heap: the movie plays\n";
        return false;
    }
    RouteToOwnHeap route;
    s->scratchRaw = GuestAlloc(ctx, kMaxBones * (16 + 12) + kMatrixBytes + 32 + 96);
    if (!s->scratchRaw) return false;
    s->quats = Align32(s->scratchRaw);
    s->trans = Align32(s->quats + kMaxBones * 16);
    s->mat = Align32(s->trans + kMaxBones * 12);
    s->morphArgs = Align32(s->mat + kMatrixBytes);
    g = std::move(s);  // from here Release() cleans up

    uint32_t built = 0, resident = 0, missing = 0;
    for (const auto& m : g->cs.models) {
        LoadedModel lm;
        lm.id = m.id;
        if (m.size == 0) {
            const uint32_t sizeOut = g->mat;  // scratch word for the size the lookup writes
            lm.atomic = Call(ctx, func_8003FF84, {m.id, sizeOut});
            resident += lm.atomic != 0;
        } else if (uint64_t(m.offset) + m.size <= g->data.size()) {
            const uint32_t raw = GuestAlloc(ctx, m.size);
            if (raw) {
                const uint32_t buf = Align32(raw);
                std::memcpy(Memory::GetPointer(buf, m.size), g->data.data() + m.offset, m.size);
                lm.atomic = Call(ctx, func_8000E978, {buf, m.size});
                lm.owned = lm.atomic != 0;
                Call(ctx, func_8002FD70, {raw});  // the loader has finished with the bytes
                built += lm.atomic != 0;
            }
        }
        if (lm.atomic) {
            lm.matricesRaw = GuestAlloc(ctx, (kMaxBones + 1) * kMatrixBytes);
            lm.matrices = lm.matricesRaw ? Align32(lm.matricesRaw) : 0;
            if (!lm.matrices) lm.atomic = 0;
        }
        missing += lm.atomic == 0;
        g->models.push_back(lm);
    }
    RT_LOG(RT_TAG_RUNTIME) << "[cutscene] " << std::hex << csn << std::dec << " from " << host << ": "
                           << g->cs.ChunkCount() << " chunks, models built " << built << ", resident " << resident
                           << ", missing " << missing << ", sound '" << g->cs.sound << "'\n";
    return true;
}

void Start(CpuContext* ctx) {
    g->playing = true;
    g->time = 0;
    g->SetChunk(0);
    Call(ctx, func_80048BE0, {kPlayer, kPlayerControlOff});
    Call(ctx, func_80048BE0, {kPlayer, kPlayerInvisible});
    Call(ctx, func_8003D318, {});
    uint32_t soundId = 0;
    if (g->chunk) {
        for (const auto& b : g->chunk->blocks) {
            if (b.type == kSound) { soundId = b.id; break; }
        }
    }
    if (soundId) {
        const uint32_t h = Call(ctx, func_8003D074, {soundId, 0x7F, 0, 0}, {1.0, 1.0, 0.0});
        g->sound = (h == 0 || h >= 0xFFFFFFFE) ? 0 : h;
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] sound " << std::hex << soundId << " -> handle " << h << std::dec << "\n";
    }
}

// The world matrix and bone matrices of one animated model at the session's time; false when it can't be posed.
bool Pose(CpuContext* ctx, size_t animIndex, const LoadedModel& m) {
    const AnimationBlock& a = g->anims[animIndex];
    const float t = g->cs.TrackTime(g->time, int32_t(a.index)) - g->chunk->start;
    std::vector<Quat> q;
    std::vector<Vec3> tr;
    a.skb.Eval(t, q, tr);
    const uint32_t bones = std::min(Call(ctx, func_8000E828, {m.atomic}), kMaxBones);
    if (bones > 0) {
        Vec3 pos = a.translate;
        for (uint32_t b = 0; b <= a.rootIndex && b < q.size(); ++b) {
            if (std::fabs(q[b].w) < 0.9999f) break;
            pos = {pos.x + tr[b].x, pos.y + tr[b].y, pos.z + tr[b].z};
            tr[b] = {};
        }
        const float identity[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        WriteMatrix(m.matrices, identity, pos);
        for (uint32_t b = 0; b < bones; ++b) {
            const Quat r = b < q.size() ? q[b] : Quat{};
            const Vec3 v = b < tr.size() ? tr[b] : Vec3{};
            Memory::WriteFloat32(g->quats + 16 * b, r.x);
            Memory::WriteFloat32(g->quats + 16 * b + 4, r.y);
            Memory::WriteFloat32(g->quats + 16 * b + 8, r.z);
            Memory::WriteFloat32(g->quats + 16 * b + 12, r.w);
            Memory::WriteFloat32(g->trans + 12 * b, v.x);
            Memory::WriteFloat32(g->trans + 12 * b + 4, v.y);
            Memory::WriteFloat32(g->trans + 12 * b + 8, v.z);
        }
        Call(ctx, func_8000E4C0, {m.atomic, g->quats, g->trans, m.matrices + kMatrixBytes});
        return true;
    }
    // A rigid prop: one rotation, the inverse of the key's (as the bone path and the camera use it).
    if (q.empty()) return false;
    Memory::WriteFloat32(g->quats, q[0].x);
    Memory::WriteFloat32(g->quats + 4, q[0].y);
    Memory::WriteFloat32(g->quats + 8, q[0].z);
    Memory::WriteFloat32(g->quats + 12, -q[0].w);
    Call(ctx, func_8002D3A0, {g->quats, m.matrices});
    Memory::Write32(m.matrices + 0x0C, 3);
    Memory::WriteFloat32(m.matrices + 0x30, tr[0].x + a.translate.x);
    Memory::WriteFloat32(m.matrices + 0x34, tr[0].y + a.translate.y);
    Memory::WriteFloat32(m.matrices + 0x38, tr[0].z + a.translate.z);
    return true;
}

// The game's morph render keeps its blend state in the geometry's first user-data array ("MORPHSTATE") when there is
// one, without checking its name; a geometry with some other user data must not go through it.
bool Morphable(uint32_t atomic, uint32_t geometry) {
    for (const auto& [a, ok] : g->morphable) {
        if (a == atomic) return ok;
    }
    bool ok = true;
    const uint32_t ext = geometry + Memory::Read32(kUserDataOffset);
    const uint32_t count = Memory::Contains(ext, 8) ? Memory::Read32(ext) : 1;
    if (count) {
        const uint32_t arrays = Memory::Read32(ext + 4);
        const uint32_t name = Memory::Contains(arrays, 16) ? Memory::Read32(arrays) : 0;
        const void* p = name && Memory::Contains(name, 11) ? Memory::GetPointer(name, 11) : nullptr;
        ok = p && std::memcmp(p, "MORPHSTATE", 11) == 0;
    }
    g->morphable.emplace_back(atomic, ok);
    return ok;
}

// Draws one atomic, through the morph render when one of the chunk's morph blocks deforms it (the block names this
// animation block and its vertex count matches the atomic's), as the PS2 did; false when no morph applied.
bool DrawMorphed(CpuContext* ctx, const AnimationBlock& a, uint32_t atomic, uint32_t matrices) {
    const uint32_t geometry = Memory::Read32(atomic + 0x18);
    if (!Memory::Contains(geometry, 0x18)) return false;
    const uint32_t vertices = Memory::Read32(geometry + 0x14);
    for (const auto& m : g->morphs) {
        if (m.anim != a.index || m.vertices != vertices) continue;
        if (!Morphable(atomic, geometry)) return false;
        uint32_t ka = 0, kb = 0;
        int32_t wb = 0;
        m.Blend(g->cs.TrackTime(g->time, int32_t(a.index)), ka, kb, wb);
        const uint32_t targetBytes = Align32(uint32_t(m.TargetValues()) * 2);
        if (targetBytes * 2 > g->morphBytes) {
            if (g->morphRaw) Call(ctx, func_8002FD70, {g->morphRaw});
            g->morphRaw = GuestAlloc(ctx, targetBytes * 2);
            g->morphTargets = g->morphRaw ? Align32(g->morphRaw) : 0;
            g->morphBytes = g->morphRaw ? targetBytes * 2 : 0;
            if (!g->morphRaw) return false;
        }
        const uint32_t key[2] = {ka, kb};
        for (int i = 0; i < 2; ++i) {
            const uint8_t* src = m.Target(key[i]);
            uint8_t* dst = static_cast<uint8_t*>(Memory::GetPointer(g->morphTargets + targetBytes * i, targetBytes));
            for (size_t v = 0; v < m.TargetValues(); ++v) {
                dst[2 * v] = src[2 * v + 1];
                dst[2 * v + 1] = src[2 * v];
            }
            Memory::Write32(g->morphArgs + 4 * i, g->morphTargets + targetBytes * i);
            Memory::Write32(g->morphArgs + 8 + 4 * i, 0);
        }
        Memory::Write16(g->morphArgs + 0x10, uint16_t(0x4000 - wb));
        Memory::Write16(g->morphArgs + 0x12, uint16_t(wb));
        Memory::Write16(g->morphArgs + 0x14, 0);
        Memory::Write16(g->morphArgs + 0x16, 0);
        Call(ctx, func_8000EBA4, {atomic, matrices, g->morphArgs, g->morphArgs + 0x10, 0}, {m.scale / 16384.0});
        return true;
    }
    return false;
}

void Render(CpuContext* ctx) {
    if (!g->chunk) return;
    RouteToOwnHeap route;  // first draws instance the geometry
    size_t drawn = 0, morphed = 0;
    for (size_t i = 0; i < g->anims.size(); ++i) {
        const AnimationBlock& a = g->anims[i];
        const LoadedModel* m = g->Model(a.modelId);
        if (!m || !Pose(ctx, i, *m)) continue;
        uint32_t atomic = m->atomic;
        for (uint32_t k = 0; atomic && k < 64; ++k) {
            if (g->cs.Visible(a.index, k, g->time)) {
                if (DrawMorphed(ctx, a, atomic, m->matrices)) {
                    ++morphed;
                } else {
                    Call(ctx, func_8000E39C, {atomic, m->matrices});
                }
                ++drawn;
            }
            atomic = Call(ctx, func_8000E89C, {atomic});
        }
    }
    if (!g->loggedFirstFrame || (morphed && !g->loggedMorph)) {
        g->loggedMorph |= morphed != 0;
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] " << (g->loggedFirstFrame ? "first morph" : "first frame") << " at "
                               << g->time << " s: " << g->anims.size() << " animated models, " << drawn
                               << " atomics drawn (" << morphed << " morphed), " << g->morphs.size()
                               << " morph blocks, camera " << (g->camera ? "yes" : "no") << "\n";
        g->loggedFirstFrame = true;
    }
}

void Camera(CpuContext* ctx) {
    if (!g->camera) return;
    const float t = g->cs.TrackTime(g->time, -1);
    std::vector<Quat> q;
    std::vector<Vec3> tr;
    g->camera->skb.Eval(t - g->chunk->start, q, tr);
    if (q.empty()) return;
    // The camera frame is the inverse of the key's rotation, turned half a turn about Y (applied on the left, so the
    // right and at rows flip). Checked against cin_sh_barrel: this aims the first shot at Shaggy and the 6 s shot at
    // the barrel, where the movie has them.
    float rows[3][3];
    QuatToRows({q[0].x, q[0].y, q[0].z, -q[0].w}, rows);
    for (int c = 0; c < 3; ++c) {
        rows[0][c] = -rows[0][c];
        rows[2][c] = -rows[2][c];
    }
    WriteMatrix(g->mat, rows, {tr[0].x + g->camera->translate.x, tr[0].y + g->camera->translate.y,
                               tr[0].z + g->camera->translate.z});
    static uint32_t logged = 0;
    if (g->time == 0 || (logged != g->chunkIndex + 1 && g->chunkIndex % 4 == 0)) {
        logged = uint32_t(g->chunkIndex + 1);
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] camera at " << g->time << " s: pos " << Memory::ReadFloat32(g->mat + 0x30) << " "
                               << Memory::ReadFloat32(g->mat + 0x34) << " " << Memory::ReadFloat32(g->mat + 0x38) << " at "
                               << rows[2][0] << " " << rows[2][1] << " " << rows[2][2] << "\n";
    }
    const uint32_t rwCamera = Memory::Read32(kGlobals);
    if (!rwCamera) return;
    Call(ctx, func_80007400, {rwCamera, g->mat});
    std::memcpy(Memory::GetPointer(kCameraMatrix, kMatrixBytes), Memory::GetPointer(g->mat, kMatrixBytes), kMatrixBytes);
    const float fov = g->camera->Fov(t);
    if (fov > 0) {
        Call(ctx, func_80007394, {rwCamera}, {fov});
        Memory::WriteFloat32(kCameraFov, fov);
    }
}

// The session belongs to the manager the game has active; anything else means the scene moved on.
bool Current() {
    if (!g) return false;
    return Memory::Read32(kCutsceneManager) == g->manager;
}

void EventImpl(CpuContext* ctx) {
    const uint32_t manager = ctx->gpr[4], event = ctx->gpr[5];
    if (event == kEventReset && g && g->manager == manager) Release(ctx, true);
    func_80046054(ctx);
    if (event == kEventStart && Memory::Read32(kCutsceneManager) == manager) {
        if (g && g->manager != manager) Release(ctx, false);
        if (!g && !Load(ctx, manager)) Release(ctx, true);
    }
}

void UpdateImpl(CpuContext* ctx) {
    const uint32_t manager = ctx->gpr[3];
    if (g && g->manager != manager) Release(ctx, false);
    if (!g) {
        func_80045F08(ctx);
        return;
    }
    const uint32_t state = Memory::Read32(manager + 0x20);
    if (!g->playing) {
        if (!(state & 1)) {
            func_80045F08(ctx);
            return;
        }
        Memory::Write32(manager + 0x20, state & ~1u);
        Call(ctx, func_80048B2C, {manager, manager, kEventPlay});  // unlocks it for the gallery; the movie is skipped
        Start(ctx);
        return;
    }
    g->time += Memory::ReadFloat32(kFrameTime);
    while (g->chunk && g->time > g->chunk->end && g->chunkIndex + 1 < g->cs.ChunkCount()) g->SetChunk(g->chunkIndex + 1);
    UpdateEmitters(ctx, false);
    UpdateClip(ctx);
    bool skip = false;
    if (Memory::Read32(kSkippable) && g->time > kSkipAfter) {
        const uint32_t pad = Memory::Read32(kPad0);
        skip = pad && Memory::Contains(pad + 0x30, 4) && (Memory::Read32(pad + 0x30) & kSkipButtons);
    }
    if (skip || !g->chunk || g->time >= g->End()) {
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] " << (skip ? "skipped" : "finished") << " at " << g->time << " s\n";
        UpdateEmitters(ctx, true);
        Release(ctx, true);
        func_80045F08(ctx);  // the "play" handler left the end flag set: the game's update ends the cutscene
    }
}

void MovieImpl(CpuContext* ctx) {
    if (g && ctx->lr == kMovieCallReturn && Current()) {
        RT_LOG(RT_TAG_RUNTIME) << "[cutscene] playing " << std::hex << g->csn << std::dec << " live instead of the movie\n";
        ctx->gpr[3] = 0;
        return;
    }
    func_800ABBA4(ctx);
}

std::atomic<uint32_t> g_startRequest{0};  // dev menu "csn NAME": a manager to send the start event to

void CameraImpl(CpuContext* ctx) {
    const bool ours = g && g->playing && ctx->gpr[3] == kGlobals && Current();
    func_8001E9DC(ctx);
    if (ours) Camera(ctx);
    if (const uint32_t manager = g_startRequest.exchange(0); manager && !g && !Memory::Read32(kCutsceneManager)) {
        Call(ctx, func_80048BE0, {manager, kEventStart});
    }
}

void RenderImpl(CpuContext* ctx) {
    if (g && g->playing && Current()) Render(ctx);
    func_8006CA9C(ctx);
}

// OS-heap allocations made during this file's guest calls come from the cutscene heap (the OS heap if that is full).
void HeapAllocImpl(CpuContext* ctx) {
    const uint32_t heap = ctx->gpr[3], size = ctx->gpr[4];
    if (g_routeDepth > 0 && g_ownHeap >= 0 && heap == Memory::Read32(kOsHeap)) {
        ctx->gpr[3] = uint32_t(g_ownHeap);
        func_80104724(ctx);
        if (ctx->gpr[3]) return;
        ctx->gpr[3] = heap;
        ctx->gpr[4] = size;
    }
    func_80104724(ctx);
}

// A block goes back to the heap it came from, whichever heap the caller names (RenderWare frees everything through the
// OS heap's handle, at any time: a resource purge in the game's own drawing can free a cutscene model's data).
void HeapFreeImpl(CpuContext* ctx) {
    if (g_ownHeap >= 0) {
        if (InOwnHeap(ctx->gpr[4])) {
            ctx->gpr[3] = uint32_t(g_ownHeap);
        } else if (ctx->gpr[3] == uint32_t(g_ownHeap)) {
            ctx->gpr[3] = Memory::Read32(kOsHeap);
        }
    }
    func_80104820(ctx);
}

}  // namespace No100fCutscenePlayer

extern "C" void No100f_CutsceneEvent(CpuContext* ctx) { No100fCutscenePlayer::EventImpl(ctx); }
extern "C" void No100f_CutsceneUpdate(CpuContext* ctx) { No100fCutscenePlayer::UpdateImpl(ctx); }
extern "C" void No100f_CutsceneMovie(CpuContext* ctx) { No100fCutscenePlayer::MovieImpl(ctx); }
extern "C" void No100f_CutsceneCamera(CpuContext* ctx) { No100fCutscenePlayer::CameraImpl(ctx); }
extern "C" void No100f_CutsceneRender(CpuContext* ctx) { No100fCutscenePlayer::RenderImpl(ctx); }
extern "C" void No100f_CutsceneHeapAlloc(CpuContext* ctx) { No100fCutscenePlayer::HeapAllocImpl(ctx); }
extern "C" void No100f_CutsceneHeapFree(CpuContext* ctx) { No100fCutscenePlayer::HeapFreeImpl(ctx); }
extern "C" void No100f_CutsceneRequestStart(uint32_t manager) { No100fCutscenePlayer::g_startRequest = manager; }

REGISTER_NATIVE_FUNCTION_AS(0x80046054, No100f_CutsceneEvent, "No100f_CutsceneEvent");
REGISTER_NATIVE_FUNCTION_AS(0x80045F08, No100f_CutsceneUpdate, "No100f_CutsceneUpdate");
REGISTER_NATIVE_FUNCTION_AS(0x800ABBA4, No100f_CutsceneMovie, "No100f_CutsceneMovie");
REGISTER_NATIVE_FUNCTION_AS(0x8001E9DC, No100f_CutsceneCamera, "No100f_CutsceneCamera");
REGISTER_NATIVE_FUNCTION_AS(0x8006CA9C, No100f_CutsceneRender, "No100f_CutsceneRender");
REGISTER_NATIVE_FUNCTION_AS(0x80104724, No100f_CutsceneHeapAlloc, "No100f_CutsceneHeapAlloc");
REGISTER_NATIVE_FUNCTION_AS(0x80104820, No100f_CutsceneHeapFree, "No100f_CutsceneHeapFree");

