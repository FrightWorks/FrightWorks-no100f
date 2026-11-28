#pragma once

#include "discord_presence.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

// The SET_ACTIVITY frame's JSON (Discord's local IPC), pure so tests/discord_activity_json_tests.cpp checks it without
// a Discord app. Field names and what Discord accepts were checked against a running Discord on 2026-11-27: every key
// written here came back in its answer; a details or state of one character fails the whole call ("length must be at
// least 2 characters long"), so those are left out instead.
namespace DiscordPresence::ActivityJson {

inline std::string Escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '\"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += kHex[ch >> 4];
                escaped += kHex[ch & 0x0f];
            } else {
                escaped += static_cast<char>(ch);
            }
        }
    }
    return escaped;
}

inline void AppendString(std::ostringstream& output, bool& hasValue, std::string_view key, std::string_view value,
                         size_t minLength = 1) {
    if (value.size() < minLength) {
        return;
    }
    if (hasValue) {
        output << ',';
    }
    output << '\"' << key << "\":\"" << Escape(value) << '\"';
    hasValue = true;
}

inline void AppendNumber(std::ostringstream& output, bool& hasValue, std::string_view key, int64_t value) {
    if (hasValue) {
        output << ',';
    }
    output << '\"' << key << "\":" << value;
    hasValue = true;
}

inline std::string Build(const Activity& activity, int64_t pid) {
    std::ostringstream json;
    json << "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"wiicompiled\",\"args\":{\"pid\":" << pid << ",\"activity\":{";
    bool hasField = false;
    AppendString(json, hasField, "name", activity.name);
    if (activity.type != ActivityType::Playing) {
        AppendNumber(json, hasField, "type", static_cast<int>(activity.type));
    }
    if (activity.statusDisplay != StatusDisplay::Name) {
        AppendNumber(json, hasField, "status_display_type", static_cast<int>(activity.statusDisplay));
    }
    AppendString(json, hasField, "details", activity.details, 2);
    AppendString(json, hasField, "details_url", activity.details.size() >= 2 ? activity.detailsUrl : "");
    AppendString(json, hasField, "state", activity.state, 2);
    AppendString(json, hasField, "state_url", activity.state.size() >= 2 ? activity.stateUrl : "");
    const auto appendSection = [&](std::string_view name, const auto& append) {
        if (hasField) {
            json << ',';
        }
        json << '\"' << name << "\":{";
        append();
        json << '}';
        hasField = true;
    };
    if (!activity.largeImageKey.empty() || !activity.largeImageText.empty() || !activity.smallImageKey.empty() ||
        !activity.smallImageText.empty()) {
        appendSection("assets", [&] {
            bool hasAsset = false;
            AppendString(json, hasAsset, "large_image", activity.largeImageKey);
            AppendString(json, hasAsset, "large_text", activity.largeImageText, 2);
            AppendString(json, hasAsset, "large_url", activity.largeImageKey.empty() ? "" : activity.largeImageUrl);
            AppendString(json, hasAsset, "small_image", activity.smallImageKey);
            AppendString(json, hasAsset, "small_text", activity.smallImageText, 2);
            AppendString(json, hasAsset, "small_url", activity.smallImageKey.empty() ? "" : activity.smallImageUrl);
        });
    }
    if (activity.startTimestamp > 0 || activity.endTimestamp > 0) {
        appendSection("timestamps", [&] {
            bool hasTime = false;
            if (activity.startTimestamp > 0) {
                AppendNumber(json, hasTime, "start", activity.startTimestamp);
            }
            if (activity.endTimestamp > 0) {
                AppendNumber(json, hasTime, "end", activity.endTimestamp);
            }
        });
    }
    if (activity.partySize > 0 && activity.partyMax > 0) {
        appendSection("party", [&] {
            json << "\"size\":[" << activity.partySize << ',' << activity.partyMax << ']';
        });
    }
    bool hasButton = false;
    for (const ActivityButton& button : activity.buttons) {
        if (button.label.empty() || button.url.empty()) {
            continue;
        }
        if (hasField) {
            json << ',';
        }
        json << (hasButton ? "{" : "\"buttons\":[{");
        bool hasButtonField = false;
        AppendString(json, hasButtonField, "label", button.label);
        AppendString(json, hasButtonField, "url", button.url);
        json << '}';
        hasButton = true;
        hasField = true;
    }
    if (hasButton) {
        json << ']';
    }
    if (hasField) {
        json << ',';
    }
    json << "\"instance\":false}}}";
    return json.str();
}

}  // namespace DiscordPresence::ActivityJson
