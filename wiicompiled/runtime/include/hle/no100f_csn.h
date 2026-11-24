// NO100F live cutscenes: reader and evaluator for the PS2 version's CSN cutscene assets.
//
// Our own code, written from the data format (documented below); no game code. Everything here works on host memory
// and has no runtime dependencies, so the CTest (tests/no100f_csn_tests.cpp) covers it without the game.
//
// A CSN is little-endian throughout:
//   header (0x50): 'CTSN', asset ID, model count, chunk count, three buffer sizes, header size, visibility count and
//                  size (words), break count, pad, sound names (16 + 16 chars)
//   model table:   {type 1, model ID, size, offset} per model; size 0 = a model already in the level (or boot.HIP)
//   chunk offsets: u32 [chunks + 1], absolute within the asset; the last one is the asset size
//   visibility:    {u16 anim, u16 atomic, u16 frame0, u16 frame1, u32 bits[]}: bit f = atomic shown at frame f (30 fps)
//   breaks:        {f32 time, s32 index}: a camera cut (index -1) or a cut on animation block n; the time used holds
//                  at break - 1/30 s just before it, so nothing blends across a cut
//   then the embedded models (RenderWare 3.1 clumps), then the time chunks, each about 1 s:
//                  {f32 start, f32 end, u32 blocks, u32 index} + blocks of {type, ID, size, 0} + payload (16-padded)
// Block types: 2 animation (ID = model), 3 camera, 4 morph (ID = animation block index), 5 sound (PS2 ADPCM; the
// GameCube disc has the same soundtrack as its own SNDS asset, so the player ignores these).
// Animation payloads hold an 'SKB1' keyframe set: per time step, per bone, an index into 16-byte keys
// {u16 time index, s16 quaternion x y z w (/32767), s16 translation x y z (* scale)}; key k and k+1 bracket the step.
// Morph payloads (faces and mouths): {u32 n, {f32 time from the cutscene start, u32 0}[n]}, padded to a multiple of
// 16 bytes, then 'MPH1' {u16 targets (= n), u16 vertices, u32 flags (bit 0: normals follow), f32 scale, 16 bytes},
// then n targets of absolute vertex positions, s16 x y z per vertex (* scale), each padded to a multiple of 8 values
// (twice that with normals). They apply to the atomic of the animated model whose vertex count matches.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace No100fCsn {

constexpr uint32_t kMagic = 0x4E535443;  // 'CTSN' read as a little-endian word
constexpr uint32_t kSkbMagic = 0x31424B53;  // 'SKB1'
constexpr uint32_t kMphMagic = 0x3148504D;  // 'MPH1'
constexpr float kFrame = 1.0f / 30.0f;

enum BlockType : uint32_t { kModel = 1, kAnimation = 2, kCamera = 3, kMorph = 4, kSound = 5 };

inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline int16_t Les16(const uint8_t* p) { return int16_t(Le16(p)); }
inline float Lef(const uint8_t* p) { uint32_t u = Le32(p); float f; std::memcpy(&f, &u, 4); return f; }

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Vec3 { float x = 0, y = 0, z = 0; };

struct Model { uint32_t id = 0, size = 0, offset = 0; };
struct Break { float time = 0; int32_t index = 0; };
struct Visibility { uint16_t anim = 0, atomic = 0, frame0 = 0, frame1 = 0; const uint8_t* bits = nullptr; };

// A view of one SKB1 keyframe set (no copy; valid while the bytes it points at are).
struct Skb {
    const uint8_t* keys = nullptr;
    const uint8_t* times = nullptr;
    const uint8_t* offsets = nullptr;
    uint16_t bones = 0, timeCount = 0;
    uint32_t keyCount = 0;
    Vec3 scale;

    static std::optional<Skb> Parse(const uint8_t* p, size_t size) {
        if (size < 0x1C || Le32(p) != kSkbMagic) return std::nullopt;
        Skb s;
        s.bones = Le16(p + 8);
        s.timeCount = Le16(p + 10);
        s.keyCount = Le32(p + 12);
        s.scale = {Lef(p + 16), Lef(p + 20), Lef(p + 24)};
        if (!s.bones || s.timeCount < 2 || s.keyCount < 2) return std::nullopt;
        const size_t need = 0x1C + size_t(s.keyCount) * 16 + size_t(s.timeCount) * 4 + size_t(s.timeCount - 1) * s.bones * 2;
        if (need > size) return std::nullopt;
        s.keys = p + 0x1C;
        s.times = s.keys + size_t(s.keyCount) * 16;
        s.offsets = s.times + size_t(s.timeCount) * 4;
        return s;
    }

