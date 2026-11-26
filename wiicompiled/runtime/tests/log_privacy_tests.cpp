// The home-folder rewriting that keeps user names out of console.log (include/log_privacy.h).
#include "log_privacy.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
int g_failures = 0;

void Check(const char* name, const std::string& home, bool windows, std::string text, const std::string& expected) {
    LogPrivacy::Apply(text, LogPrivacy::RulesForHome(home, windows));
    if (text == expected) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s\n  got  '%s'\n  want '%s'\n", name, text.c_str(), expected.c_str());
}

// The Windows build under Wine: the Wine profile's rules (if any) plus the Linux home's, as CurrentRules builds them.
void CheckWine(const char* name, const std::string& unixHome, std::string text, const std::string& expected,
               const std::string& wineProfile = "") {
    std::vector<LogPrivacy::Rule> rules = LogPrivacy::RulesForHome(wineProfile, true);
    for (LogPrivacy::Rule& rule : LogPrivacy::RulesForWineHost(unixHome)) rules.push_back(std::move(rule));
    LogPrivacy::detail::SortLongestFirst(rules);
    LogPrivacy::Apply(text, rules);
    if (text == expected) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s\n  got  '%s'\n  want '%s'\n", name, text.c_str(), expected.c_str());
}

void CheckHomeDir(const char* name, const std::string& wineHomeDir, const std::string& expected) {
    const std::string got = LogPrivacy::UnixHomeFromWineHomeDir(wineHomeDir);
    if (got == expected) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s\n  got  '%s'\n  want '%s'\n", name, got.c_str(), expected.c_str());
}
}  // namespace

int main() {
    // Linux / macOS
    Check("linux path", "/home/nick", false, "cache at /home/nick/.local/share/WiiCompiled/Cache",
          "cache at ~/.local/share/WiiCompiled/Cache");
    Check("linux trailing slash home", "/home/nick/", false, "'/home/nick/x'", "'~/x'");
    Check("linux several", "/home/nick", false, "/home/nick/a and /home/nick/b", "~/a and ~/b");
    Check("linux whole home", "/home/nick", false, "HOME=/home/nick", "HOME=~");
    Check("linux longer name kept", "/home/nick", false, "/home/nick2/x /home/nick.old/y", "/home/nick2/x /home/nick.old/y");
    Check("linux inside another path kept", "/home/nick", false, "/mnt/backup/home/nick/x", "/mnt/backup/home/nick/x");
    Check("bazzite var home", "/var/home/nick", false, "/var/home/nick/a /home/nick/b", "~/a ~/b");
    Check("home also covers var home", "/home/nick", false, "/var/home/nick/a", "~/a");
    Check("linux case matters", "/home/nick", false, "/home/Nick/x", "/home/Nick/x");
    Check("file url", "/home/nick", false, "file:///home/nick/x", "file://~/x");
    Check("root home ignored", "/", false, "/usr/lib/x", "/usr/lib/x");
    Check("empty home", "", false, "/home/nick/x", "/home/nick/x");
    Check("deck", "/home/deck", false, "argv=/home/deck/Games/FrightWorks/data/FrightWorks Game",
          "argv=~/Games/FrightWorks/data/FrightWorks Game");

    // Windows
    Check("windows backslashes", "C:\\Users\\Nick", true,
          "C:\\Users\\Nick\\AppData\\Local\\FrightWorks-Data\\Config.toml",
          "%USERPROFILE%\\AppData\\Local\\FrightWorks-Data\\Config.toml");
    Check("windows forward slashes", "C:\\Users\\Nick", true, "dvd_root=C:/Users/Nick/Games/DATA",
          "dvd_root=%USERPROFILE%/Games/DATA");
    Check("windows any case", "C:\\Users\\Nick", true, "c:\\users\\NICK\\x", "%USERPROFILE%\\x");
    Check("windows longer name kept", "C:\\Users\\Nick", true, "C:\\Users\\Nicky\\x", "C:\\Users\\Nicky\\x");
    Check("windows long path prefix", "C:\\Users\\Nick", true, "\\\\?\\C:\\Users\\Nick\\x", "\\\\?\\%USERPROFILE%\\x");
    Check("windows other drive kept", "C:\\Users\\Nick", true, "D:\\My Documents\\x", "D:\\My Documents\\x");
    Check("windows utf8 name", "C:\\Users\\Jos\xC3\xA9", true, "C:\\Users\\Jos\xC3\xA9\\x C:\\Users\\Jos\xC3\xA9s\\y",
          "%USERPROFILE%\\x C:\\Users\\Jos\xC3\xA9s\\y");

    // Wine / Proton: the Windows build also sees the Linux home through Z: and in Unix form.
    CheckWine("wine z backslashes", "/home/nick", "Z:\\home\\nick\\Games\\FrightWorks\\data\\FrightWorks Game.exe",
              "~\\Games\\FrightWorks\\data\\FrightWorks Game.exe");
    CheckWine("wine z slashes", "/home/nick", "dvd_root=Z:/home/nick/Games/DATA", "dvd_root=~/Games/DATA");
    CheckWine("wine any case", "/home/nick", "z:\\HOME\\NICK\\x", "~\\x");
    CheckWine("wine unix form", "/home/nick", "HOME=/home/nick, /home/nick/x", "HOME=~, ~/x");
    CheckWine("wine bazzite", "/var/home/nick", "Z:\\var\\home\\nick\\a Z:\\home\\nick\\b /var/home/nick/c",
              "~\\a ~\\b ~/c");
    CheckWine("wine longer name kept", "/home/nick", "Z:\\home\\nicky\\x", "Z:\\home\\nicky\\x");
    CheckWine("wine other folders kept", "/home/nick", "Z:\\mnt\\games\\x C:\\windows\\x", "Z:\\mnt\\games\\x C:\\windows\\x");
    CheckWine("wine with its own profile", "/home/nick", "C:\\users\\nick\\AppData\\x Z:\\home\\nick\\y",
              "%USERPROFILE%\\AppData\\x ~\\y", "C:\\users\\nick");
    CheckWine("wine no home", "", "Z:\\home\\nick\\x", "Z:\\home\\nick\\x");
    CheckHomeDir("winehomedir", "\\??\\Z:\\home\\nick", "/home/nick");
    CheckHomeDir("winehomedir plain", "Z:\\var\\home\\nick", "/var/home/nick");
    CheckHomeDir("winehomedir not a path", "nick", "");
    CheckHomeDir("winehomedir empty", "", "");

    if (g_failures == 0) std::printf("log_privacy_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
