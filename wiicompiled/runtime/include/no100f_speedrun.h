#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Night of 100 Frights: play time per save and the speedrun timer (F10 > Speedrun, the overlay, the run files).
// Pure, so the timing rules are tested without the game (tests/no100f_speedrun_tests.cpp); the game side samples the
// game each retrace and keeps the files (src/no100f_speedrun.cpp).
namespace no100f_speedrun {

// Game modes and states (no100f_game_state.h; load probe 2026-11-27): 2 title, 4 load game, 6 the Mystery Machine
// (save) menu, 7 pause, 11 in play. In play, state 0 is a level loading (after New Game or a save loads), 4 the fade
// out of a scene switch and 5 the next scene loading; 1 is playing.
inline constexpr uint32_t kModeTitle = 2, kModeLoadGame = 4, kModeSaveMenu = 6, kModePause = 7, kModeInPlay = 11;

constexpr bool IsLoading(uint32_t mode, uint32_t state) {
    return mode == kModeInPlay && (state == 0 || state == 4 || state == 5);
}

// Play time (human's call, 2026-11-27): playing and paused, the save menu included; not loads, Bink movies, the
// title or the load menu.
constexpr bool CountsAsPlayTime(uint32_t mode, uint32_t state, bool movie) {
    if (movie) return false;
    if (mode == kModeInPlay) return !IsLoading(mode, state);
    return mode == kModePause || mode == kModeSaveMenu;
}

// A gap between two samples longer than this (the PC slept, a debugger stopped the game) doesn't count as play time.
inline constexpr double kMaxStep = 0.5;

// The bosses and the Bink movie each one's defeat starts. main.dol names cin_mmpd "cin_mm_post_death"; the other three
// follow each boss's intro movie (main.dol's intro table: cin_2, cin_4, cin_6, cin_10; frames checked 2026-11-27).
enum class Boss { BlackKnight, GreenGhost, Redbeard, Mastermind };
struct BossInfo {
    Boss boss;
    const char* name;
    const char* defeatMovie;
};
inline constexpr std::array<BossInfo, 4> kBosses = {{
    {Boss::BlackKnight, "Black Knight", "cin3.bik"},
    {Boss::GreenGhost, "Green Ghost", "cin5.bik"},
    {Boss::Redbeard, "Redbeard", "cin7.bik"},
    {Boss::Mastermind, "Mastermind", "cin_mmpd.bik"},
}};

inline bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? char(a[i] | 0x20) : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? char(b[i] | 0x20) : b[i];
        if (x != y) return false;
    }
    return true;
}

inline const BossInfo* BossForMovie(std::string_view movie) {
    for (const BossInfo& info : kBosses)
        if (SameName(movie, info.defeatMovie)) return &info;
    return nullptr;
}

inline constexpr uint32_t kTokenTotal = 21;  // no100f_progress::kTokenTotal

// What ends a run. All of speedrun.com's categories (game n100f) start on the press of New Game.
enum class End {
    Mastermind,   // "the instant Mastermind is electrocuted": his defeat movie starts
    AllBosses,    // the fourth boss defeated (All Bosses, Reverse Boss Order)
    AllTokens,    // the Mastermind and all 21 Monster Tokens, whichever comes last
    Complete100,  // the Mastermind and 100% completion, whichever comes last
};
struct Category {
    const char* id;    // Config.toml [speedrun] category
    const char* name;  // speedrun.com's name
    End end;
    bool tokenSplits;  // a split for each Monster Token too
    const char* rules; // the F10 description: when it ends
};
inline constexpr std::array<Category, 8> kCategories = {{
    {"any", "Any%", End::Mastermind, false, "Ends when the Mastermind's defeat movie starts."},
    {"all-bosses", "All Bosses", End::AllBosses, false, "Ends when the fourth boss's defeat movie starts."},
    {"100", "100%", End::Complete100, true,
     "Ends at the Mastermind's defeat or 100% completion, whichever comes last. speedrun.com checks the save."},
    {"amt", "All Monster Tokens", End::AllTokens, true,
     "Ends at the Mastermind's defeat or the 21st Monster Token, whichever comes last."},
    {"rbo", "Reverse Boss Order", End::AllBosses, false,
     "Ends when the fourth boss's defeat movie starts (the Black Knight, in this category's order)."},
    {"warpless", "Warpless", End::Mastermind, false, "Ends when the Mastermind's defeat movie starts."},
    {"nle", "No Levels Early", End::Mastermind, false, "Ends when the Mastermind's defeat movie starts."},
    {"cheat", "Cheat%", End::Mastermind, false, "Ends when the Mastermind's defeat movie starts."},
}};