    float Time(uint32_t i) const { return Lef(times + 4 * i); }

    // The pose of every bone at time t (seconds from the chunk's start), clamped to the set's time range.
    // Rotations blend linearly along the shorter arc and are renormalised (the keys are about 1/30 s apart).
    void Eval(float t, std::vector<Quat>& q, std::vector<Vec3>& tr) const {
        q.resize(bones);
        tr.resize(bones);
        uint32_t step = 0;
        if (t <= Time(0)) {
            t = Time(0);
        } else if (t >= Time(timeCount - 1)) {
            t = Time(timeCount - 1);
            step = timeCount - 2;
        } else {
            uint32_t lo = 0, hi = timeCount - 1;  // Time(lo) <= t < Time(hi)
            while (hi - lo > 1) {
                const uint32_t mid = (lo + hi) / 2;
                (Time(mid) <= t ? lo : hi) = mid;
            }
            step = lo;
        }
        for (uint32_t b = 0; b < bones; ++b) {
            uint32_t k = Le16(offsets + 2 * (size_t(step) * bones + b));
            if (k + 1 >= keyCount) k = keyCount - 2;
            const uint8_t* a = keys + 16 * size_t(k);
            const uint8_t* c = a + 16;
            const float ta = Time(std::min<uint32_t>(Le16(a), timeCount - 1));
            const float tc = Time(std::min<uint32_t>(Le16(c), timeCount - 1));
            float u = tc > ta ? (t - ta) / (tc - ta) : 0.0f;
            u = std::clamp(u, 0.0f, 1.0f);
            Quat qa{Les16(a + 2) / 32767.0f, Les16(a + 4) / 32767.0f, Les16(a + 6) / 32767.0f, Les16(a + 8) / 32767.0f};
            Quat qc{Les16(c + 2) / 32767.0f, Les16(c + 4) / 32767.0f, Les16(c + 6) / 32767.0f, Les16(c + 8) / 32767.0f};
            if (qa.x * qc.x + qa.y * qc.y + qa.z * qc.z + qa.w * qc.w < 0) qc = {-qc.x, -qc.y, -qc.z, -qc.w};
            Quat r{qa.x + (qc.x - qa.x) * u, qa.y + (qc.y - qa.y) * u, qa.z + (qc.z - qa.z) * u, qa.w + (qc.w - qa.w) * u};
            const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (n > 1e-6f) r = {r.x / n, r.y / n, r.z / n, r.w / n};
            q[b] = r;
            const Vec3 va{Les16(a + 10) * scale.x, Les16(a + 12) * scale.y, Les16(a + 14) * scale.z};
            const Vec3 vc{Les16(c + 10) * scale.x, Les16(c + 12) * scale.y, Les16(c + 14) * scale.z};
            tr[b] = {va.x + (vc.x - va.x) * u, va.y + (vc.y - va.y) * u, va.z + (vc.z - va.z) * u};
        }
    }
};

struct Block {
    uint32_t type = 0, id = 0, size = 0;
    const uint8_t* data = nullptr;
};

struct AnimationBlock {
    uint32_t index = 0;  // which animation block of the chunk (morph blocks and the visibility table count this way)
    uint32_t modelId = 0, rootIndex = 0;
    Vec3 translate;
    Skb skb;
};

struct CameraBlock {
    Vec3 translate;
    std::vector<std::pair<float, float>> fov;  // (time from the cutscene start, degrees)
    Skb skb;

    // Field of view at time t from the cutscene start; 0 when the block has no FOV keys.
    float Fov(float t) const {
        if (fov.empty()) return 0.0f;
        if (t <= fov.front().first) return fov.front().second;
        for (size_t i = 1; i < fov.size(); ++i) {
            if (t < fov[i].first) {
                const auto& a = fov[i - 1];
                const auto& b = fov[i];
                const float u = b.first > a.first ? (t - a.first) / (b.first - a.first) : 0.0f;
                return a.second + (b.second - a.second) * u;
            }
        }
        return fov.back().second;
    }
};

// A view of one morph block (no copy; valid while the bytes it points at are).
struct MorphBlock {
    uint32_t anim = 0;  // the animation block whose model it deforms
    uint32_t count = 0;
    uint16_t vertices = 0;
    float scale = 0;
    const uint8_t* times = nullptr;    // {f32 time, u32 0}[count]
    const uint8_t* targets = nullptr;  // count targets of TargetValues() s16 values
    size_t stride = 0;                 // s16 values from one target to the next

