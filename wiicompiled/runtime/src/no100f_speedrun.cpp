#include "no100f_speedrun.h"

#include "hle/no100f_xbox_layout.h"
#include "memory.h"
#include "no100f_discord_status.h"
#include "no100f_game_state.h"
#include "no100f_progress.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace no100f_speedrun {
namespace {

std::mutex g_mutex;
Tracker g_tracker;
bool g_categoryRead = false;
double g_runStart = -1;      // the run whose files were written (or are due): its start time
bool g_runWritten = false;
std::string g_lastRunFile;
bool g_playTimesRead = false;
std::vector<PlayTimeEntry> g_playTimes;
uint16_t g_lastButtons = 0;
uint32_t g_lastMovie = 0;
std::atomic<uint32_t> g_bossesDied{0};  // bit (1 << Boss), set by EntityEvent, taken by Tick

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path PlayTimePath() { return RuntimeConfigFile::SavesDirectory() / "FrightWorks" / "play-time.ini"; }
std::filesystem::path RunsPath() { return RuntimeConfigFile::SavesDirectory() / "Speedruns"; }

// Caller holds g_mutex.
void ReadPlayTimes() {
    if (g_playTimesRead) return;
    g_playTimesRead = true;
    std::ifstream in(PlayTimePath(), std::ios::binary);
    if (!in) return;
    std::stringstream text;
    text << in.rdbuf();
    g_playTimes = ParsePlayTimes(text.str());
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.flush()) return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

// Caller holds g_mutex. A finished run's CSV and LiveSplit splits, named by its start time and category.
void WriteRunFiles(double now) {
    if (!RuntimeConfigFile::SpeedrunSaveRuns()) return;
    const Category& category = kCategories[size_t(g_tracker.RunCategory())];
    const std::time_t ended = std::time(nullptr);
    const std::time_t started = ended - static_cast<std::time_t>(now - g_tracker.StartTime());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &started);
#else
    localtime_r(&started, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &local);
    const std::string base = std::string(stamp) + " " + category.name;
    const std::filesystem::path csv = RunsPath() / (base + ".csv");
    const bool ok = WriteText(csv, RunCsv(g_tracker.Splits())) &&
                    WriteText(RunsPath() / (base + ".lss"), RunLss(g_tracker.Splits(), category.name, started, ended));
    if (ok) g_lastRunFile = RuntimeConfigFile::PathToUtf8(csv);
    RT_LOGF(RT_TAG_RUNTIME, "[speedrun] %s finished in %s (%s without loads)%s\n", category.name,
            FormatTime(g_tracker.RealTime(now)).c_str(), FormatTime(g_tracker.WithoutLoads()).c_str(),
            ok ? ", splits saved" : ", couldn't save the splits");
}

}  // namespace

void Tick() {
    namespace gs = no100f_game_state;
    Frame f;
    f.now = Now();
    f.layout = gs::g_diagnosticLayoutVerified.load(std::memory_order_relaxed);
    uint32_t movie = 0;
    if (f.layout && Memory::TryRead32(gs::kModeAddress, f.mode) && Memory::TryRead32(gs::kStateAddress, f.state) &&
        Memory::TryRead32(gs::kMovieAddress, movie)) {
        f.movie = movie != 0;
        if (movie != 0 && movie != g_lastMovie) f.movieStarted = gs::LastMovieName();
        g_lastMovie = movie;
        if (const no100f_progress::Progress progress = no100f_progress::Latest(); progress.valid) {
            f.progressKnown = true;
            f.percent = progress.percent;
            f.tokens = progress.inputs.tokens;
        }
        uint32_t scenePtr = 0, scene = 0;
        if (Memory::TryRead32(No100fXboxLayout::kScenePointer, scenePtr) && scenePtr != 0 &&
            Memory::TryRead32(scenePtr + No100fXboxLayout::kSceneIdOffset, scene)) {
            if (const auto* level = no100f_discord_status::FindLevel(scene)) f.place = level->name;
            f.gallery = scene == no100f_discord_status::SceneId("H003");
        }
    } else {
        f.layout = false;
    }
    f.bossesDied = g_bossesDied.exchange(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_categoryRead) {
        g_categoryRead = true;
        g_tracker.SetCategory(CategoryIndex(RuntimeConfigFile::SpeedrunCategory()));
    }
    const bool wasFinished = g_tracker.Finished();
    const size_t splitsBefore = g_tracker.Started() ? g_tracker.Splits().size() : 0;
    g_tracker.Update(f);
    for (size_t i = splitsBefore; g_tracker.Started() && i < g_tracker.Splits().size(); ++i) {
        const Split& split = g_tracker.Splits()[i];
        RT_LOGF(RT_TAG_RUNTIME, "[speedrun] split %s at %s (%s without loads)\n", split.name.c_str(),
                FormatTime(split.realTime).c_str(), FormatTime(split.withoutLoads).c_str());
    }
    if (g_tracker.Started() && g_tracker.StartTime() != g_runStart) {
        g_runStart = g_tracker.StartTime();
        g_runWritten = false;
        g_lastRunFile.clear();
        RT_LOGF(RT_TAG_RUNTIME, "[speedrun] New Game: %s run started\n",
                kCategories[size_t(g_tracker.RunCategory())].name);
    }
    if (g_tracker.Finished() && !wasFinished && !g_runWritten) {
        g_runWritten = true;
        WriteRunFiles(f.now);
    }
}