inline int CategoryIndex(std::string_view id) {
    for (size_t i = 0; i < kCategories.size(); ++i)
        if (id == kCategories[i].id) return int(i);
    return 0;
}

// One game sample (each retrace).
struct Frame {
    double now = 0;  // seconds, steady clock
    bool layout = false;  // a known main.dol
    uint32_t mode = 0, state = 0;
    bool movie = false;          // a Bink movie is open
    std::string movieStarted;    // a movie opened since the last sample: its file name
    bool progressKnown = false;  // the live completion below can be read
    uint32_t percent = 0, tokens = 0;
    std::string place;  // the level (or area) Scooby is in, for token split names
    bool gallery = false;  // in the Monster Gallery, where the boss movies can be watched again
};

struct Split {
    std::string name;
    double realTime = 0;     // since the run's start
    double withoutLoads = 0; // the same, less loads
};

// The run timer and the save's play time, fed one Frame per retrace. Save events come from the memory card calls.
class Tracker {
public:
    // The category for the next run (a run under way keeps its own).
    void SetCategory(int index) { nextCategory_ = std::clamp(index, 0, int(kCategories.size()) - 1); }
    int NextCategory() const { return nextCategory_; }

    // A press of A or Start on any controller. On the title menu it may be the press of New Game: the game fades out
    // for about 2 s before it leaves the menu (probe 2026-11-27), and the run starts at the press.
    void ConfirmPressed(double now) {
        if (mode_ == kModeTitle) lastConfirm_ = now;
    }

    // A save was loaded (its play time from the play-time file, 0 if none was kept), or written.
    void SaveLoaded(double playSeconds) {
        playSeconds_ = std::max(0.0, playSeconds);
        playKnown_ = true;
    }
    double PlaySeconds() const { return playSeconds_; }
    bool PlayKnown() const { return playKnown_; }

    void Update(const Frame& f) {
        const double step = haveLast_ ? f.now - lastNow_ : 0.0;
        haveLast_ = true;
        lastNow_ = f.now;
        if (!f.layout) {
            mode_ = 0;
            return;
        }
        const uint32_t previousMode = mode_;
        mode_ = f.mode;
        // New Game: straight from the title into play (a save goes through the load game menu, mode 4).
        if (previousMode == kModeTitle && f.mode == kModeInPlay) {
            const double pressed = lastConfirm_ > 0 && f.now - lastConfirm_ < 4.0 ? lastConfirm_ : f.now;
            StartRun(pressed);
            playSeconds_ = 0;
            playKnown_ = true;
            lastTokens_ = f.progressKnown ? f.tokens : 0;
        }
        if (playKnown_ && step > 0 && step <= kMaxStep && CountsAsPlayTime(f.mode, f.state, f.movie)) {
            playSeconds_ += step;
        }
        if (running_) {
            if (step > 0 && !IsLoading(f.mode, f.state)) loadless_ += step;
            if (!f.movieStarted.empty() && !f.gallery) {
                if (const BossInfo* boss = BossForMovie(f.movieStarted)) BossDefeated(*boss, f.now);
            }
            if (f.progressKnown) {
                const Category& category = kCategories[size_t(category_)];
                if (f.tokens > lastTokens_ && f.tokens <= kTokenTotal && category.tokenSplits) {
                    for (uint32_t t = lastTokens_ + 1; t <= f.tokens; ++t)
                        AddSplit("Token " + std::to_string(t) + (f.place.empty() ? "" : " (" + f.place + ")"), f.now);
                }
                lastTokens_ = f.tokens;
                tokens_ = f.tokens;
                percent_ = f.percent;
            }
            CheckEnd(f.now);
        }
    }