    static std::optional<MorphBlock> Parse(uint32_t anim, const uint8_t* p, size_t size) {
        if (size < 4) return std::nullopt;
        MorphBlock m;
        m.anim = anim;
        m.count = Le32(p);
        const size_t head = ((size_t(m.count) * 2 + 4) & ~size_t(3)) * 4;
        if (m.count == 0 || m.count > 65535 || head + 0x20 > size || Le32(p + head) != kMphMagic) return std::nullopt;
        const uint8_t* mph = p + head;
        m.vertices = Le16(mph + 6);
        m.scale = Lef(mph + 12);
        m.stride = (size_t(m.vertices) * 3 + 7) & ~size_t(7);
        if (Le32(mph + 8) & 1) m.stride *= 2;
        if (!m.vertices || head + 0x20 + size_t(m.count) * m.stride * 2 > size) return std::nullopt;
        m.times = p + 4;
        m.targets = mph + 0x20;
        return m;
    }

    float Time(uint32_t i) const { return Lef(times + 8 * i); }
    size_t TargetValues() const { return size_t(vertices) * 3; }
    const uint8_t* Target(uint32_t i) const { return targets + size_t(i) * stride * 2; }

    // The two targets to blend at time t (from the cutscene start) and the second one's weight in 1/16384ths, as the
    // PS2 picked them: the first key at or after t (searching from key 1, at most the last), blended from the key before.
    void Blend(float t, uint32_t& a, uint32_t& b, int32_t& weightB) const {
        if (count < 2) {
            a = b = 0;
            weightB = 0;
            return;
        }
        uint32_t i = 1;
        while (i < count - 1 && Time(i) < t) ++i;
        a = i - 1;
        b = i;
        const float t0 = Time(a), t1 = Time(b);
        const float w = t1 > t0 ? 16384.0f * (t - t0) / (t1 - t0) : 16384.0f;
        weightB = int32_t(std::clamp(w, 0.0f, 16384.0f));
    }
};

struct Chunk {
    float start = 0, end = 0;
    uint32_t index = 0;
    std::vector<Block> blocks;

    static std::optional<Chunk> Parse(const uint8_t* p, size_t size) {
        if (size < 16) return std::nullopt;
        Chunk c;
        c.start = Lef(p);
        c.end = Lef(p + 4);
        const uint32_t n = Le32(p + 8);
        c.index = Le32(p + 12);
        size_t o = 16;
        for (uint32_t i = 0; i < n; ++i) {
            if (o + 16 > size) return std::nullopt;
            Block b{Le32(p + o), Le32(p + o + 4), Le32(p + o + 8), p + o + 16};
            o += 16;
            if (b.size > size - o) return std::nullopt;
            o += (size_t(b.size) + 15) & ~size_t(15);
            if (b.type != 0 || b.size != 0) c.blocks.push_back(b);
        }
        return c;
    }

    std::vector<AnimationBlock> Animations() const {
        std::vector<AnimationBlock> out;
        uint32_t index = 0;
        for (const auto& b : blocks) {
            if (b.type != kAnimation) continue;
            const uint32_t i = index++;
            if (b.size < 16) continue;
            auto skb = Skb::Parse(b.data + 16, b.size - 16);
            if (!skb) continue;
            out.push_back({i, b.id, Le32(b.data), {Lef(b.data + 4), Lef(b.data + 8), Lef(b.data + 12)}, *skb});
        }
        return out;
    }

    std::vector<MorphBlock> Morphs() const {
        std::vector<MorphBlock> out;
        for (const auto& b : blocks) {
            if (b.type != kMorph) continue;
            if (auto m = MorphBlock::Parse(b.id, b.data, b.size)) out.push_back(*m);
        }
        return out;
    }

    std::optional<CameraBlock> Camera() const {
        for (const auto& b : blocks) {
            if (b.type != kCamera || b.size < 20) continue;
            CameraBlock cam;
            cam.translate = {Lef(b.data + 4), Lef(b.data + 8), Lef(b.data + 12)};
            size_t skbAt = 16;
            if (Le32(b.data + 16) != kSkbMagic) {
                const uint32_t n = Le32(b.data + 16);
                if (20 + size_t(n) * 8 > b.size) continue;
                for (uint32_t i = 0; i < n; ++i) cam.fov.emplace_back(Lef(b.data + 20 + 8 * i), Lef(b.data + 24 + 8 * i));
                skbAt = 20 + size_t(n) * 8;
            }
            auto skb = Skb::Parse(b.data + skbAt, b.size - skbAt);
            if (!skb) continue;
            cam.skb = *skb;
            return cam;
        }
        return std::nullopt;
    }
};

