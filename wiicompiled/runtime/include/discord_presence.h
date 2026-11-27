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

struct Activity {
    std::string details;
    std::string state;
    std::string largeImageKey;
    std::string largeImageText;
    std::string smallImageKey;
    std::string smallImageText;
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