    bool Running() const { return running_; }
    bool Finished() const { return finished_; }
    bool Started() const { return running_ || finished_; }
    int RunCategory() const { return category_; }
    // The run's time at `now` (frozen once it ends).
    double RealTime(double now) const { return finished_ ? endReal_ : running_ ? now - start_ : 0.0; }
    // Without loads: counted up to the last sample.
    double WithoutLoads() const { return finished_ ? endLoadless_ : loadless_; }
    const std::vector<Split>& Splits() const { return splits_; }
    double StartTime() const { return start_; }

private:
    void StartRun(double at) {
        running_ = true;
        finished_ = false;
        category_ = nextCategory_;
        start_ = at;
        loadless_ = std::max(0.0, lastNow_ - at);  // the menu's fade after the press isn't a load
        splits_.clear();
        defeated_ = {};
        lastTokens_ = tokens_ = percent_ = 0;
        lastConfirm_ = 0;
    }

    void AddSplit(std::string name, double now) {
        splits_.push_back({std::move(name), now - start_, loadless_});
    }

    void BossDefeated(const BossInfo& boss, double now) {
        bool& done = defeated_[size_t(boss.boss)];
        if (done) return;  // a movie watched again from the gallery
        done = true;
        AddSplit(boss.name, now);
    }

    void CheckEnd(double now) {
        const bool mastermind = defeated_[size_t(Boss::Mastermind)];
        bool end = false;
        switch (kCategories[size_t(category_)].end) {
        case End::Mastermind: end = mastermind; break;
        case End::AllBosses: end = std::all_of(defeated_.begin(), defeated_.end(), [](bool d) { return d; }); break;
        case End::AllTokens: end = mastermind && tokens_ >= kTokenTotal; break;
        case End::Complete100: end = mastermind && percent_ >= 100; break;
        }
        if (!end) return;
        running_ = false;
        finished_ = true;
        endReal_ = now - start_;
        endLoadless_ = loadless_;
        // The last split is the run's end. A boss or a token that ended it is already there; 100% gets its own.
        if (splits_.empty() || splits_.back().realTime < endReal_ - 1e-9) {
            splits_.push_back({kCategories[size_t(category_)].end == End::Complete100 ? "100%" : "End", endReal_,
                               endLoadless_});
        }
    }

    int nextCategory_ = 0;
    int category_ = 0;
    uint32_t mode_ = 0;
    bool haveLast_ = false;
    double lastNow_ = 0;
    double lastConfirm_ = 0;
    double playSeconds_ = 0;
    bool playKnown_ = false;
    bool running_ = false, finished_ = false;
    double start_ = 0, loadless_ = 0, endReal_ = 0, endLoadless_ = 0;
    std::vector<Split> splits_;
    std::array<bool, 4> defeated_{};
    uint32_t lastTokens_ = 0, tokens_ = 0, percent_ = 0;
};

// "1:02:03.45", "2:03.45", "3.45"; hundredths, rounded down.
inline std::string FormatTime(double seconds, bool hundredths = true) {
    if (!(seconds > 0)) seconds = 0;
    const auto centis = static_cast<long long>(std::floor(seconds * 100.0 + 1e-6));
    const long long total = centis / 100, h = total / 3600, m = total / 60 % 60, s = total % 60, c = centis % 100;
    char text[48];
    if (h > 0) std::snprintf(text, sizeof text, "%lld:%02lld:%02lld", h, m, s);
    else if (m > 0) std::snprintf(text, sizeof text, "%lld:%02lld", m, s);
    else std::snprintf(text, sizeof text, "%lld", s);
    std::string out = text;
    if (hundredths) {
        std::snprintf(text, sizeof text, ".%02lld", c);
        out += text;
    }
    return out;
}

