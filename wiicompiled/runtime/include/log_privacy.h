#pragma once

// Keeps the player's user name out of the logs they send us: their home folder becomes "~" (Linux, macOS) or
// "%USERPROFILE%" (Windows) wherever a log line has it; under Wine or Proton the Linux home (Z:\home\name) becomes "~"
// too. console.log goes through this as it is written (main.cpp),
// and so do the crash files' details. Names elsewhere (a disc image in another folder that happens to carry it) stay.
// Header-only so tests/log_privacy_tests.cpp can check it without the runtime.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace LogPrivacy {

struct Rule {
    std::string prefix;       // the home folder as it shows up in a path
    std::string replacement;  // "~" or "%USERPROFILE%"
    bool ignoreCase = false;  // Windows paths
};

namespace detail {

inline bool IsNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.' ||
           static_cast<unsigned char>(c) >= 0x80;  // UTF-8 letters in a user name
}

inline bool SameText(std::string_view a, std::string_view b, bool ignoreCase) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = ignoreCase ? static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))) : a[i];
        const char y = ignoreCase ? static_cast<char>(std::tolower(static_cast<unsigned char>(b[i]))) : b[i];
        if (x != y) return false;
    }
    return true;
}

inline void AddRule(std::vector<Rule>& rules, std::string prefix, const char* replacement, bool ignoreCase) {
    while (prefix.size() > 1 && (prefix.back() == '/' || prefix.back() == '\\')) prefix.pop_back();
    // A home that is the root or a bare drive ("/", "C:") would rewrite every path: leave those alone.
    if (prefix.size() < 4) return;
    for (const Rule& rule : rules) {
        if (SameText(rule.prefix, prefix, ignoreCase)) return;
    }
    rules.push_back({std::move(prefix), replacement, ignoreCase});
}

// Longest first, so /var/home/name is replaced before /home/name (which it contains).
inline void SortLongestFirst(std::vector<Rule>& rules) {
    std::stable_sort(rules.begin(), rules.end(),
                     [](const Rule& a, const Rule& b) { return a.prefix.size() > b.prefix.size(); });
}

}  // namespace detail

// The rules for a home folder. windows: home is like C:\Users\Name (both slash forms get a rule); otherwise like
// /home/name, and /home/name and /var/home/name are both covered (Fedora Atomic systems such as Bazzite set HOME to
// /var/home/name while /home links to it, so paths show up either way).
inline std::vector<Rule> RulesForHome(const std::string& home, bool windows) {
    std::vector<Rule> rules;
    if (home.empty()) return rules;
    if (windows) {
        std::string backslashes = home;
        std::replace(backslashes.begin(), backslashes.end(), '/', '\\');
        std::string slashes = home;
        std::replace(slashes.begin(), slashes.end(), '\\', '/');
        detail::AddRule(rules, backslashes, "%USERPROFILE%", true);
        detail::AddRule(rules, slashes, "%USERPROFILE%", true);
    } else {
        detail::AddRule(rules, home, "~", false);
        constexpr std::string_view kVarHome = "/var/home/";
        constexpr std::string_view kHome = "/home/";
        if (home.rfind(kVarHome, 0) == 0) {
            detail::AddRule(rules, std::string(kHome) + home.substr(kVarHome.size()), "~", false);
        } else if (home.rfind(kHome, 0) == 0) {
            detail::AddRule(rules, std::string(kVarHome) + home.substr(kHome.size()), "~", false);
        }
    }
    detail::SortLongestFirst(rules);
    return rules;
}

// The Linux home folder as the Windows build sees it under Wine or Proton: through drive Z: (Wine maps Z: to /), as
// Z:\home\name or Z:/home/name, and in Unix form in values Wine passes through. unixHome is the Linux $HOME; an empty
// or non-Unix value gives no rules.
inline std::vector<Rule> RulesForWineHost(const std::string& unixHome) {
    std::vector<Rule> rules;
    if (unixHome.empty() || unixHome.front() != '/') return rules;
    for (const Rule& unixRule : RulesForHome(unixHome, false)) {
        detail::AddRule(rules, unixRule.prefix, "~", false);
        std::string dos = "Z:" + unixRule.prefix;
        detail::AddRule(rules, dos, "~", true);
        std::replace(dos.begin(), dos.end(), '/', '\\');
        detail::AddRule(rules, dos, "~", true);
    }
    detail::SortLongestFirst(rules);
    return rules;
}

// The Linux home from Wine's WINEHOMEDIR ("\??\Z:\home\name"), for when HOME isn't passed through; "" if it isn't
// in that form.
inline std::string UnixHomeFromWineHomeDir(std::string value) {
    constexpr std::string_view kNtPrefix = "\\??\\";
    if (value.rfind(kNtPrefix, 0) == 0) value.erase(0, kNtPrefix.size());
    if (value.size() < 3 || value[1] != ':' || (value[2] != '\\' && value[2] != '/')) return {};
    value.erase(0, 2);  // the drive letter: Wine's Z: is /
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

// Replaces each rule's folder where it stands as a whole path part: not "/home/name2", not "x/home/name".
inline void Apply(std::string& text, const std::vector<Rule>& rules) {
    for (const Rule& rule : rules) {
        const size_t n = rule.prefix.size();
        for (size_t pos = 0; pos + n <= text.size();) {
            if (!detail::SameText(std::string_view(text).substr(pos, n), rule.prefix, rule.ignoreCase) ||
                (pos > 0 && detail::IsNameChar(text[pos - 1])) ||
                (pos + n < text.size() && detail::IsNameChar(text[pos + n]))) {
                ++pos;
                continue;
            }
            text.replace(pos, n, rule.replacement);
            pos += rule.replacement.size();
        }
    }
}

// The rules for this process's user, worked out once.
inline const std::vector<Rule>& CurrentRules() {
    static const std::vector<Rule> rules = [] {
#if defined(_WIN32)
        // The wide variable, as UTF-8 like the log's paths (getenv would give the ANSI code page, which doesn't match
        // a user name with letters outside it).
        const auto envUtf8 = [](const wchar_t* name) -> std::string {
            const wchar_t* value = _wgetenv(name);
            if (value == nullptr) return {};
            const std::u8string utf8 = std::filesystem::path(value).u8string();
            return std::string(utf8.begin(), utf8.end());
        };
        std::vector<Rule> all = RulesForHome(envUtf8(L"USERPROFILE"), true);
        // Under Wine or Proton (ntdll has wine_get_version), the Linux home shows up too, through Z:.
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr) {
            std::string unixHome = envUtf8(L"HOME");
            if (unixHome.empty() || unixHome.front() != '/') unixHome = UnixHomeFromWineHomeDir(envUtf8(L"WINEHOMEDIR"));
            for (Rule& rule : RulesForWineHost(unixHome)) all.push_back(std::move(rule));
            detail::SortLongestFirst(all);
        }
        return all;
#else
        const char* home = std::getenv("HOME");
        return RulesForHome(home != nullptr ? home : "", false);
#endif
    }();
    return rules;
}

inline std::string Sanitized(std::string text) {
    Apply(text, CurrentRules());
    return text;
}

}  // namespace LogPrivacy
