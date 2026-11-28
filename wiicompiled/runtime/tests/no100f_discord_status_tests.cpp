#include "no100f_discord_status.h"

#include "no100f_devmenu_progress.h"

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
        Expect(Describe(InPlay("C005", kModePause)), "Misbehavin'? Cause a Cave In",
               "Paused | 30% complete | 1,240 Snacks", "paused");
        Expect(Describe(InPlay("G001", kModeSave)), "A Tight Spot for a Grave Plot!", "30% complete | 1,240 Snacks",
               "save menu");
        Expect(Describe(InPlay("H001")), "At the Mystery Machine", "30% complete | 1,240 Snacks", "hub");
        Expect(Describe(InPlay("H002")), "In the playground", "30% complete | 1,240 Snacks", "playground");
        Expect(Describe(InPlay("H003")), "In the Monster Gallery", "30% complete | 1,240 Snacks", "gallery");
        Expect(Describe(InPlay("S006")), "Watching the ending", "", "ending");
        Expect(Describe(InPlay("ZZZZ")), "", "", "unknown scene");

        // Boss rooms say who, the rest of the status stays.
        Expect(Describe(InPlay("O008")), "Facing the Black Knight", "30% complete | 1,240 Snacks", "black knight");
        Expect(Describe(InPlay("G009")), "Facing the Green Ghost", "30% complete | 1,240 Snacks", "green ghost");
        Expect(Describe(InPlay("W028")), "Facing Redbeard", "30% complete | 1,240 Snacks", "redbeard");
        Expect(Describe(InPlay("S005", kModePause)), "Facing the Mastermind", "Paused | 30% complete | 1,240 Snacks",
               "mastermind, paused");
        Expect(Describe(InPlay("O005")), "A Dark and Stormy Knight!", "30% complete | 1,240 Snacks",
               "the boss's level before the boss room");
        for (const Boss& boss : kBosses) {
            const Level* level = FindLevel(boss.scene);
            Require(level != nullptr, std::string(boss.text) + ": a level scene");
            uint32_t last = 0;
            for (uint32_t id : level->scenes)
                if (id != 0) last = id;
            Require(last == boss.scene, std::string(boss.text) + ": the level's last scene");
        }

        // Hover texts: the area on the big picture, the level's snacks and the tokens on the small one.
        Snapshot hover = InPlay("E002");
        Require(Describe(hover).largeText == "Hedge Maze", "area hover");
        Require(Describe(hover).smallText == kStudio, "no stats known: the studio");
        hover.tokensKnown = true;
        hover.tokens = 12;
        Require(Describe(hover).smallText == "12 of 21 Monster Tokens", "tokens only");
        hover.levelSnacksKnown = true;
        hover.levelSnacks = 45;
        hover.levelSnackTotal = 371;
        Require(Describe(hover).smallText == "45 of 371 Snacks in this level | 12 of 21 Monster Tokens", "both");
        Require(Describe(InPlay("H002")).largeText == "The Mystery Machine", "hub area");
        Require(Describe(InPlay("H001", kModeTitle)).largeText == kGameTitle, "menu: the game's title");
        Require(Describe(InPlay("S006")).largeText == kGameTitle, "ending: the game's title");
        Require(Describe(hover) != Describe(InPlay("E002")), "a hover change is a change");

        // Area pictures: every level's area and the hub have one; menus and the ending keep the icon.
        std::set<std::string> files;
        for (const AreaPicture& picture : kAreaPictures) files.insert(picture.file);
        Require(files.size() == sizeof(kAreaPictures) / sizeof(kAreaPictures[0]), "one file per area");
        for (const Level& level : kLevels)
            Require(AreaPictureFor(level.area) != kLargeImage, std::string(level.area) + ": has a picture");
        Require(Describe(InPlay("C003")).largeImage == "https://frightworks.dev/discord/no100f/cliffs-caves.jpg",
                "level picture URL");
        Require(Describe(InPlay("H003")).largeImage == "https://frightworks.dev/discord/no100f/hub.jpg", "hub picture");
        Require(Describe(InPlay("O008")).largeImage == "https://frightworks.dev/discord/no100f/tower.jpg",
                "boss room: its area's picture");
        Require(Describe(InPlay("H001", kModeTitle)).largeImage == kLargeImage, "menu: the icon");
        Require(Describe(InPlay("S006")).largeImage == kLargeImage, "ending: the icon");
        Status otherPicture = Describe(InPlay("E002"));
        otherPicture.largeImage = kLargeImage;
        Require(otherPicture != Describe(InPlay("E002")), "a picture change is a change");

        // SnackAreaIndex matches the game's snack-area table (no100f_devmenu_progress.h), scene by scene.
        for (uint32_t area = 0; area < no100f_devmenu::kSnackAreaScenes.size(); ++area)
            for (uint32_t id : no100f_devmenu::kSnackAreaScenes[area])
                if (id != 0) Require(SnackAreaIndex(id) == area, "snack area of scene " + std::to_string(id));
        Require(SnackAreaIndex(0) == 0 && SnackAreaIndex(SceneId("S006")) == 0, "no area for 0 or the ending");

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