// A clock: "0:00:12", "3:05:09".
inline std::string FormatClock(double seconds) {
    const auto total = static_cast<long long>(std::max(0.0, seconds));
    char text[48];
    std::snprintf(text, sizeof text, "%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
    return text;
}

// Play time as people say it: "3 h 05 min", "12 min", "less than a minute".
inline std::string FormatPlayTime(double seconds) {
    const auto minutes = static_cast<long long>(std::max(0.0, seconds) / 60.0);
    if (minutes < 1) return "less than a minute";
    char text[48];
    if (minutes < 60) std::snprintf(text, sizeof text, "%lld min", minutes);
    else std::snprintf(text, sizeof text, "%lld h %02lld min", minutes / 60, minutes % 60);
    return text;
}

// LiveSplit's time: "01:02:03.4500000".
inline std::string LssTime(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    const auto ticks = static_cast<long long>(std::llround(seconds * 1e7));
    const long long total = ticks / 10000000, frac = ticks % 10000000;
    char text[48];
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld.%07lld", total / 3600, total / 60 % 60, total % 60, frac);
    return text;
}

inline std::string XmlEscape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

inline std::string CsvField(std::string_view text) {
    if (text.find_first_of(",\"\n") == std::string_view::npos) return std::string(text);
    std::string out = "\"";
    for (const char c : text) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// The run as CSV: one row per split, with the segment (time since the split before it).
inline std::string RunCsv(const std::vector<Split>& splits) {
    std::string out = "Split,Real time,Without loads,Segment (real time),Segment (without loads)\n";
    double lastReal = 0, lastLoadless = 0;
    for (const Split& s : splits) {
        out += CsvField(s.name) + "," + FormatTime(s.realTime) + "," + FormatTime(s.withoutLoads) + "," +
               FormatTime(s.realTime - lastReal) + "," + FormatTime(s.withoutLoads - lastLoadless) + "\n";
        lastReal = s.realTime;
        lastLoadless = s.withoutLoads;
    }
    return out;
}

// "11/27/2026 21:05:00" in UTC, as LiveSplit writes an attempt's start and end.
inline std::string LssDate(std::time_t when) {
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &when);
#else
    gmtime_r(&when, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%m/%d/%Y %H:%M:%S", &utc);
    return text;
}

// The run as a LiveSplit splits file: its splits as the Personal Best, so LiveSplit can compare the next run with it.
// Game time is the time without loads.
inline std::string RunLss(const std::vector<Split>& splits, const char* category, std::time_t started,
                          std::time_t ended) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Run version=\"1.7.0\">\n  <GameIcon />\n"
                      "  <GameName>Scooby-Doo! Night of 100 Frights</GameName>\n  <CategoryName>" +
                      XmlEscape(category) +
                      "</CategoryName>\n  <LayoutPath>\n  </LayoutPath>\n  <Metadata>\n    <Run id=\"\" />\n"
                      "    <Platform usesEmulator=\"False\">\n    </Platform>\n    <Region>\n    </Region>\n"
                      "    <Variables />\n    <CustomVariables />\n  </Metadata>\n  <Offset>00:00:00</Offset>\n"
                      "  <AttemptCount>1</AttemptCount>\n  <AttemptHistory>\n";
    const double endReal = splits.empty() ? 0 : splits.back().realTime;
    const double endLoadless = splits.empty() ? 0 : splits.back().withoutLoads;
    out += "    <Attempt id=\"1\" started=\"" + LssDate(started) + "\" isStartedSynced=\"False\" ended=\"" +
           LssDate(ended) + "\" isEndedSynced=\"False\">\n      <RealTime>" + LssTime(endReal) +
           "</RealTime>\n      <GameTime>" + LssTime(endLoadless) + "</GameTime>\n    </Attempt>\n";
    out += "  </AttemptHistory>\n  <Segments>\n";
    double lastReal = 0, lastLoadless = 0;
    for (const Split& s : splits) {
        const std::string segReal = LssTime(s.realTime - lastReal), segGame = LssTime(s.withoutLoads - lastLoadless);
        out += "    <Segment>\n      <Name>" + XmlEscape(s.name) + "</Name>\n      <Icon />\n      <SplitTimes>\n"
               "        <SplitTime name=\"Personal Best\">\n          <RealTime>" + LssTime(s.realTime) +
               "</RealTime>\n          <GameTime>" + LssTime(s.withoutLoads) +
               "</GameTime>\n        </SplitTime>\n      </SplitTimes>\n      <BestSegmentTime>\n        <RealTime>" +
               segReal + "</RealTime>\n        <GameTime>" + segGame +
               "</GameTime>\n      </BestSegmentTime>\n      <SegmentHistory>\n        <Time id=\"1\">\n"
               "          <RealTime>" + segReal + "</RealTime>\n          <GameTime>" + segGame +
               "</GameTime>\n        </Time>\n      </SegmentHistory>\n    </Segment>\n";
        lastReal = s.realTime;
        lastLoadless = s.withoutLoads;
    }
    out += "  </Segments>\n  <AutoSplitterSettings />\n</Run>\n";
    return out;
}

// The play-time file: one "ScoobyDoo00 = 3725.4" line per save slot.
struct PlayTimeEntry {
    std::string slot;
    double seconds = 0;
};
inline std::vector<PlayTimeEntry> ParsePlayTimes(std::string_view text) {
    std::vector<PlayTimeEntry> entries;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(pos, end - pos));
        pos = end + 1;
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
        };
        const std::string slot = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        char* stop = nullptr;
        const double seconds = std::strtod(value.c_str(), &stop);
        if (slot.empty() || stop == value.c_str() || !(seconds >= 0) || !std::isfinite(seconds)) continue;
        entries.push_back({slot, seconds});
    }
    return entries;
}
inline std::string FormatPlayTimes(const std::vector<PlayTimeEntry>& entries) {
    std::string out = "# FrightWorks: play time per save slot, in seconds (playing and paused, not loads or movies)\n";
    char value[64];
    for (const PlayTimeEntry& e : entries) {
        std::snprintf(value, sizeof value, "%.1f", e.seconds);
        out += e.slot + " = " + value + "\n";
    }
    return out;
}
inline void SetPlayTime(std::vector<PlayTimeEntry>& entries, const std::string& slot, double seconds) {
    for (PlayTimeEntry& e : entries)
        if (e.slot == slot) {
            e.seconds = seconds;
            return;
        }
    entries.push_back({slot, seconds});
}
inline double FindPlayTime(const std::vector<PlayTimeEntry>& entries, const std::string& slot) {
    for (const PlayTimeEntry& e : entries)
        if (e.slot == slot) return e.seconds;
    return 0;
}

