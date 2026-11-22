#include "hle/no100f_stream_read.h"

#include <iostream>
#include <stdexcept>
#include <unordered_map>

using namespace No100fStreamRead;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace {
constexpr uint32_t kEntry = 0x8024C258u;
constexpr uint32_t kChunkBytes = 0x2EE0;

struct Guest {
    std::unordered_map<uint32_t, uint32_t> mem;
    Guest() { mem[kEntry] = 0x8026C098u; }
    uint32_t& At(uint32_t field) { return mem[kEntry + field]; }
    // A chunk arrives: the wrapper's clear, then what the game's callback does to bytes read (fn_800DA71C).
    bool Read(uint32_t bytes = kChunkBytes) {
        const bool cleared = ClearPrevReadOnFirstChunk(kEntry, [this](uint32_t a) { return mem[a]; },
                                                       [this](uint32_t a, uint32_t v) { mem[a] = v; });
        if (mem[kEntry] != 0) At(kBytesRead) += bytes;
        return cleared;
    }
    // The end check at a crossing into half 1 (fn_800D9EF8, 800DA058): returns true when the track looks finished.
    bool CrossHalf1() {
        if (At(kBytesRead) == At(kPrevRead)) return true;
        At(kPrevRead) = At(kBytesRead);
        return false;
    }
};
}  // namespace

int main() {
    try {
        // Issue 14: the previous stream stopped after one crossing, leaving 0xBB80. The next stream must not look finished.
        {
            Guest g;
            g.At(kPrevRead) = 0xBB80;
            Require(g.Read(), "the first chunk clears the previous read");
            for (int i = 0; i < 3; ++i) g.Read();
            Require(g.At(kBytesRead) == 0xBB80, "four chunks read");
            Require(!g.CrossHalf1(), "a new stream's first crossing is not the end of the track");
            Require(g.At(kPrevRead) == 0xBB80, "the crossing stores bytes read");
        }
        // Only the first chunk clears it, and a real end still compares equal.
        {
            Guest g;
            g.Read();
            g.At(kPrevRead) = 0x1234;
            Require(!g.Read(), "later chunks don't clear");
            Require(g.At(kPrevRead) == 0x1234, "later chunks leave the previous read alone");
            g.At(kPrevRead) = 0;
            Require(!g.CrossHalf1(), "still reading");
            Require(g.CrossHalf1(), "no read since the last crossing is the end of the track");
        }
        // A freed voice changes nothing.
        {
            Guest g;
            g.At(0) = 0;
            g.At(kPrevRead) = 0xBB80;
            Require(!g.Read(), "no clear");
            Require(g.At(kPrevRead) == 0xBB80 && g.At(kBytesRead) == 0, "no state change");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "no100f_stream_read_tests passed\n";
    return 0;
}
