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
    s.percentKnown = true;
    s.percent = 30;
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

        Expect(Describe(InPlay("E004")), "On Edge in the Hedge!", "30% complete | 1,240 Snacks", "level in play");
        Expect(Describe(InPlay("I004")), "Mind Your Manors!", "30% complete | 1,240 Snacks", "i004");
        Expect(Describe(InPlay("C005", kModePause)), "Misbehavin'? Cause a Cave In", "30% complete | 1,240 Snacks",
               "paused: still only the completion and the snacks");
        Expect(Describe(InPlay("G001", kModeSave)), "A Tight Spot for a Grave Plot!", "30% complete | 1,240 Snacks",
               "save menu");
        Expect(Describe(InPlay("H001")), "At the Mystery Machine", "30% complete | 1,240 Snacks", "hub");
        Expect(Describe(InPlay("H002")), "In the playground", "30% complete | 1,240 Snacks", "playground");
        Expect(Describe(InPlay("H003")), "In the Monster Gallery", "30% complete | 1,240 Snacks", "gallery");
        Expect(Describe(InPlay("S006")), "Watching the ending", "", "ending");
        Expect(Describe(InPlay("ZZZZ")), "", "", "unknown scene");

        Snapshot noPercent = InPlay("F001");
        noPercent.percentKnown = false;  // a new game before its first save
        Expect(Describe(noPercent), "Shock on the Dock!", "1,240 Snacks", "completion not known yet");
        Snapshot noSnacks = InPlay("F001");
        noSnacks.snacksKnown = false;
        Expect(Describe(noSnacks), "Shock on the Dock!", "30% complete", "snacks unreadable");
        Snapshot full = InPlay("S004");
        full.percent = 100;
        full.snacks = 1;
        Expect(Describe(full), "Mastermind Unmasked!", "100% complete | 1 Snack", "100% and one snack");

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

        Require(SnackText(0) == "0 Snacks", "0");
        Require(SnackText(1) == "1 Snack", "1");
        Require(SnackText(999) == "999 Snacks", "999");
        Require(SnackText(1000) == "1,000 Snacks", "1000");
        Require(SnackText(99999) == "99,999 Snacks", "99999");
        Require(SnackText(1234567) == "1,234,567 Snacks", "1234567");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "no100f_discord_status_tests: ok\n";
    return 0;
}