// A folder as a file:// URL for SDL_OpenURL: "D:\My Documents" becomes "file:///D:/My%20Documents".
inline std::string FileUrl(std::string_view path) {
    std::string out = path.empty() || path[0] != '/' ? "file:///" : "file://";
    for (const char c : path) {
        if (c == '\\') out += '/';
        else if (c == ' ') out += "%20";
        else if (c == '%') out += "%25";
        else if (c == '#') out += "%23";
        else out += c;
    }
    return out;
}

// A save slot's file name as the game opens it: "ScoobyDoo" and two digits.
inline bool IsSaveSlot(std::string_view name) {
    return name.size() == 11 && name.substr(0, 9) == "ScoobyDoo" && name[9] >= '0' && name[9] <= '9' &&
           name[10] >= '0' && name[10] <= '9';
}

// Memory card calls that load or write a whole save (probe 2026-11-27): loading reads the save's 24 KB of data at
// offset 16384, the load menu only 512-byte headers there (and the banner from offset 0); a save writes all 40 KB.
constexpr bool IsSaveLoad(int32_t offset, int32_t length) { return offset >= 16384 && length >= 8192; }
constexpr bool IsSaveWrite(int32_t length) { return length >= 8192; }

// The game side (src/no100f_speedrun.cpp). All of these are safe from any thread.
// Each retrace (vi.cpp): sample the game.
void Tick();
// Each PADRead (pad.cpp): the buttons the game gets, all controllers together.
void PadButtons(uint16_t buttons);
// Memory card calls on a save slot's file (card_gc.cpp).
void CardRead(const std::string& file, int32_t offset, int32_t length);
void CardWritten(const std::string& file, int32_t length);
// The category for the next run (F10 > Speedrun).
void SetNextCategory(int index);
// The loaded save's play time so far, for the Discord status; nothing before a save is loaded or a new game starts.
std::optional<double> PlaySeconds();

// What the overlay and F10 show.
struct View {
    bool started = false, running = false, finished = false;
    std::string category;  // the run's, or the next run's before one starts
    double realTime = 0, withoutLoads = 0;
    std::vector<Split> lastSplits;  // oldest first
    bool playKnown = false;
    double playSeconds = 0;
    std::string lastRunFile;  // the last run's CSV, once written
};
View CurrentView(size_t splits);
// Saves/Speedruns, made if it isn't there yet.
std::string RunsFolder();


}  // namespace no100f_speedrun
