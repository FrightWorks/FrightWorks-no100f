#pragma once

// Dev tools texture pack switcher (F6, the dev menu's Textures tab, `texpack` in devmenu.txt). Packs are folders,
// <UserData>/texture_packs/<name>/, any layout inside (the same file names as texture_replacements); the selected one is
// the only source of replacements in aurora (aurora_set_texture_pack); what it lacks draws magenta. This header is the part that needs
// neither the game nor aurora: the pack list, the cycle order, the command words and the dev tools gate.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace no100f_texture_packs {

// The pack folders under <userData>/texture_packs, in name order (letters of either case together). Hidden folders
// (a leading dot) and plain files are not packs.
inline std::vector<std::string> ListPacks(const std::filesystem::path& userData) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(userData / "texture_packs", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryError;
        if (!it->is_directory(entryError)) continue;
        const auto u8 = it->path().filename().u8string();
        std::string name(u8.begin(), u8.end());
        if (!name.empty() && name[0] != '.') names.push_back(std::move(name));
    }
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    return names;
}

// "" = the usual replacements (no pack). The cycle is usual, each pack in name order, then usual again. A pack that is
// no longer listed (its folder was deleted) counts as usual.
inline std::string NextInCycle(const std::vector<std::string>& packs, const std::string& active, bool backwards) {
    size_t position = 0;  // 0 = usual, 1.. = packs[position - 1]
    for (size_t i = 0; i < packs.size(); ++i)
        if (packs[i] == active) position = i + 1;
    const size_t count = packs.size() + 1;
    position = backwards ? (position + count - 1) % count : (position + 1) % count;
    return position == 0 ? std::string() : packs[position - 1];
}

inline std::string Label(const std::string& pack) { return pack.empty() ? std::string("usual (no pack)") : pack; }

// What the three ways in (key, menu, command file) share. `enabled` is the dev tools gate: off, nothing is listed or
// applied and every call answers that the developer tools are off.
class Switcher {
public:
    struct Hooks {
        std::function<std::vector<std::string>()> list;      // the packs, in name order
        std::function<bool(const std::string&)> apply;      // aurora_set_texture_pack; false = refused
    };

    Switcher(bool enabled, Hooks hooks) : enabled_(enabled), hooks_(std::move(hooks)) {}

    bool Enabled() const { return enabled_; }
    const std::string& Active() const { return active_; }

    // "" = usual. A pack that isn't listed is refused. Asking for the active pack again rescans it.
    bool Select(const std::string& pack) {
        if (!enabled_) return false;
        if (!pack.empty()) {
            const auto packs = hooks_.list();
            if (std::find(packs.begin(), packs.end(), pack) == packs.end()) return false;
        }
        if (!hooks_.apply(pack)) return false;
        active_ = pack;
        return true;
    }

    bool Cycle(bool backwards) {
        if (!enabled_) return false;
        return Select(NextInCycle(hooks_.list(), active_, backwards));
    }

    // The words after `texpack` in devmenu.txt: next, prev, usual (none, off), reload, list, a pack's name, or nothing
    // (what is active). The answer is the line for devmenu.log.
    std::string Command(std::string_view argument) {
        if (!enabled_) return "texpack: the developer tools are off";
        std::string word(argument);
        const size_t first = word.find_first_not_of(" \t\r");
        word = first == std::string::npos ? std::string() : word.substr(first, word.find_last_not_of(" \t\r") + 1 - first);
        const auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string key = lower(word);
        const auto packs = hooks_.list();
        if (key == "list") {
            std::string text = "texpack: usual (no pack)";
            for (const auto& pack : packs) text += ", " + pack;
            return text + " | active " + Label(active_);
        }
        if (key.empty()) return "texpack: active " + Label(active_);
        if (key == "next" || key == "prev") {
            if (!Cycle(key == "prev")) return "texpack: could not switch";
        } else if (key == "reload") {
            if (!Select(active_)) return "texpack: could not reload " + Label(active_);
        } else if (key == "usual" || key == "none" || key == "off") {
            if (!Select(std::string())) return "texpack: could not switch";
        } else {
            // A pack's own name; letters of either case match when only one pack fits.
            std::string match;
            int fits = 0;
            for (const auto& pack : packs) {
                if (pack == word) {
                    match = pack;
                    fits = 1;
                    break;
                }
                if (lower(pack) == key) {
                    match = pack;
                    ++fits;
                }
            }
            if (fits != 1) return "texpack: no pack named " + word + " (list shows them)";
            if (!Select(match)) return "texpack: could not switch to " + word;
        }
        return "texpack: " + Label(active_);
    }

private:
    bool enabled_;
    Hooks hooks_;
    std::string active_;
};

}  // namespace no100f_texture_packs
