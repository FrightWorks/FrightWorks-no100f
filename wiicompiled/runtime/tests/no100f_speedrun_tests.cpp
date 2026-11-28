#include "no100f_speedrun.h"

#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace no100f_speedrun;

static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static void Near(double got, double want, double tolerance, const std::string& what) {
    Require(std::fabs(got - want) <= tolerance, what + ": got " + std::to_string(got) + ", want " + std::to_string(want));
}

// Feeds the tracker one sample per 1/60 s, like the retrace.
struct Game {
    Tracker tracker;
    double now = 0;
    uint32_t mode = 0, state = 0;
    bool movie = false;
    bool progressKnown = true;
    uint32_t percent = 0, tokens = 0;
    std::string place = "Shock on the Dock!";
    bool gallery = false;

    void Run(double seconds, const std::string& movieStarted = {}, uint32_t bossesDied = 0) {
        const int steps = static_cast<int>(std::lround(seconds * 60.0));
        for (int i = 0; i < steps; ++i) {
            now += 1.0 / 60.0;
            Frame f;
            f.now = now;
            f.layout = true;
            f.mode = mode;
            f.state = state;
            f.movie = movie;
            f.movieStarted = i == 0 ? movieStarted : std::string{};
            f.bossesDied = i == 0 ? bossesDied : 0;
            f.progressKnown = progressKnown;
            f.percent = percent;
            f.tokens = tokens;
            f.place = place;
            f.gallery = gallery;
            tracker.Update(f);
        }
    }
    void Set(uint32_t m, uint32_t s) {
        mode = m;
        state = s;
    }
    // A boss dies (its kill or death event), then `seconds` of play.
    void Die(Boss boss, double seconds) { Run(seconds, {}, 1u << unsigned(boss)); }
    // A boss's defeat movie: it starts, plays `seconds`, closes.
    void Movie(const char* name, double seconds) {
        movie = true;
        Run(seconds, name);
        movie = false;
    }
    // From the title: New Game pressed `at` seconds before the game leaves the menu (its fade), then the first level
    // loads for a second.
    void NewGame(double fade = 2.0, bool press = true) {
        Set(kModeTitle, 0);
        Run(1.0);
        if (press) tracker.ConfirmPressed(now);
        Run(fade);
        Set(kModeInPlay, 0);
        Run(1.0);
        Set(kModeInPlay, 1);
    }
};

