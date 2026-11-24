// Tests for the CSN cutscene reader (hle/no100f_csn.h) on a cutscene built here, byte by byte. No game data.
// Optional: NO100F_CSN_DIR=<folder of .csn files cut from a player's own PS2 levels> also parses each of those and
// evaluates every block of every chunk (a local check; CI has no such folder).
#include "hle/no100f_csn.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace No100fCsn;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

namespace {

struct Writer {
    std::vector<uint8_t> b;
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void U16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
    void S16(int16_t v) { U16(uint16_t(v)); }
    void F(float f) { uint32_t u; std::memcpy(&u, &f, 4); U32(u); }
    void Pad(size_t a) { while (b.size() % a) b.push_back(0); }
    void Put32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
};

// One bone, 3 time steps (0, 0.5, 1 s), 3 keys: identity at 0, 90 degrees about Y at 0.5 and 1; x moves 0 -> 2 -> 4.
void WriteSkb(Writer& w) {
    w.U32(kSkbMagic);
    w.U32(0xCDCDCDCD);
    w.U16(1);
    w.U16(3);
    w.U32(3);
    w.F(0.01f); w.F(0.01f); w.F(0.01f);
    const int16_t s = int16_t(std::lround(std::sqrt(0.5) * 32767));
    const int16_t keys[3][8] = {{0, 0, 0, 0, 32767, 0, 0, 0}, {1, 0, s, 0, s, 200, 0, 0}, {2, 0, s, 0, s, 400, 0, 0}};
    for (const auto& k : keys) {
        w.U16(uint16_t(k[0]));
        for (int i = 1; i < 8; ++i) w.S16(k[i]);
    }
    w.F(0.0f); w.F(0.5f); w.F(1.0f);
    w.U16(0);  // step 0 -> keys 0, 1
    w.U16(1);  // step 1 -> keys 1, 2
}

std::vector<uint8_t> BuildCutscene() {
    Writer w;
    w.U32(kMagic);
    w.U32(0x12345678);  // ID
    w.U32(1);           // one model
    w.U32(1);           // one chunk
    w.U32(0x800); w.U32(0x400); w.U32(0);
    const size_t headerSizeAt = w.b.size();
    w.U32(0);           // header size, filled in below
    w.U32(1);           // one visibility entry
    w.U32(3);           // of 3 words
    w.U32(1);           // one break
    w.U32(0);
    const char sound[16] = "cin_test_m";
    for (char c : sound) w.b.push_back(uint8_t(c));
    for (int i = 0; i < 16; ++i) w.b.push_back(0);
    w.U32(1); w.U32(0xAABBCCDD); w.U32(0); w.U32(0);  // resident model
    const size_t chunkOffsAt = w.b.size();
    w.U32(0); w.U32(0);
    w.U16(0); w.U16(1); w.U16(0); w.U16(31); w.U32(0x0000FFFF);  // anim 0 atomic 1: frames 0-15 shown, 16-31 hidden
    w.F(0.5f); w.U32(uint32_t(-1));                                 // camera cut at 0.5 s
    w.Put32(headerSizeAt, uint32_t(w.b.size()));
    w.Pad(0x800);
    const size_t chunkAt = w.b.size();
    w.F(0.0f); w.F(1.0f); w.U32(2); w.U32(0);
    // animation block
    Writer a;
    a.U32(0); a.F(10.0f); a.F(0.0f); a.F(0.0f);
    WriteSkb(a);
    w.U32(kAnimation); w.U32(0xAABBCCDD); w.U32(uint32_t(a.b.size())); w.U32(0);
    w.b.insert(w.b.end(), a.b.begin(), a.b.end());
    w.Pad(16);
    // camera block with two FOV keys
    Writer c;
    c.U32(0); c.F(1.0f); c.F(2.0f); c.F(3.0f); c.U32(2); c.F(0.0f); c.F(40.0f); c.F(1.0f); c.F(60.0f);
    WriteSkb(c);
    w.U32(kCamera); w.U32(0); w.U32(uint32_t(c.b.size())); w.U32(0);
    w.b.insert(w.b.end(), c.b.begin(), c.b.end());
    w.Pad(16);
    w.Put32(chunkOffsAt, uint32_t(chunkAt));
    w.Put32(chunkOffsAt + 4, uint32_t(w.b.size()));
    return w.b;
}

void TestSynthetic() {
    const auto bytes = BuildCutscene();
    const auto cs = Cutscene::ParseHeader(bytes.data(), bytes.size());
    Require(cs.has_value(), "header parses");
    Require(cs->id == 0x12345678 && cs->sound == "cin_test_m", "id and sound");
    Require(cs->models.size() == 1 && cs->models[0].id == 0xAABBCCDD && cs->models[0].size == 0, "model table");
    Require(cs->ChunkCount() == 1, "chunk count");
    Require(cs->Visible(0, 1, 0.2f) && !cs->Visible(0, 1, 0.9f), "visibility bits");
    Require(cs->Visible(0, 0, 0.9f), "atomics without an entry are shown");
    Require(!cs->Visible(0, 1, 2.0f), "past the entry's frames is hidden");
    Require(Near(cs->TrackTime(0.48f, -1), 0.5f - kFrame) && Near(cs->TrackTime(0.48f, 0), 0.48f), "camera cut hold");
    Require(Near(cs->TrackTime(0.6f, -1), 0.6f), "after the cut");

    const size_t at = cs->chunkOffsets[0];
    const auto chunk = Chunk::Parse(bytes.data() + at, cs->chunkOffsets[1] - at);
    Require(chunk.has_value() && chunk->blocks.size() == 2, "chunk parses");
    const auto anims = chunk->Animations();
    Require(anims.size() == 1 && anims[0].modelId == 0xAABBCCDD && Near(anims[0].translate.x, 10.0f), "animation block");
    std::vector<Quat> q;
    std::vector<Vec3> t;
    anims[0].skb.Eval(0.25f, q, t);
    Require(q.size() == 1 && Near(t[0].x, 1.0f), "translation halfway through step 0");
    const float angle = 2.0f * std::acos(std::clamp(q[0].w, -1.0f, 1.0f));
    Require(Near(angle, 3.14159265f / 4.0f, 2e-3f), "rotation halfway between 0 and 90 degrees");
    anims[0].skb.Eval(0.75f, q, t);
    Require(Near(t[0].x, 3.0f) && Near(q[0].y, std::sqrt(0.5f), 2e-3f), "step 1");
    anims[0].skb.Eval(5.0f, q, t);
    Require(Near(t[0].x, 4.0f), "clamped past the end");
    anims[0].skb.Eval(-1.0f, q, t);
    Require(Near(t[0].x, 0.0f) && Near(q[0].w, 1.0f), "clamped before the start");

    const auto cam = chunk->Camera();
    Require(cam.has_value() && Near(cam->translate.z, 3.0f), "camera block");
    Require(Near(cam->Fov(0.5f), 50.0f) && Near(cam->Fov(2.0f), 60.0f) && Near(cam->Fov(-1.0f), 40.0f), "fov keys");

    // Damaged input is refused, never read past.
    auto cut = bytes;
    cut.resize(0x40);
    Require(!Cutscene::ParseHeader(cut.data(), cut.size()), "short header refused");
    Require(!Chunk::Parse(bytes.data() + at, 40), "short chunk refused");
    auto bad = bytes;
    bad[0] = 'X';
    Require(!Cutscene::ParseHeader(bad.data(), bad.size()), "bad magic refused");

    float m[3][3];
    QuatToRows({0, std::sqrt(0.5f), 0, std::sqrt(0.5f)}, m);  // 90 degrees about Y: right -> -Z... RenderWare rows
    Require(Near(m[0][2], -1.0f) && Near(m[2][0], 1.0f) && Near(m[1][1], 1.0f), "quaternion rows");
}

int TestFolder(const char* dir) {
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".csn") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto cs = Cutscene::ParseHeader(d.data(), d.size());
        Require(cs.has_value(), "real header parses");
        Require(cs->chunkOffsets.back() == d.size(), "last chunk offset is the asset size");
        std::vector<Quat> q;
        std::vector<Vec3> t;
        size_t anims = 0;
        for (size_t i = 0; i < cs->ChunkCount(); ++i) {
            const auto chunk = Chunk::Parse(d.data() + cs->chunkOffsets[i], cs->chunkOffsets[i + 1] - cs->chunkOffsets[i]);
            Require(chunk.has_value() && chunk->index == i, "real chunk parses");
            Require(chunk->Camera().has_value(), "every chunk has a camera");
            for (const auto& a : chunk->Animations()) {
                a.skb.Eval((chunk->end - chunk->start) * 0.5f, q, t);
                for (const auto& r : q) Require(Near(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w, 1.0f, 1e-3f), "unit rotation");
                ++anims;
            }
        }
        std::cout << e.path().filename().string() << ": " << cs->ChunkCount() << " chunks, " << anims << " animation blocks\n";
        ++files;
    }
    return files;
}

}  // namespace

int main() {
    try {
        TestSynthetic();
        if (const char* dir = std::getenv("NO100F_CSN_DIR")) {
            std::cout << "real cutscenes checked: " << TestFolder(dir) << "\n";
        }
        std::cout << "no100f_csn_tests: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "no100f_csn_tests: FAILED: " << e.what() << "\n";
        return 1;
    }
}
