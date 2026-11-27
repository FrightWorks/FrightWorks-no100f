#include "no100f_progress.h"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace no100f_progress;

static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static uint32_t Percent(uint32_t flags, uint32_t states, uint32_t snacks, uint32_t tokens, bool finished = false) {
    Inputs in;
    in.progressFlags = flags;
    in.savedStates = states;
    in.snacksCollected = snacks;
    in.tokens = tokens;
    in.finished = finished;
    return Completion(in);
}

// Guest memory as a word map; anything not set reads as missing.
struct FakeMemory {
    std::map<uint32_t, uint32_t> words;
    bool operator()(uint32_t address, uint32_t& value) const {
        const auto it = words.find(address);
        if (it == words.end()) return false;
        value = it->second;
        return true;
    }
};

static FakeMemory Game() {
    FakeMemory m;
    m.words[no100f_devmenu::kPlayerSnacks] = 1240;
    m.words[kProgressFlags] = 0xFFFF8007;  // 3 of the 15 counted bits; the high ones are not part of it
    m.words[kTokenFlags] = 0xFFE0001F;     // 5 tokens; bits 21 and up are not tokens
    m.words[kSnacksCollected] = 2000;
    m.words[kFinished] = 0;
    m.words[kBitMasks] = 0x80000000;  // the first state bit is the top one
    const uint32_t records = 0x80500000;
    m.words[kSavedStates + 0x14] = records;
    m.words[kSavedStates + 0x18] = 4;
    // Record: tag, bits pointer, (unused), byte size.
    const auto record = [&](uint32_t index, uint32_t tag, uint32_t firstWord, uint32_t bytes) {
        const uint32_t at = 0x80510000 + index * 0x20, bits = 0x80520000 + index * 0x20;
        m.words[records + index * 4] = at;
        m.words[at] = tag;
        m.words[at + 4] = bits;
        m.words[at + 0xC] = bytes;
        m.words[bits] = firstWord;
    };
    record(0, 0x11111111, 0x80000000, 4);  // first bit set: counts
    record(1, kPlayerTag, 0xFFFFFFFF, 64);  // the player: never counts
    record(2, 0x22222222, 0x7FFFFFFF, 4);  // other bits only: doesn't
    record(3, 0x33333333, 0x80000000, 0);  // no state bits: doesn't
    return m;
}

int main() {
    try {
        // The game's weights: 43 flags, 20 saved states, 20 snacks, 15 tokens, 2 when finished: everything is 100.
        Require(Percent(0, 0, 0, 0) == 0, "nothing: 0%");
        Require(Percent(13, 86, 6748, 21, true) == 100, "everything: 100%");
        Require(Percent(13, 86, 6748, 21, false) == 98, "everything but finished: 98%");
        Require(Percent(15, 90, 6748, 21, true) == 100, "capped at 100%");
        Require(Percent(13, 0, 0, 0) == 43, "all flags: 43%");
        Require(Percent(0, 86, 0, 0) == 20, "all saved states: 20%");
        Require(Percent(0, 0, 6748, 0) == 20, "all snacks: 20%");
        Require(Percent(0, 0, 0, 21) == 15, "all tokens: 15%");
        Require(Percent(1, 0, 0, 0) == 3, "one flag: 43/13 = 3.3, rounded down");
        Require(Percent(0, 0, 1, 0) == 1, "one snack counts as 1%");
        Require(Percent(0, 0, 0, 1) == 1, "one token: 15/21 = 0.7, counts as 1%");
        Require(Percent(0, 1, 0, 0) == 0, "one saved state alone: 0.2, stays 0% (not in the at-least-1 rule)");
        Require(Percent(0, 0, 0, 0, true) == 2, "finished alone: 2%");
        Require(Percent(6, 40, 3000, 10) == 45, "a mid game: 19.8 + 9.3 + 8.9 + 7.1 = 45.2");

        Require(CountBits(0) == 0 && CountBits(0x7FFF) == 15 && CountBits(0xFFFFFFFF) == 32, "CountBits");

        FakeMemory memory = Game();
        uint32_t states = 99;
        Require(ReadSavedStates(memory, states) && states == 1, "saved states: one first bit set, PLYR skipped");

        Progress progress;
        Require(ReadProgress(memory, progress) && progress.valid, "reads");
        Require(progress.snacks == 1240, "snacks held");
        Require(progress.inputs.progressFlags == 3 && progress.inputs.tokens == 5 &&
                    progress.inputs.snacksCollected == 2000 && progress.inputs.savedStates == 1 &&
                    !progress.inputs.finished,
                "inputs");
        Require(progress.percent == Percent(3, 1, 2000, 5), "percent from the inputs");
        Require(!progress.levelsKnown, "no snack-area table in the fake: levels unknown, progress still valid");

        FakeMemory tooMany = Game();
        tooMany.words[kSavedStates + 0x18] = kSavedStateLimit + 1;
        Require(!ReadProgress(tooMany, progress) && !progress.valid, "a table too big to be the game's: not valid");
        FakeMemory noMask = Game();
        noMask.words[kBitMasks] = 0;  // before the game fills its mask table
        Require(!ReadProgress(noMask, progress), "mask table not filled yet: not valid");
        FakeMemory badRecord = Game();
        badRecord.words[0x80500000 + 4] = 0x00001000;  // a record pointer outside guest memory
        Require(!ReadProgress(badRecord, progress), "a bad record pointer: not valid");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "no100f_progress_tests: ok\n";
    return 0;
}
