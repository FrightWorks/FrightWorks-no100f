// Old-save conversion (old_save_convert.h): real files in a scratch folder, no game. Covers old only, collision,
// a run stopped or failing at each of the 4 steps and then run again, running twice, a path the ANSI code page
// cannot hold, the stuck file (name right, header wrong) and files that must be left alone.
#include "old_save_convert.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using OldSaveConvert::Hooks;
using OldSaveConvert::Step;
using OldSaveConvert::Summary;
using Bytes = std::vector<uint8_t>;

static void Require(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}

static const char* kGame = "GIHE";

// A .gci: 0x40-byte entry (game 0-3, maker 4-5, 0xFF, banner flags, name 8-39) + body.
static Bytes MakeGci(const char* game, const char* maker, const std::string& name, uint8_t seed, size_t body = 8192) {
    Bytes b(0x40 + body, 0);
    std::memcpy(&b[0], game, 4);
    std::memcpy(&b[4], maker, 2);
    b[6] = 0xFF;
    std::memcpy(&b[8], name.data(), std::min<size_t>(name.size(), 32));
    for (size_t i = 0x40; i < b.size(); ++i) b[i] = uint8_t(seed * 31 + i * 7);
    return b;
}

static void Put(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    o.close();
    Require(bool(o), "fixture write failed");
}

