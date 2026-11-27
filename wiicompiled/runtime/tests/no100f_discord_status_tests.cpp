#include "no100f_discord_status.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace no100f_discord_status;

static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static void Expect(const Status& got, const std::string& details, const std::string& state, const char* what) {
    Require(got.details == details && got.state == state,
            std::string(what) + ": got \"" + got.details + "\" / \"" + got.state + "\"");
}

static Snapshot InPlay(const char (&scene)[5], uint32_t mode = kModeInPlay) {
    Snapshot s;
    s.layoutVerified = true;
    s.mode = mode;
    s.scene = SceneId(scene);
    s.snacksKnown = true;
    s.snacks = 1240;
    return s;
}

int main() {
    try {
        Require(SceneId("H001") == 0x48303031, "SceneId packs big-endian");

        // Every scene listed once; 28 levels (the table less the hub).
        Require(sizeof(kLevels) / sizeof(kLevels[0]) == 28, "28 levels");
        std::set<uint32_t> seen;
        for (const Level& level : kLevels) {
            Require(level.scenes[0] != 0, std::string(level.name) + " has scenes");
            for (uint32_t id : level.scenes) {
                if (id == 0) continue;
                Require(seen.insert(id).second, std::string("scene listed twice in ") + level.name);
                Require(FindLevel(id) == &level, std::string("FindLevel finds ") + level.name);
            }
        }
        Require(FindLevel(0) == nullptr && FindLevel(SceneId("H001")) == nullptr, "no level for 0 or the hub");

        Expect(Describe(InPlay("E004")), "On Edge in the Hedge!", "Hedge Maze, Part 1 | 1,240 Scooby Snacks",
               "level in play");
        Expect(Describe(InPlay("I001")), "Clamor in the Manor!", "1st Floor, Part 1 | 1,240 Scooby Snacks", "i001");
        Expect(Describe(InPlay("I004")), "Mind Your Manors!", "1st Floor, Part 2 | 1,240 Scooby Snacks", "i004");
        Expect(Describe(InPlay("E008")), "Chills & Spills on Haunted Hill!", "Hedge Maze, Part 3 | 1,240 Scooby Snacks",
               "third level of an area");
        Expect(Describe(InPlay("B002")), "Who's Yella' in the Cellar?", "Cellars | 1,240 Scooby Snacks",
               "an area with one level has no part");
        Expect(Describe(InPlay("C005", kModePause)), "Misbehavin'? Cause a Cave In", "Cliffs & Caves, Part 2 (paused)",
               "paused");
        Expect(Describe(InPlay("G001", kModeSave)), "A Tight Spot for a Grave Plot!", "Saving the game", "save menu");
        Expect(Describe(InPlay("H001")), "At the Mystery Machine", "1,240 Scooby Snacks", "hub");
        Expect(Describe(InPlay("H002")), "In the playground", "1,240 Scooby Snacks", "playground");
        Expect(Describe(InPlay("H002", kModePause)), "In the playground", "Paused", "playground paused");
        Expect(Describe(InPlay("H003")), "In the Monster Gallery", "1,240 Scooby Snacks", "gallery");
        Expect(Describe(InPlay("S006")), "Watching the ending", "", "ending");
        Expect(Describe(InPlay("ZZZZ")), "", "", "unknown scene");

        Snapshot noSnacks = InPlay("F001");
        noSnacks.snacksKnown = false;
        Expect(Describe(noSnacks), "Shock on the Dock!", "Fishing Village, Part 1", "snacks unreadable");

        Snapshot menu = InPlay("H001", kModeTitle);
        Expect(Describe(menu), "In the main menu", "", "title");
        menu.state = 1;  // attract demo: still the menu
        Expect(Describe(menu), "In the main menu", "", "attract demo");
        menu.mode = kModeLoad;
        Expect(Describe(menu), "In the main menu", "", "load game");

        Snapshot credits = InPlay("H001", kModeTitle);
        credits.movie = true;
        credits.credits = true;
        Expect(Describe(credits), "Watching the credits", "", "credits");
        credits.credits = false;  // a trailer from the menu
        Expect(Describe(credits), "In the main menu", "", "trailer");

        Snapshot boot = InPlay("H001", 0);
        Expect(Describe(boot), "", "", "boot");
        Snapshot unknownBuild = InPlay("E001");
        unknownBuild.layoutVerified = false;
        Expect(Describe(unknownBuild), "", "", "unverified layout");

        Require(SnackText(0) == "0 Scooby Snacks", "0");
        Require(SnackText(1) == "1 Scooby Snack", "1");
        Require(SnackText(999) == "999 Scooby Snacks", "999");
        Require(SnackText(1000) == "1,000 Scooby Snacks", "1000");
        Require(SnackText(99999) == "99,999 Scooby Snacks", "99999");
        Require(SnackText(1234567) == "1,234,567 Scooby Snacks", "1234567");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "no100f_discord_status_tests: ok\n";
    return 0;
}
