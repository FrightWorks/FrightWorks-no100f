#pragma once

// Dev-menu logic that needs no SDL, ImGui or guest memory, so it can be unit tested.

#include <cstdint>
#include <cctype>
#include <string_view>

namespace no100f_devmenu {

struct Powerup {
    const char* key;  // Existing sd2.ini / command-file identity, independent of display order.
    const char* label;
    uint32_t bit;  // Zero denotes the separate Map-received boolean, not a gadget bit.
    bool unused = false;
};

inline constexpr Powerup kPowerupList[] = {
    {"Map", "Map", 0},
    {"Shovel", "Shovel", 1u << 13},
    {"Spring", "Springs", 1u << 9},
    {"FootballHelmet", "Helmet", 1u << 11},
    {"LampShade", "Lamp", 1u << 5},
    {"Slippers", "Slippers", 1u << 4},
    {"BlackKnight", "Knight Armor", 1u << 6},
    {"StickyBoots", "Galoshes (Boots)", 1u << 2},
    {"LightningBolt", "Super Smash", 1u << 10},
    {"LightningBoltStun", "Super Sonic Smash (Stun)", 1u << 14},
    {"Umbrella", "Umbrella", 1u << 12},
    {"Plungers", "Plunger Boots", 1u << 3},
    {"BubbleGum", "Bubblegum", 1u << 0},
    {"SoapBar", "Soap Bubbles", 1u << 1},
    {"FlowerPot", "Flower Pot", 1u << 7, true},
    {"DivingHelmet", "Diving Helmet", 1u << 8, true},
};
inline constexpr const char* kUnusedPowerupHeading = "--UNUSED--";

constexpr uint32_t PowerupMask(bool includeUnused) {
    uint32_t mask = 0;
    for (const auto& power : kPowerupList)
        if (includeUnused || !power.unused) mask |= power.bit;
    return mask;
}

inline constexpr uint32_t kAllPowerups = PowerupMask(true);
inline constexpr uint32_t kMainGamePowerups = PowerupMask(false);

constexpr uint32_t WithPowerupBits(uint32_t current, uint32_t mask, uint32_t bits) {
    return (current & ~mask) | (bits & mask);
}

constexpr bool PowerupEnabled(const Powerup& power, uint32_t value) {
    return power.bit ? (value & power.bit) != 0 : value != 0;
}

constexpr uint32_t WithPowerup(const Powerup& power, uint32_t value, bool on) {
    return power.bit ? WithPowerupBits(value, power.bit, on ? power.bit : 0) : (on ? 1u : 0u);
}

// Which button skips what is playing: A for a recorded cutscene, Start for the title intro / attract demo (mode 2).
enum class SkipControl { None, Confirm, Start };

inline SkipControl SkipControlFor(uint32_t mode, uint32_t state, bool recordedCutscene) {
    if (recordedCutscene) return SkipControl::Confirm;
    if (mode == 2 && (state == 0 || state == 1)) return SkipControl::Start;
    return SkipControl::None;
}



// One queued scene change: waits until the game is in play with no cutscene and no portal pending, submits once,
// then reports arrival; gives up after 60 s.
class SceneRequest {
public:
    enum class Action { Wait, Submit, Arrived, Timeout };

    bool Begin(uint32_t destination, uint64_t now) {
        if (destination == 0 || destination_ != 0) return false;
        destination_ = destination;
        deadline_ = now + 60000;
        submitted_ = false;
        return true;
    }

    Action Update(uint32_t current, bool ready, bool cinematic, bool portalPending, uint64_t now) {
        if (!Active()) return Action::Wait;
        if (now >= deadline_) {
            Cancel();
            return Action::Timeout;
        }
        if (!ready) return Action::Wait;
        if (current == destination_ && (submitted_ || !cinematic)) {
            Cancel();
            return Action::Arrived;
        }
        if (!submitted_ && !cinematic && !portalPending) {
            submitted_ = true;
            return Action::Submit;
        }
        return Action::Wait;
    }

    bool Active() const { return destination_ != 0; }
    bool Submitted() const { return Active() && submitted_; }
    void Cancel() { destination_ = 0; }

private:
    uint32_t destination_ = 0;
    uint64_t deadline_ = 0;
    bool submitted_ = false;
};

// Skip button: press for 100 ms every 500 ms while the same sequence plays, for up to 10 s.
class SkipRequest {
public:
    void Begin(uint64_t now) {
        started_ = now;
        active_ = true;
    }

    bool Press(uint64_t now, bool cinematic) {
        if (!active_) return false;
        if (!cinematic || now - started_ >= 10000) {
            active_ = false;
            return false;
        }
        // Fresh presses survive the movie's initial unskippable interval.
        return (now - started_) % 500 < 100;
    }

    bool Active() const { return active_; }

private:
    uint64_t started_ = 0;
    bool active_ = false;
};

} // namespace no100f_devmenu