static Bytes Get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    Require(bool(i), "fixture read failed: " + OldSaveConvert::detail::Utf8(p));
    return Bytes((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

// Every file under the roots, relative path -> bytes.
static std::map<std::string, Bytes> Snapshot(const fs::path& root) {
    std::map<std::string, Bytes> m;
    if (!fs::exists(root)) return m;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto rel = e.path().lexically_relative(root);
        m[OldSaveConvert::detail::Utf8(rel)] = Get(e.path());
    }
    return m;
}

struct Env {
    fs::path base, cards, backup, a, b;
    explicit Env(const fs::path& base_) : base(base_) {
        cards = base / "Saves" / "MemoryCard";
        backup = base / "Saves" / "MemoryCard-old";
        a = cards / "USA" / "Card A";
        b = cards / "USA" / "Card B";
        fs::remove_all(base);
        fs::create_directories(a);
        fs::create_directories(b);
    }
    Summary Run(const Hooks& h = {}) const { return OldSaveConvert::Run(cards, backup, kGame, h); }
};

static const Bytes kOld = MakeGci("G5DE", "78", "ScoobyDoo00", 1);
static const Bytes kNew = MakeGci("GIHE", "78", "ScoobyDoo00", 2);
static Bytes Fixed(const Bytes& b) { Bytes f = b; std::memcpy(&f[0], kGame, 4); return f; }

static const char* kOldName = "78-G5DE-ScoobyDoo00.gci";
static const char* kNewName = "78-GIHE-ScoobyDoo00.gci";
static const char* kOldBackup = "USA/Card A/78-G5DE-ScoobyDoo00.gci.bak";

static void ExpectNoGciInBackup(const Env& e) {
    if (!fs::exists(e.backup)) return;
    for (const auto& f : fs::recursive_directory_iterator(e.backup))
        Require(f.path().extension() != ".gci", "backup file ends in .gci: " + OldSaveConvert::detail::Utf8(f.path()));
}

static void ExpectNoTemp(const Env& e) {
    for (const auto& f : fs::recursive_directory_iterator(e.cards))
        Require(f.path().extension() != ".tmp", "temp file left: " + OldSaveConvert::detail::Utf8(f.path()));
}

static void OldOnly(const fs::path& base) {
    Env e(base);
    Put(e.a / kOldName, kOld);
    const Summary s = e.Run();
    Require(s.converted == 1 && s.failed == 0 && !s.killed, "old only: one conversion");
    Require(!fs::exists(e.a / kOldName), "old only: original removed");
    Require(Get(e.a / kNewName) == Fixed(kOld), "old only: header code replaced, body intact");
    Require(Get(e.backup / kOldBackup) == kOld, "old only: backup is the original");
    ExpectNoGciInBackup(e);
    ExpectNoTemp(e);
    Require(!fs::exists(e.cards / "USA" / "Card A" / "x"), "sanity");
    // The backup is outside the card folders.
    Require(e.backup.lexically_relative(e.cards).begin()->string() == "..", "backup outside MemoryCard");
}

static void Collision(const fs::path& base) {
    Env e(base);
    Put(e.a / kOldName, kOld);
    Put(e.a / kNewName, kNew);
    const Summary s = e.Run();
    Require(s.collided == 1 && s.converted == 0 && s.failed == 0, "collision: counted");
    Require(Get(e.a / kNewName) == kNew, "collision: right-code save untouched");
    Require(!fs::exists(e.a / kOldName), "collision: old removed from card folder");
    Require(Get(e.backup / kOldBackup) == kOld, "collision: old kept in backup");
    ExpectNoGciInBackup(e);
}

static void NotOurs(const fs::path& base) {
    Env e(base);
    std::map<std::string, Bytes> want;
    auto add = [&](const fs::path& p, const Bytes& b) { Put(p, b); want[OldSaveConvert::detail::Utf8(p.lexically_relative(e.base))] = b; };
    add(e.a / "78-GIHE-ScoobyDoo00.gci", kNew);                                   // already right
    add(e.a / "01-G5DE-Other.gci", MakeGci("G5DE", "01", "Other", 3));            // other maker and name
    add(e.a / "78-ABCD-NotScooby.gci", MakeGci("ABCD", "78", "NotScooby", 4));    // right maker, other name
    add(e.a / "01-ABCD-ScoobyDoo01.gci", MakeGci("ABCD", "01", "ScoobyDoo01", 5));// right name, other maker
    add(e.a / "78-ABCD-ScoobyDoo1.gci", MakeGci("ABCD", "78", "ScoobyDoo1", 6));  // one digit
    add(e.a / "78-ABCD-ScoobyDoo001.gci", MakeGci("ABCD", "78", "ScoobyDoo001", 7));  // three digits
    add(e.a / "short.gci", Bytes(10, 1));                                          // too short for a header
    add(e.a / "78-G5DE-ScoobyDoo03.gci.bak", MakeGci("G5DE", "78", "ScoobyDoo03", 8));  // not a .gci
    add(e.cards / "stray" / "78-G5DE-ScoobyDoo04.gci", MakeGci("G5DE", "78", "ScoobyDoo04", 9));  // not in a card folder
    const Summary s = e.Run();
    Require(s.converted + s.repaired + s.collided + s.failed == 0, "not ours: nothing done");
    Require(Snapshot(e.base) == want, "not ours: tree unchanged, no backup folder");
}

// Any other code is ours too (rule by header), as long as it differs from the booted game's.
static void OtherCode(const fs::path& base) {
    Env e(base);
    Put(e.a / "odd.gci", MakeGci("ZZZZ", "78", "ScoobyDoo07", 10));
    Put(e.b / kOldName, MakeGci("G5DE", "78", "ScoobyDoo01", 11));
    const Summary s = e.Run();
    Require(s.converted == 2, "other code: both converted");
    Require(Get(e.a / "78-GIHE-ScoobyDoo07.gci") == Fixed(MakeGci("ZZZZ", "78", "ScoobyDoo07", 10)), "other code: slot A");
    Require(Get(e.b / "78-GIHE-ScoobyDoo01.gci") == Fixed(MakeGci("G5DE", "78", "ScoobyDoo01", 11)), "other code: slot B");
    Require(!fs::exists(e.a / "odd.gci"), "other code: odd name removed");
}

// Snapshot of the finished state to compare restarts against.
static std::map<std::string, Bytes> CleanRun(const fs::path& base, const std::function<void(const Env&)>& setup) {
    Env e(base);
    setup(e);
    e.Run();
    return Snapshot(e.base);
}

// Stop (kill) after step n, check nothing was lost, then run again: same end state as an uninterrupted run.
// Also fail step n instead: the file stays as it was (no temp), and the next run finishes.
static void StepsThenAgain(const fs::path& base, const char* label, const std::function<void(const Env&)>& setup,
                           const std::function<void(const Env&, Step)>& checkAfterKill) {
    const auto want = CleanRun(base, setup);
    for (Step st : {Step::Backup, Step::WriteTemp, Step::Rename, Step::RemoveOriginal}) {
        {
            Env e(base);
            setup(e);
            Hooks h;
            h.killAfter = [&](Step s) { return s == st; };
            const Summary s = e.Run(h);
            // Not every step exists on every path (a collision skips 2 and 3, a repair skips 4).
            if (s.killed) {
                checkAfterKill(e, st);
                const Summary s2 = e.Run();
                Require(!s2.killed && s2.failed == 0, std::string(label) + ": rerun after kill clean");
                Require(Snapshot(e.base) == want,
                        std::string(label) + ": end state after kill at step " + std::to_string(int(st)) + " differs");
                const auto once = Snapshot(e.base);
                const Summary s3 = e.Run();
                Require(s3.converted + s3.repaired + s3.collided + s3.failed == 0 && Snapshot(e.base) == once,
                        std::string(label) + ": third run changes nothing");
            } else {
                Require(Snapshot(e.base) == want, std::string(label) + ": step " + std::to_string(int(st)) + " not reached but end state differs");
            }
        }
        {
            Env e(base);
            setup(e);
            const auto before = Snapshot(e.cards);
            Hooks h;
            h.failStep = [&](Step s) { return s == st; };
            const Summary s = e.Run(h);
            if (s.failed) {
                ExpectNoTemp(e);
                // Steps 1-3 failing leave the card folder exactly as it was; a failed removal leaves the new file too.
                if (st != Step::RemoveOriginal)
                    Require(Snapshot(e.cards) == before, std::string(label) + ": failed step " + std::to_string(int(st)) + " left the card folder as it was");
                const Summary s2 = e.Run();
                Require(s2.failed == 0 && Snapshot(e.base) == want, std::string(label) + ": rerun after failed step " + std::to_string(int(st)) + " finishes");
            } else {
                Require(Snapshot(e.base) == want, std::string(label) + ": step not reached but end state differs");
            }
        }
    }
}

static void RunTwice(const fs::path& base) {
    Env e(base);
    Put(e.a / kOldName, kOld);
    Put(e.b / kOldName, MakeGci("G5DE", "78", "ScoobyDoo02", 12));
    Require(e.Run().converted == 2, "twice: first run converts");
    const auto once = Snapshot(e.base);
    const Summary s = e.Run();
    Require(s.converted + s.repaired + s.collided + s.failed == 0 && !s.killed, "twice: second run has nothing to do");
    Require(Snapshot(e.base) == once, "twice: second run changes nothing");
}

static void StuckFile(const fs::path& base) {
    Env e(base);
    Put(e.a / kNewName, kOld);  // named for the right code, header still the old one
    const Summary s = e.Run();
    Require(s.repaired == 1 && s.failed == 0, "stuck: repaired");
    Require(Get(e.a / kNewName) == Fixed(kOld), "stuck: header fixed in place");
    Require(Get(e.backup / "USA/Card A/78-GIHE-ScoobyDoo00.gci.bak") == kOld, "stuck: backup holds the stuck bytes");
    ExpectNoTemp(e);
    ExpectNoGciInBackup(e);
}

// A stuck file and an old-named file for the same slot: the repaired one is the right-code save, the other collides.
static void StuckAndOld(const fs::path& base) {
    Env e(base);
    const Bytes stuck = MakeGci("G5DE", "78", "ScoobyDoo00", 20);
    Put(e.a / kNewName, stuck);
    Put(e.a / kOldName, kOld);
    const Summary s = e.Run();
    Require(s.repaired == 1 && s.collided == 1 && s.failed == 0, "stuck+old: one repaired, one collided");
    Require(Get(e.a / kNewName) == Fixed(stuck), "stuck+old: the stuck file wins");
    Require(!fs::exists(e.a / kOldName), "stuck+old: old removed");
    Require(Get(e.backup / kOldBackup) == kOld, "stuck+old: old backed up");
}

// A different file already sits at the backup name: it is kept and the new backup gets another name.
static void BackupNameTaken(const fs::path& base) {
    Env e(base);
    Put(e.a / kOldName, kOld);
    const Bytes other = MakeGci("G5DE", "78", "ScoobyDoo00", 99);
    Put(e.backup / kOldBackup, other);
    Require(e.Run().converted == 1, "backup taken: converted");
    Require(Get(e.backup / kOldBackup) == other, "backup taken: earlier backup untouched");
    bool found = false;
    for (const auto& f : fs::recursive_directory_iterator(e.backup))
        if (f.is_regular_file() && f.path() != e.backup / kOldBackup && Get(f.path()) == kOld) found = true;
    Require(found, "backup taken: new backup under another name");
}

// Backup cannot be made (a file where its folder should go): nothing in the card folders changes.
static void BackupBlocked(const fs::path& base) {
    Env e(base);
    Put(e.a / kOldName, kOld);
    Put(e.backup, Bytes{1, 2, 3});  // "MemoryCard-old" is a file
    const Summary s = e.Run();
    Require(s.failed == 1 && s.converted == 0, "backup blocked: fails");
    Require(Get(e.a / kOldName) == kOld && !fs::exists(e.a / kNewName), "backup blocked: original alone");
}

// Missing folders are fine.
static void NoFolders(const fs::path& base) {
    fs::remove_all(base);
    fs::create_directories(base);
    Require(OldSaveConvert::Run(base / "none", base / "none-old", kGame).converted == 0, "no folder");
    fs::create_directories(base / "empty");
    Require(OldSaveConvert::Run(base / "empty", base / "empty-old", kGame).converted == 0, "empty folder");
}

int main() {
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path root = fs::temp_directory_path() / ("old-save-tests-" + stamp);
    // Not representable in the ANSI code page of a Western PC: Cyrillic, CJK, accents.
    const fs::path odd = root / fs::path(u8"Пример-日本語-é");
    int failures = 0;
    auto run = [&](const char* name, const std::function<void()>& fn) {
        try {
            fn();
            std::cout << "ok   " << name << "\n";
        } catch (const std::exception& ex) {
            std::cout << "FAIL " << name << ": " << ex.what() << "\n";
            ++failures;
        }
    };
    run("old only", [&] { OldOnly(root / "t1"); });
    run("collision", [&] { Collision(root / "t2"); });
    run("not ours", [&] { NotOurs(root / "t3"); });
    run("other code", [&] { OtherCode(root / "t4"); });
    run("run twice", [&] { RunTwice(root / "t5"); });
    run("stuck file", [&] { StuckFile(root / "t6"); });
    run("stuck and old", [&] { StuckAndOld(root / "t7"); });
    run("backup name taken", [&] { BackupNameTaken(root / "t8"); });
    run("backup blocked", [&] { BackupBlocked(root / "t9"); });
    run("no folders", [&] { NoFolders(root / "t10"); });
    run("non-ANSI path: old only", [&] { OldOnly(odd / "oldonly"); });
    run("non-ANSI path: collision", [&] { Collision(odd / "collision"); });
    run("non-ANSI path: stuck", [&] { StuckFile(odd / "stuck"); });

    run("steps: old only", [&] {
        StepsThenAgain(root / "s1", "old only", [](const Env& e) { Put(e.a / kOldName, kOld); },
                       [](const Env& e, Step st) {
                           Require(Get(e.backup / kOldBackup) == kOld, "killed: backup present");
                           if (st == Step::Backup) Require(Get(e.a / kOldName) == kOld && !fs::exists(e.a / kNewName), "killed after 1: original only");
                           if (st == Step::WriteTemp) Require(Get(e.a / kOldName) == kOld && !fs::exists(e.a / kNewName), "killed after 2: original only, temp aside");
                           if (st == Step::Rename) Require(Get(e.a / kOldName) == kOld && Get(e.a / kNewName) == Fixed(kOld), "killed after 3: both");
                           if (st == Step::RemoveOriginal) Require(!fs::exists(e.a / kOldName) && Get(e.a / kNewName) == Fixed(kOld), "killed after 4: new only");
                       });
    });
    run("steps: collision", [&] {
        StepsThenAgain(root / "s2", "collision", [](const Env& e) { Put(e.a / kOldName, kOld); Put(e.a / kNewName, kNew); },
                       [](const Env& e, Step st) {
                           Require(Get(e.a / kNewName) == kNew, "killed: right-code save untouched");
                           Require(Get(e.backup / kOldBackup) == kOld, "killed: backup present");
                           if (st == Step::Backup) Require(Get(e.a / kOldName) == kOld, "killed after 1: old still there");
                           if (st == Step::RemoveOriginal) Require(!fs::exists(e.a / kOldName), "killed after 4: old gone");
                       });
    });
    run("steps: stuck file", [&] {
        StepsThenAgain(root / "s3", "stuck", [](const Env& e) { Put(e.a / kNewName, kOld); },
                       [](const Env& e, Step st) {
                           Require(Get(e.backup / "USA/Card A/78-GIHE-ScoobyDoo00.gci.bak") == kOld, "killed: backup present");
                           if (st == Step::Backup || st == Step::WriteTemp) Require(Get(e.a / kNewName) == kOld, "killed early: stuck file as it was");
                           if (st == Step::Rename) Require(Get(e.a / kNewName) == Fixed(kOld), "killed after 3: fixed");
                       });
    });
    run("steps: non-ANSI path", [&] {
        StepsThenAgain(odd / "steps", "non-ANSI", [](const Env& e) { Put(e.a / kOldName, kOld); }, [](const Env&, Step) {});
    });

    fs::remove_all(root);
    if (failures) {
        std::cout << failures << " failed\n";
        return 1;
    }
    std::cout << "all old save conversion tests passed\n";
    return 0;
}
