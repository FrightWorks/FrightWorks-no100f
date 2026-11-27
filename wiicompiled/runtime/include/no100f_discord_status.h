#pragma once

#include <cstdint>
#include <string>

// Night of 100 Frights: what the Discord status says (Rich Presence), from the game state sampled on the game thread.
// Pure, so the wording is tested without the game (tests/no100f_discord_status_tests.cpp).
namespace no100f_discord_status {

// Art asset names uploaded to the FrightWorks - No100F Discord application (Rich Presence > Art Assets). Discord can't
// rename an asset: a new picture needs a new name here.
inline constexpr const char* kLargeImage = "frightworks-no100f-icon";
inline constexpr const char* kSmallImage = "frightworks-pfp";

// The two buttons under the status (the launcher shows the same: launcher/DiscordPresence.cs).
inline constexpr const char* kButton1Label = "Get the Recomp";
inline constexpr const char* kButton1Url = "https://frightworks.dev/games/no100f/";
inline constexpr const char* kButton2Label = "Get it on RecompLauncher";
inline constexpr const char* kButton2Url = "https://recomplauncher.com/game/frightworks-no100f";

// Scene ID as the game stores it at [[0x80245CF4] + 0x6C]: four characters, big-endian ('H001' = 0x48303031).
constexpr uint32_t SceneId(const char (&name)[5]) {
    return uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 | uint32_t(uint8_t(name[2])) << 8 |
           uint32_t(uint8_t(name[3]));
}

struct Level {
    uint32_t scenes[4];
    const char* name;
    const char* area;  // the game's grouping, only to number the parts (not shown)
};

// The level table in main.dol (tools/debug/game/levels.md, from tools/debug/game/levels.py), less level 1 (the hub
// scenes, worded on their own below).
inline constexpr Level kLevels[] = {
    {{SceneId("E001"), SceneId("E002"), SceneId("E003"), SceneId("E004")}, "On Edge in the Hedge!", "Hedge Maze"},
    {{SceneId("E005"), SceneId("E006")}, "It's a Mean Greenhouse, Scooby!", "Hedge Maze"},
    {{SceneId("E007"), SceneId("E008"), SceneId("E009")}, "Chills & Spills on Haunted Hill!", "Hedge Maze"},
    {{SceneId("C001"), SceneId("C002"), SceneId("C003"), SceneId("C004")}, "Scared Stiff at Skull Cliff!",
     "Cliffs & Caves"},
    {{SceneId("C005"), SceneId("C006"), SceneId("C007")}, "Misbehavin'? Cause a Cave In", "Cliffs & Caves"},
    {{SceneId("G001"), SceneId("G002")}, "A Tight Spot for a Grave Plot!", "Cemetery"},
    {{SceneId("G003"), SceneId("G004"), SceneId("G005"), SceneId("G006")}, "A One Way Trip to the Crypt!", "Cemetery"},
    {{SceneId("G007"), SceneId("G008"), SceneId("G009")}, "Gloom & Doom Down in the Tomb!", "Cemetery"},
    {{SceneId("F001"), SceneId("F003")}, "Shock on the Dock!", "Fishing Village"},
    {{SceneId("F004"), SceneId("F005"), SceneId("F006"), SceneId("F007")}, "Fishy Clues for Scooby-Doo?",
     "Fishing Village"},
    {{SceneId("F008"), SceneId("F009"), SceneId("F010")}, "Fear on the Pier!", "Fishing Village"},
    {{SceneId("L011"), SceneId("L013"), SceneId("L014")}, "Coast for Some Ghosts!", "Lighthouse"},
    {{SceneId("L015")}, "Going Down Witch Way?", "Lighthouse"},
    {{SceneId("L017"), SceneId("L018"), SceneId("L019")}, "Lighthouse Fright House!", "Lighthouse"},
    {{SceneId("W020"), SceneId("W021"), SceneId("W022")}, "Wreck on the Deck!", "Shipwreck"},
    {{SceneId("W023"), SceneId("W025"), SceneId("W026")}, "Aghast by the Mast!", "Shipwreck"},
    {{SceneId("W027"), SceneId("W028")}, "Shiver Your Timbers, Scooby!", "Shipwreck"},
    {{SceneId("I001"), SceneId("I020"), SceneId("I021"), SceneId("I003")}, "Clamor in the Manor!", "1st Floor"},
    {{SceneId("I004"), SceneId("I005"), SceneId("I006")}, "Mind Your Manors!", "1st Floor"},
    {{SceneId("R001"), SceneId("R020"), SceneId("R021"), SceneId("R003")}, "All Scares Upstairs!",
     "Attic & Balconies"},
    {{SceneId("R004"), SceneId("R005"), SceneId("R006")}, "Don't Look Down, Scooby-Doo!", "Attic & Balconies"},
    {{SceneId("O001"), SceneId("O002"), SceneId("O003"), SceneId("O004")}, "Panic in the Attic!", "Tower"},
    {{SceneId("O005"), SceneId("O006"), SceneId("O008")}, "A Dark and Stormy Knight!", "Tower"},
    {{SceneId("B001"), SceneId("B002"), SceneId("B003"), SceneId("B004")}, "Who's Yella' in the Cellar?", "Cellars"},
    {{SceneId("P001"), SceneId("P002"), SceneId("P003")}, "Creepy Crawlies in the Hallways", "Secret Passages"},
    {{SceneId("P004"), SceneId("P005")}, "Gusts Abound Underground", "Secret Passages"},
    {{SceneId("S001"), SceneId("S002"), SceneId("S003")}, "Little Lab of Horrors", "Super Secret Lab"},
    {{SceneId("S004"), SceneId("S005")}, "Mastermind Unmasked!", "Super Secret Lab"},
};

inline const Level* FindLevel(uint32_t scene) {
    if (scene == 0) return nullptr;
    for (const Level& level : kLevels)
        for (uint32_t id : level.scenes)
            if (id == scene) return &level;
    return nullptr;
}

// "Part 2": the level's place among its area's levels (the table's order is the game's). Empty for an area with one
// level (Cellars).
inline std::string AreaPart(const Level& level) {
    int part = 0, parts = 0;
    for (const Level& other : kLevels) {
        if (std::string(other.area) != level.area) continue;
        ++parts;
        if (&other == &level) part = parts;
    }
    return parts > 1 ? "Part " + std::to_string(part) : std::string{};
}

// Game modes (no100f_game_state.h): 2 title screen and its menus, 4 load game, 6 the Mystery Machine (save) menu,
// 7 pause, 11 in play.
inline constexpr uint32_t kModeTitle = 2, kModeLoad = 4, kModeSave = 6, kModePause = 7, kModeInPlay = 11;

struct Snapshot {
    bool layoutVerified = false;  // a known main.dol: the addresses below mean what this file says
    uint32_t mode = 0;
    uint32_t state = 0;
    bool movie = false;
    bool credits = false;  // the open movie is a credits roll
    uint32_t scene = 0;
    bool snacksKnown = false;
    uint32_t snacks = 0;
};

struct Status {
    std::string details;  // first line under the game's name
    std::string state;    // second line
    bool operator==(const Status& other) const { return details == other.details && state == other.state; }
    bool operator!=(const Status& other) const { return !(*this == other); }
};

// "1,240 Scooby Snacks"
inline std::string SnackText(uint32_t snacks) {
    std::string digits = std::to_string(snacks);
    for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
    return digits + (snacks == 1 ? " Scooby Snack" : " Scooby Snacks");
}

// Empty details = nothing more to say than the game's name (boot, an unknown build, a mode not listed here).
inline Status Describe(const Snapshot& s) {
    if (!s.layoutVerified) return {};
    if (s.movie && s.credits) return {"Watching the credits", ""};
    if (s.mode == kModeTitle || s.mode == kModeLoad) return {"In the main menu", ""};
    if (s.mode != kModeInPlay && s.mode != kModePause && s.mode != kModeSave) return {};

    std::string where;
    std::string what;
    if (const Level* level = FindLevel(s.scene)) {
        where = level->name;
        what = AreaPart(*level);
    } else if (s.scene == SceneId("H001")) {
        where = "At the Mystery Machine";
    } else if (s.scene == SceneId("H002")) {
        where = "In the playground";
    } else if (s.scene == SceneId("H003")) {
        where = "In the Monster Gallery";
    } else if (s.scene == SceneId("S006")) {
        return {"Watching the ending", ""};
    } else {
        return {};
    }
    if (s.mode == kModePause) {
        what = what.empty() ? "Paused" : what + " (paused)";
    } else if (s.mode == kModeSave) {
        what = "Saving the game";
    } else if (s.snacksKnown) {
        const std::string snacks = SnackText(s.snacks);
        what = what.empty() ? snacks : what + " | " + snacks;
    }
    return {where, what};
}

// Game thread, once per retrace (vi.cpp): samples the game about once a second and hands a changed status to
// DiscordPresence (src/no100f_discord_presence.cpp).
void SampleGame();

}  // namespace no100f_discord_status
