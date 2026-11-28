#include "discord_activity_json.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace DiscordPresence;

static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static void Expect(const std::string& got, const std::string& want, const char* what) {
    Require(got == want, std::string(what) + ":\n  got  " + got + "\n  want " + want);
}

static const std::string kHead = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"wiicompiled\",\"args\":{\"pid\":42,\"activity\":{";
static const std::string kTail = "\"instance\":false}}}";

int main() {
    try {
        Expect(ActivityJson::Build({}, 42), kHead + kTail, "empty activity");

        // The fields FrightWorks has always sent, in the same order as before.
        Activity basic;
        basic.details = "On Edge in the Hedge!";
        basic.state = "30% complete | 1,240 Snacks";
        basic.largeImageKey = "frightworks-no100f-icon";
        basic.largeImageText = "Hedge Maze";
        basic.smallImageKey = "frightworks-pfp";
        basic.smallImageText = "FrightWorks";
        basic.startTimestamp = 1795823920;
        basic.buttons = {{{"Get the Recomp", "https://frightworks.dev/games/no100f/"}, {"Two", "https://b"}}};
        Expect(ActivityJson::Build(basic, 42),
               kHead +
                   "\"details\":\"On Edge in the Hedge!\",\"state\":\"30% complete | 1,240 Snacks\","
                   "\"assets\":{\"large_image\":\"frightworks-no100f-icon\",\"large_text\":\"Hedge Maze\","
                   "\"small_image\":\"frightworks-pfp\",\"small_text\":\"FrightWorks\"},"
                   "\"timestamps\":{\"start\":1795823920},"
                   "\"buttons\":[{\"label\":\"Get the Recomp\",\"url\":\"https://frightworks.dev/games/no100f/\"},"
                   "{\"label\":\"Two\",\"url\":\"https://b\"}]," +
                   kTail,
               "basic activity");

        // The newer fields (each one echoed back by a running Discord, 2026-11-27).
        Activity full = basic;
        full.name = "Night of 100 Frights";
        full.type = ActivityType::Competing;
        full.statusDisplay = StatusDisplay::Details;
        full.detailsUrl = "https://d";
        full.stateUrl = "https://s";
        full.largeImageUrl = "https://l";
        full.smallImageUrl = "https://m";
        full.endTimestamp = 1795827520;
        full.partySize = 1;
        full.partyMax = 4;
        full.buttons = {};
        Expect(ActivityJson::Build(full, 42),
               kHead +
                   "\"name\":\"Night of 100 Frights\",\"type\":5,\"status_display_type\":2,"
                   "\"details\":\"On Edge in the Hedge!\",\"details_url\":\"https://d\","
                   "\"state\":\"30% complete | 1,240 Snacks\",\"state_url\":\"https://s\","
                   "\"assets\":{\"large_image\":\"frightworks-no100f-icon\",\"large_text\":\"Hedge Maze\","
                   "\"large_url\":\"https://l\",\"small_image\":\"frightworks-pfp\",\"small_text\":\"FrightWorks\","
                   "\"small_url\":\"https://m\"},"
                   "\"timestamps\":{\"start\":1795823920,\"end\":1795827520},\"party\":{\"size\":[1,4]}," +
                   kTail,
               "every field");

        // Discord refuses the whole activity for a one-character details or state: they're left out, and so is the
        // link on a text that isn't sent, or on a picture that isn't.
        Activity shortText;
        shortText.details = "x";
        shortText.detailsUrl = "https://d";
        shortText.state = "y";
        shortText.largeImageText = "z";
        shortText.largeImageUrl = "https://l";
        shortText.smallImageText = "ok";
        Expect(ActivityJson::Build(shortText, 42), kHead + "\"assets\":{\"small_text\":\"ok\"}," + kTail,
               "short texts and orphan links left out");

        // Playing and the name display are Discord's defaults: not written.
        Activity defaults;
        defaults.details = "In the main menu";
        Expect(ActivityJson::Build(defaults, 42), kHead + "\"details\":\"In the main menu\"," + kTail, "defaults");

        Expect(ActivityJson::Escape("a\"b\\c\nd\x01"), "a\\\"b\\\\c\\nd\\u0001", "escaping");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "discord_activity_json_tests: ok\n";
    return 0;
}