void EntityEvent(uint32_t fromId, uint32_t fromEvent, uint32_t toId, uint32_t toEvent) {
    const BossInfo* died = BossForEvent(fromId, fromEvent, toId, toEvent);
    if (died) g_bossesDied.fetch_or(1u << unsigned(died->boss), std::memory_order_relaxed);
    const BossInfo* boss = died ? died : BossById(toId);
    if (!boss) return;
    RT_LOGF(RT_TAG_RUNTIME, "[boss] %s gets event %u (%08X raised %u)%s\n", boss->name, toEvent, fromId, fromEvent,
            died ? ": died" : "");
}

void PadButtons(uint16_t buttons) {
    constexpr uint16_t kConfirm = 0x0100 /* A */ | 0x1000 /* Start */;
    const uint16_t pressed = buttons & ~g_lastButtons & kConfirm;
    g_lastButtons = buttons;
    if (!pressed) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tracker.ConfirmPressed(Now());
}

void CardRead(const std::string& file, int32_t offset, int32_t length) {
    if (!IsSaveSlot(file) || !IsSaveLoad(offset, length)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ReadPlayTimes();
    const double seconds = FindPlayTime(g_playTimes, file);
    g_tracker.SaveLoaded(seconds);
    RT_LOGF(RT_TAG_RUNTIME, "[speedrun] loaded %s: play time %s\n", file.c_str(), FormatClock(seconds).c_str());
}

void CardWritten(const std::string& file, int32_t length) {
    if (!IsSaveSlot(file) || !IsSaveWrite(length)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_tracker.PlayKnown()) return;
    ReadPlayTimes();
    SetPlayTime(g_playTimes, file, g_tracker.PlaySeconds());
    const bool ok = WriteText(PlayTimePath(), FormatPlayTimes(g_playTimes));
    RT_LOGF(RT_TAG_RUNTIME, "[speedrun] saved %s: play time %s%s\n", file.c_str(),
            FormatClock(g_tracker.PlaySeconds()).c_str(), ok ? "" : " (couldn't write the play-time file)");
}

void SetNextCategory(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_categoryRead = true;
    g_tracker.SetCategory(index);
}

std::optional<double> PlaySeconds() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_tracker.PlayKnown()) return std::nullopt;
    return g_tracker.PlaySeconds();
}

View CurrentView(size_t splits) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_categoryRead) {
        g_categoryRead = true;
        g_tracker.SetCategory(CategoryIndex(RuntimeConfigFile::SpeedrunCategory()));
    }
    View view;
    view.started = g_tracker.Started();
    view.running = g_tracker.Running();
    view.finished = g_tracker.Finished();
    view.category = kCategories[size_t(view.started ? g_tracker.RunCategory() : g_tracker.NextCategory())].name;
    view.realTime = g_tracker.RealTime(Now());
    view.withoutLoads = g_tracker.WithoutLoads();
    const auto& all = g_tracker.Splits();
    const size_t first = all.size() > splits ? all.size() - splits : 0;
    view.lastSplits.assign(all.begin() + static_cast<std::ptrdiff_t>(first), all.end());
    view.playKnown = g_tracker.PlayKnown();
    view.playSeconds = g_tracker.PlaySeconds();
    view.lastRunFile = g_lastRunFile;
    return view;
}

std::string RunsFolder() {
    std::error_code ec;
    std::filesystem::create_directories(RunsPath(), ec);
    return RuntimeConfigFile::PathToUtf8(RunsPath());
}

}  // namespace no100f_speedrun
