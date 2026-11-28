#pragma once

#include <array>
#include <cstdint>
#include <string>

// Discord Rich Presence through the local Discord app's IPC pipe or socket (no SDK). The host sets its own
// activity (SetBasicActivity); Wii guests can also use Dolphin's /dev/dolphin Discord contract. The guest
// sends only strings and big-endian integer fields; the IPC wire protocol is
// owned here so translated game code never needs host SDK headers.
namespace DiscordPresence {

// A link under the status, seen by other people (Discord doesn't show your own buttons to you). Label up to 32
// characters.
struct ActivityButton {
    std::string label;
    std::string url;
};

// What the status line says before the app's name: "Playing", "Listening to", "Watching", "Competing in".
enum class ActivityType { Playing = 0, Listening = 2, Watching = 3, Competing = 5 };

// Which text the short status (member list, DMs) shows after "Playing": the app's name, the state or the details.
enum class StatusDisplay { Name = 0, State = 1, Details = 2 };

struct Activity {
    std::string name;  // replaces the app's name on the status's top line; empty = the app's name
    ActivityType type = ActivityType::Playing;
    StatusDisplay statusDisplay = StatusDisplay::Name;
    std::string details;     // at least 2 characters, or left out (Discord refuses the whole call otherwise)
    std::string detailsUrl;  // clicking the details text opens this
    std::string state;       // at least 2 characters, or left out
    std::string stateUrl;
    std::string largeImageKey;  // an Art Assets name, or an https URL (Discord fetches it through its image proxy)
    std::string largeImageText;
    std::string largeImageUrl;  // clicking the picture opens this
    std::string smallImageKey;
    std::string smallImageText;
    std::string smallImageUrl;
    int64_t startTimestamp = 0;
    int64_t endTimestamp = 0;
    uint32_t partySize = 0;
    uint32_t partyMax = 0;
    std::array<ActivityButton, 2> buttons;  // Discord allows two; an empty label or URL leaves one out
};

void Initialize(const std::string& basicClientId, const std::string& basicTitle);
// The host's own activity (FrightWorks: what the game is doing), shown while no guest client is set.
void SetBasicActivity(Activity activity);
void SetClient(const std::string& clientId);
void SetActivity(Activity activity);
void Reset();
void Shutdown();

} // namespace DiscordPresence