struct Cutscene {
    uint32_t id = 0, maxModel = 0, maxBufEven = 0, maxBufOdd = 0, headerSize = 0;
    std::string sound;
    std::vector<Model> models;
    std::vector<uint32_t> chunkOffsets;  // chunks + 1
    std::vector<Visibility> visibility;
    std::vector<Break> breaks;

    size_t ChunkCount() const { return chunkOffsets.empty() ? 0 : chunkOffsets.size() - 1; }

    // Parses the header part (the first headerSize bytes, which is also what a cutscene_toc entry holds).
    // `size` is how many bytes are available at p.
    static std::optional<Cutscene> ParseHeader(const uint8_t* p, size_t size) {
        if (size < 0x50 || Le32(p) != kMagic) return std::nullopt;
        Cutscene c;
        c.id = Le32(p + 4);
        const uint32_t numData = Le32(p + 8), numTime = Le32(p + 12);
        c.maxModel = Le32(p + 16);
        c.maxBufEven = Le32(p + 20);
        c.maxBufOdd = Le32(p + 24);
        c.headerSize = Le32(p + 28);
        const uint32_t visCount = Le32(p + 32), visWords = Le32(p + 36), breakCount = Le32(p + 40);
        c.sound.assign(reinterpret_cast<const char*>(p + 0x30), strnlen(reinterpret_cast<const char*>(p + 0x30), 16));
        if (c.headerSize > size || numData > 4096 || numTime > 65536) return std::nullopt;
        size_t o = 0x50;
        const size_t need = o + size_t(numData) * 16 + (size_t(numTime) + 1) * 4 + size_t(visWords) * 4 + size_t(breakCount) * 8;
        if (need > c.headerSize) return std::nullopt;
        for (uint32_t i = 0; i < numData; ++i, o += 16) c.models.push_back({Le32(p + o + 4), Le32(p + o + 8), Le32(p + o + 12)});
        for (uint32_t i = 0; i <= numTime; ++i, o += 4) c.chunkOffsets.push_back(Le32(p + o));
        const size_t visEnd = o + size_t(visWords) * 4;
        for (uint32_t i = 0; i < visCount; ++i) {
            if (o + 8 > visEnd) return std::nullopt;
            Visibility v{Le16(p + o), Le16(p + o + 2), Le16(p + o + 4), Le16(p + o + 6), p + o + 8};
            if (v.frame1 < v.frame0) return std::nullopt;
            o += 8 + (size_t((v.frame1 - v.frame0) >> 5) + 1) * 4;
            if (o > visEnd) return std::nullopt;
            c.visibility.push_back(v);
        }
        o = visEnd;
        for (uint32_t i = 0; i < breakCount; ++i, o += 8) c.breaks.push_back({Lef(p + o), int32_t(Le32(p + o + 4))});
        for (size_t i = 1; i < c.chunkOffsets.size(); ++i) {
            if (c.chunkOffsets[i] < c.chunkOffsets[i - 1]) return std::nullopt;
        }
        return c;
    }

    // Whether atomic `atomic` of the model animated by animation block `anim` shows at time t. Atomics with no entry
    // are always shown.
    bool Visible(uint32_t anim, uint32_t atomic, float t) const {
        const int frame = int(std::floor(t * 30.0f));
        for (const auto& v : visibility) {
            if (v.anim != anim || v.atomic != atomic) continue;
            if (frame < v.frame0 || frame > v.frame1) return false;
            const uint32_t f = uint32_t(frame - v.frame0);
            return (Le32(v.bits + 4 * (f >> 5)) >> (f & 31)) & 1;
        }
        return true;
    }

    // The time to sample for a track (index -1 = camera, n = animation block n): held at a cut's time - 1/30 s
    // during the last frame before that cut, so poses never blend across it.
    float TrackTime(float t, int32_t index) const {
        for (const auto& b : breaks) {
            if (b.index == index && t >= b.time - kFrame && t < b.time) return b.time - kFrame;
        }
        return t;
    }
};

// Rotation matrix (row vectors: right, up, at) of a unit quaternion, RenderWare style.
inline void QuatToRows(const Quat& q, float m[3][3]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0][0] = 1 - 2 * (yy + zz); m[0][1] = 2 * (xy + wz);     m[0][2] = 2 * (xz - wy);
    m[1][0] = 2 * (xy - wz);     m[1][1] = 1 - 2 * (xx + zz); m[1][2] = 2 * (yz + wx);
    m[2][0] = 2 * (xz + wy);     m[2][1] = 2 * (yz - wx);     m[2][2] = 1 - 2 * (xx + yy);
}

}  // namespace No100fCsn
