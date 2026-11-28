#pragma once

#include "no100f_progress.h"

#include <cstdint>
#include <string>

// Night of 100 Frights: what the Discord status says (Rich Presence), from the game state sampled on the game thread.
// Pure, so the wording is tested without the game (tests/no100f_discord_status_tests.cpp).
namespace no100f_discord_status {

// Art asset names uploaded to the FrightWorks - No100F Discord application (Rich Presence > Art Assets). Discord can't
// rename an asset: a new picture needs a new name here.
inline constexpr const char* kLargeImage = "frightworks-no100f-icon";
inline constexpr const char* kSmallImage = "frightworks-pfp";

// The name on top of the status card and in the short status ("Playing ..."), in place of the Discord app's own name
// (human's call, 2026-11-27; the launcher sends the same).
inline constexpr const char* kActivityName = "FrightWorks: Night of 100 Frights";

// Where clicking either picture goes, and the two buttons under the status (the launcher shows the same:
// launcher/DiscordPresence.cs).
inline constexpr const char* kGamePage = "https://frightworks.dev/games/no100f/";
inline constexpr const char* kButton1Label = "Get the Recomp";
inline constexpr const char* kButton1Url = kGamePage;
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
    const char* area;  // the game's own area name (the level table's third column)
};

// The level table in main.dol (tools/debug/game/levels.md, from tools/debug/game/levels.py), less level 1 (the hub
// scenes, worded on their own below). The levels are in the game's order: SnackAreaIndex counts on it.
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
    {{SceneId("R001"), SceneId("R020"), SceneId("R021"), SceneId("R003")}, "All Scares Upstairs!", "Attic & Balconies"},
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

// The hub scenes (level 1 in the game's table, "The Mystery Machine").
inline constexpr uint32_t kHubScenes[] = {SceneId("H001"), SceneId("H002"), SceneId("H003")};
inline constexpr const char* kHubArea = "The Mystery Machine";

// The level's index in the game's snack-area table (no100f_progress.h Progress::levels): 1 the hub, 2-29 kLevels in
// order, 0 not a level scene.
inline uint32_t SnackAreaIndex(uint32_t scene) {
    if (scene == 0) return 0;
    for (uint32_t id : kHubScenes)
        if (id == scene) return 1;
    if (const Level* level = FindLevel(scene)) return uint32_t(level - kLevels) + 2;
    return 0;
}

// Boss rooms, each its level's last scene (tools/debug/game/levels.md).
struct Boss {
    uint32_t scene;
    const char* text;
};
inline constexpr Boss kBosses[] = {
    {SceneId("G009"), "Facing the Green Ghost"},
    {SceneId("W028"), "Facing Redbeard"},
    {SceneId("O008"), "Facing the Black Knight"},
    {SceneId("S005"), "Facing the Mastermind"},
};

inline const char* BossText(uint32_t scene) {
    for (const Boss& boss : kBosses)
        if (boss.scene == scene) return boss.text;
    return nullptr;
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
    bool percentKnown = false;  // the save's completion, as the load screen shows it
    uint32_t percent = 0;
    bool tokensKnown = false;  // Monster Tokens found, of no100f_progress::kTokenTotal
    uint32_t tokens = 0;
    bool levelSnacksKnown = false;  // the snacks found in the scene's level, of the level's total
    uint32_t levelSnacks = 0, levelSnackTotal = 0;
};

inline constexpr const char* kGameTitle = "Night of 100 Frights";
inline constexpr const char* kStudio = "FrightWorks";

struct Status {
    std::string details;               // first line under the game's name
    std::string state;                 // second line
    std::string largeText = kGameTitle;  // hover text on the big picture: the area
    std::string smallText = kStudio;     // hover text on the small picture: tokens and the level's snacks
    bool operator==(const Status& other) const {
        return details == other.details && state == other.state && largeText == other.largeText &&
               smallText == other.smallText;
    }
    bool operator!=(const Status& other) const { return !(*this == other); }
};

// "1,240"
inline std::string Thousands(uint32_t value) {
    std::string digits = std::to_string(value);
    for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
    return digits;
}

// "1,240 Snacks"
inline std::string SnackText(uint32_t snacks) { return Thousands(snacks) + (snacks == 1 ? " Snack" : " Snacks"); }

// Empty details = nothing more to say than the game's name (boot, an unknown build, a mode not listed here).
inline Status Describe(const Snapshot& s) {
    if (!s.layoutVerified) return {};
    if (s.movie && s.credits) return {"Watching the credits", ""};
    if (s.mode == kModeTitle || s.mode == kModeLoad) return {"In the main menu", ""};
    if (s.mode != kModeInPlay && s.mode != kModePause && s.mode != kModeSave) return {};

    Status status;
    if (const Level* level = FindLevel(s.scene)) {
        status.details = level->name;
        status.largeText = level->area;
    } else if (s.scene == SceneId("H001")) {
        status.details = "At the Mystery Machine";
        status.largeText = kHubArea;
    } else if (s.scene == SceneId("H002")) {
        status.details = "In the playground";
        status.largeText = kHubArea;
    } else if (s.scene == SceneId("H003")) {
        status.details = "In the Monster Gallery";
        status.largeText = kHubArea;
    } else if (s.scene == SceneId("S006")) {
        return {"Watching the ending", ""};
    } else {
        return {};
    }
    if (const char* boss = BossText(s.scene)) status.details = boss;

    // The second line is the completion and the snacks held: "30% complete | 1,240 Snacks", "Paused | ..." in pause.
    std::string what = s.percentKnown ? std::to_string(s.percent) + "% complete" : std::string{};
    if (s.snacksKnown) what += (what.empty() ? "" : " | ") + SnackText(s.snacks);
    if (s.mode == kModePause) what = what.empty() ? "Paused" : "Paused | " + what;
    status.state = what;

    // Hover text on the small picture: "45 of 371 Snacks in this level | 12 of 21 Monster Tokens".
    std::string stats;
    if (s.levelSnacksKnown && s.levelSnackTotal > 0)
        stats = Thousands(s.levelSnacks) + " of " + Thousands(s.levelSnackTotal) + " Snacks in this level";
    if (s.tokensKnown)
        stats += (stats.empty() ? "" : " | ") + std::to_string(s.tokens) + " of " +
                 std::to_string(no100f_progress::kTokenTotal) + " Monster Tokens";
    if (!stats.empty()) status.smallText = stats;
    return status;
}

// Game thread, once per retrace (vi.cpp): samples the game about once a second and hands a changed status to
// DiscordPresence (src/no100f_discord_presence.cpp).
void SampleGame();

}  // namespace no100f_discord_status