int main() {
    try {
        // What counts as play time and as a load.
        Require(CountsAsPlayTime(kModeInPlay, 1, false), "playing counts");
        Require(CountsAsPlayTime(kModePause, 0, false), "pause counts");
        Require(CountsAsPlayTime(kModeSaveMenu, 0, false), "the save menu counts");
        Require(!CountsAsPlayTime(kModeInPlay, 1, true), "a movie doesn't");
        for (uint32_t s : {0u, 4u, 5u}) Require(!CountsAsPlayTime(kModeInPlay, s, false), "a load doesn't");
        Require(!CountsAsPlayTime(kModeTitle, 0, false) && !CountsAsPlayTime(kModeLoadGame, 0, false), "menus don't");
        Require(IsLoading(kModeInPlay, 5) && !IsLoading(kModeInPlay, 1) && !IsLoading(kModePause, 0), "loads");

        // Boss movies.
        Require(BossForMovie("cin_mmpd.bik")->boss == Boss::Mastermind, "Mastermind's movie");
        Require(BossForMovie("CIN5.BIK")->boss == Boss::GreenGhost, "any case");
        Require(BossForMovie("cin1.bik") == nullptr && BossForMovie("?") == nullptr, "not a defeat");
        Require(BossForMovie("cin3.bik")->boss == Boss::BlackKnight && BossForMovie("cin7.bik")->boss == Boss::Redbeard,
                "the other two");

        // Boss deaths: asset IDs as the level files have them, and the events that mean a death.
        Require(AssetId("BLACK_KNIGHT") == 0xEBB8C925u && AssetId("GREEN GHOST") == 0xB6C6E412u &&
                    AssetId("REDBEARD") == 0x51B8AABBu && AssetId("VIL_MASTERMIND") == 0x0C413492u &&
                    AssetId("bk_health_cntr") == 0x78C70B3Au,
                "asset IDs");
        const uint32_t knight = AssetId("BLACK_KNIGHT"), counter = AssetId("BOSS_BATTLE_OVER_CNTR");
        Require(BossForEvent(counter, 20, knight, kEventKill)->boss == Boss::BlackKnight, "killed");
        Require(BossForEvent(AssetId("REDBEARD"), kEventDie, AssetId("FREE_FRED_TRIGGER"), 1)->boss == Boss::Redbeard,
                "raises its death event");
        Require(BossForEvent(knight, kEventDie, knight, kEventDie)->boss == Boss::BlackKnight, "to itself");
        Require(BossForEvent(counter, 20, knight, kEventHurt) == nullptr, "hurt isn't a death");
        Require(BossForEvent(knight, 0, knight, kEventKill) == nullptr, "a kill it sends itself counts only if it dies");
        Require(BossForEvent(knight, 50, counter, kEventKill) == nullptr, "killing something else");
        Require(BossForEvent(counter, kEventDie, knight, 12) == nullptr, "something else dying");

        // A boss splits at its death; its movie later doesn't split again.
        {
            Game g;
            g.NewGame();
            g.Run(30);
            g.Die(Boss::BlackKnight, 8);
            Require(g.tracker.Splits().size() == 1 && g.tracker.Splits()[0].name == "Black Knight", "death split");
            Near(g.tracker.Splits()[0].realTime, 33.0, 0.05, "at the death");
            g.Movie("cin3.bik", 5);
            g.Die(Boss::BlackKnight, 1);
            Require(g.tracker.Splits().size() == 1, "once per boss");
            g.Die(Boss::Mastermind, 1);
            Require(g.tracker.Finished() && g.tracker.Splits().back().name == "Mastermind", "his death ends Any%");
            Near(g.tracker.RealTime(g.now), 47.0, 0.05, "the run ends at his death");
        }
        // Several in one sample (a kill and the death event it causes are the same boss).
        {
            Game h;
            h.tracker.SetCategory(CategoryIndex("all-bosses"));
            h.NewGame();
            h.Run(1, {}, (1u << unsigned(Boss::Redbeard)) | (1u << unsigned(Boss::GreenGhost)));
            Require(h.tracker.Splits().size() == 2 && h.tracker.Running(), "two bosses, run goes on");
            h.Die(Boss::BlackKnight, 1);
            h.Die(Boss::Mastermind, 1);
            Require(h.tracker.Finished(), "all four end All Bosses");
        }

        // Categories.
        std::set<std::string> ids;
        for (const Category& c : kCategories) ids.insert(c.id);
        Require(ids.size() == kCategories.size(), "category ids unique");
        Require(CategoryIndex("amt") == 3 && CategoryIndex("nope") == 0, "category by id");

        // Any%: starts at the press, a boss splits, the Mastermind's movie ends it.
        {
            Game g;
            g.NewGame();
            const double pressAt = 1.0;  // the press came 1 s into the title
            Require(g.tracker.Running(), "New Game starts a run");
            Near(g.tracker.StartTime(), pressAt, 0.02, "the run starts at the press");
            Near(g.tracker.RealTime(g.now), 3.0, 0.03, "fade + load in real time");
            Near(g.tracker.WithoutLoads(), 2.0, 0.03, "the fade isn't a load, the level load is");
            Require(g.tracker.PlayKnown(), "a new game has a play time");
            Near(g.tracker.PlaySeconds(), 0.0, 0.001, "and it starts at 0 (loading)");
            g.Run(60);
            Near(g.tracker.PlaySeconds(), 60.0, 0.03, "a minute of play");
            g.Movie("cin5.bik", 20);
            Require(g.tracker.Splits().size() == 1 && g.tracker.Splits()[0].name == "Green Ghost", "boss split");
            Near(g.tracker.Splits()[0].realTime, 63.0, 0.05, "split time");
            Near(g.tracker.PlaySeconds(), 60.0, 0.03, "the movie isn't play time");
            g.Movie("cin5.bik", 1);  // the same movie again
            Require(g.tracker.Splits().size() == 1, "a boss splits once");
            g.gallery = true;
            g.Movie("cin_mmpd.bik", 1);  // watched in the Monster Gallery
            g.gallery = false;
            Require(g.tracker.Running() && g.tracker.Splits().size() == 1, "gallery movies don't split");
            g.Set(kModeInPlay, 4);  // a scene switch
            g.Run(0.5);
            g.Set(kModeInPlay, 5);
            g.Run(1.5);
            g.Set(kModePause, 0);
            g.Run(10);
            g.Set(kModeInPlay, 1);
            g.Run(30);
            g.Movie("cin_mmpd.bik", 5);
            Require(g.tracker.Finished() && !g.tracker.Running(), "the Mastermind ends Any%");
            const auto& last = g.tracker.Splits().back();
            Require(g.tracker.Splits().size() == 2 && last.name == "Mastermind", "the end is the Mastermind's split");
            const double total = 3.0 + 60 + 20 + 1 + 1 + 2 + 10 + 30;
            Near(g.tracker.RealTime(g.now), total, 0.1, "final real time");
            Near(g.tracker.RealTime(g.now + 100), total, 0.1, "frozen after the end");
            Near(g.tracker.WithoutLoads(), total - 1.0 - 2.0, 0.1, "final time without loads");
            Near(last.realTime, total, 0.1, "the last split is the end");
            Near(g.tracker.PlaySeconds(), 60 + 10 + 30, 0.1, "play time: playing and pause");
        }

        // No press seen (a scripted start): the run starts when the game leaves the menu.
        {
            Game g;
            g.NewGame(2.0, false);
            Near(g.tracker.RealTime(g.now), 1.0, 0.03, "no press: from the transition");
        }

        // A save loaded from the load menu: no run, its play time carries on.
        {
            Game g;
            g.Set(kModeLoadGame, 0);
            g.Run(1);
            Require(!g.tracker.PlayKnown(), "no play time before a save");
            g.tracker.SaveLoaded(3600);
            g.Set(kModeInPlay, 0);
            g.Run(1);
            g.Set(kModeInPlay, 1);
            g.Run(30);
            Require(!g.tracker.Started(), "loading a save doesn't start a run");
            Near(g.tracker.PlaySeconds(), 3630, 0.05, "play time carries on from the save's");
            g.now += 30;  // the PC slept
            g.Run(1);
            Near(g.tracker.PlaySeconds(), 3631, 0.05, "a long gap isn't play time");
        }

        // The category is the one chosen before New Game; a change during a run waits for the next one.
        {
            Game g;
            g.tracker.SetCategory(CategoryIndex("all-bosses"));
            g.NewGame();
            g.tracker.SetCategory(CategoryIndex("any"));
            Require(g.tracker.RunCategory() == CategoryIndex("all-bosses"), "the run keeps its category");
            g.Movie("cin_mmpd.bik", 1);
            Require(g.tracker.Running(), "All Bosses goes on after the Mastermind");
            g.Movie("cin7.bik", 1);
            g.Movie("cin5.bik", 1);
            Require(g.tracker.Running(), "three of four");
            g.Movie("cin3.bik", 1);
            Require(g.tracker.Finished() && g.tracker.Splits().size() == 4, "the fourth boss ends it");
            Require(g.tracker.Splits().back().name == "Black Knight", "ends on the Black Knight");
            g.NewGame();
            Require(g.tracker.Running() && g.tracker.RunCategory() == CategoryIndex("any") &&
                        g.tracker.Splits().empty(), "the next New Game starts over with the new category");
        }

        // All Monster Tokens: a split per token; the Mastermind and the 21st token, whichever is last.
        {
            Game g;
            g.tokens = 12;  // left over from a save loaded earlier
            g.tracker.SetCategory(CategoryIndex("amt"));
            g.NewGame();
            g.tokens = 0;  // the game resets it
            g.Run(1);
            Require(g.tracker.Splits().empty(), "no split for the reset");
            g.tokens = 1;
            g.Run(1);
            Require(g.tracker.Splits().size() == 1 && g.tracker.Splits()[0].name == "Token 1 (Shock on the Dock!)",
                    "token split: " + (g.tracker.Splits().empty() ? std::string("none") : g.tracker.Splits()[0].name));
            g.tokens = 20;
            g.Run(1);
            Require(g.tracker.Splits().size() == 20, "a split for each token");
            g.Movie("cin_mmpd.bik", 1);
            Require(g.tracker.Running(), "the Mastermind with 20 tokens");
            g.tokens = 21;
            g.Run(1);
            Require(g.tracker.Finished(), "the 21st token ends it");
            Require(g.tracker.Splits().back().name == "Token 21 (Shock on the Dock!)", "ends on the token");
        }

        // 100%: the Mastermind, then 100% completion.
        {
            Game g;
            g.tracker.SetCategory(CategoryIndex("100"));
            g.NewGame();
            g.percent = 95;
            g.Movie("cin_mmpd.bik", 1);
            Require(g.tracker.Running(), "95% at the Mastermind");
            g.percent = 100;
            g.Run(1);
            Require(g.tracker.Finished() && g.tracker.Splits().back().name == "100%", "100% ends it");
        }

        // Formatting.
        Require(FormatTime(3.456) == "3.45", "seconds: " + FormatTime(3.456));
        Require(FormatTime(63.5) == "1:03.50", "minutes: " + FormatTime(63.5));
        Require(FormatTime(3723.01) == "1:02:03.01", "hours: " + FormatTime(3723.01));
        Require(FormatTime(3723.9, false) == "1:02:03", "no hundredths");
        Require(FormatTime(-1) == "0.00", "negative");
        Require(FormatPlayTime(30) == "less than a minute" && FormatPlayTime(12 * 60 + 5) == "12 min" &&
                    FormatPlayTime(3 * 3600 + 5 * 60) == "3 h 05 min", "play time words");
        Require(FormatClock(12.9) == "0:00:12" && FormatClock(3 * 3600 + 5 * 60 + 9) == "3:05:09", "clock");
        Require(LssTime(3723.45) == "01:02:03.4500000", "LiveSplit time: " + LssTime(3723.45));

        // Run files.
        const std::vector<Split> splits = {{"Green Ghost", 63.0, 62.0}, {"Mastermind, at last", 126.5, 124.0}};
        const std::string csv = RunCsv(splits);
        Require(csv == "Split,Real time,Without loads,Segment (real time),Segment (without loads)\n"
                       "Green Ghost,1:03.00,1:02.00,1:03.00,1:02.00\n"
                       "\"Mastermind, at last\",2:06.50,2:04.00,1:03.50,1:02.00\n",
                "CSV:\n" + csv);
        const std::string lss = RunLss({{"A & B", 63.0, 62.0}}, "Any%", 0, 120);
        Require(lss.find("<CategoryName>Any%</CategoryName>") != std::string::npos, "lss category");
        Require(lss.find("<Name>A &amp; B</Name>") != std::string::npos, "lss name escaped");
        Require(lss.find("<RealTime>00:01:03.0000000</RealTime>") != std::string::npos, "lss split time");
        Require(lss.find("started=\"01/01/1970 00:00:00\"") != std::string::npos, "lss date");

        // The play-time file.
        auto entries = ParsePlayTimes("# comment\nScoobyDoo00 = 3725.4\r\nbad line\nScoobyDoo02=-5\nScoobyDoo03 = 10\n");
        Require(entries.size() == 2 && entries[0].slot == "ScoobyDoo00" && entries[1].slot == "ScoobyDoo03", "parse");
        Near(FindPlayTime(entries, "ScoobyDoo00"), 3725.4, 1e-9, "find");
        Near(FindPlayTime(entries, "ScoobyDoo07"), 0, 1e-9, "a slot without an entry is 0");
        SetPlayTime(entries, "ScoobyDoo00", 4000);
        SetPlayTime(entries, "ScoobyDoo05", 12.34);
        const auto again = ParsePlayTimes(FormatPlayTimes(entries));
        Require(again.size() == 3, "round trip");
        Near(FindPlayTime(again, "ScoobyDoo00"), 4000, 0.05, "updated");
        Near(FindPlayTime(again, "ScoobyDoo05"), 12.3, 0.05, "added");

        // Memory card calls.
        Require(IsSaveSlot("ScoobyDoo00") && IsSaveSlot("ScoobyDoo07") && !IsSaveSlot("ScoobyDoo") &&
                    !IsSaveSlot("ScoobyDooAB") && !IsSaveSlot(""), "save slot names");
        Require(IsSaveLoad(16384, 24576) && !IsSaveLoad(16384, 512) && !IsSaveLoad(0, 15872), "a whole save's read");
        Require(IsSaveWrite(40960) && !IsSaveWrite(512), "a whole save's write");

        Require(FileUrl("D:\\My Documents\\Saves") == "file:///D:/My%20Documents/Saves", "Windows URL: " +
                    FileUrl("D:\\My Documents\\Saves"));
        Require(FileUrl("/home/a b/#1") == "file:///home/a%20b/%231", "Linux URL");
    } catch (const std::exception& e) {
        std::cerr << "no100f_speedrun_tests: " << e.what() << "\n";
        return 1;
    }
    std::cout << "no100f_speedrun_tests: ok\n";
    return 0;
}
