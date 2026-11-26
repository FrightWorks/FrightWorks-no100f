// The home-folder rewriting that keeps user names out of console.log (include/log_privacy.h).
#include "log_privacy.h"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;

void Check(const char* name, const std::string& home, bool windows, std::string text, const std::string& expected) {
    LogPrivacy::Apply(text, LogPrivacy::RulesForHome(home, windows));
    if (text == expected) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s\n  got  '%s'\n  want '%s'\n", name, text.c_str(), expected.c_str());
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

    if (g_failures == 0) std::printf("log_privacy_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
