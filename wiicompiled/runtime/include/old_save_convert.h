// Old-save conversion for the memory card folders. Pure filesystem logic: no guest memory, no Aurora, so a CTest
// unit test drives it (tests/old_save_convert_tests.cpp); card_gc.cpp calls it once at game start.
//
// A .gci in a card folder ("Card A" / "Card B") whose header says internal name "ScoobyDoo" + 2 digits, maker "78"
// and a game code other than the booted game's is one of ours, filed under the wrong code by a pre-release build.
// The game's own card check (GIHE / "78") rejects it, so the save is invisible until the header is fixed. No game
// code is named here: the rule is "this game's save, wrong code".
//
// Per file, in this order (Step):
//   1 Backup:         copy the original to <backupDir>/<same relative path>.bak (outside the card folders, not .gci)
//   2 WriteTemp:      write the new file (header code replaced) as <target>.tmp, next to the target
//   3 Rename:         rename the temp to the target name, never replacing an existing file
//   4 RemoveOriginal: only now remove the original from the card folder
// Any failure leaves that file alone (the temp is cleaned up), is logged, and the next file goes on. A run that is
// stopped after any step is finished by the next run, and a run with nothing left to do changes nothing.
// Collision: if the target name already holds a save with the right code, that save wins and is not touched; the
// old file only goes to the backup (steps 1 and 4).
// Stuck file: a file already named for the right code but still carrying the wrong code in its header is repaired in
// place: step 3 then replaces that very file (its bytes are in the backup by then), and step 4 has nothing to do.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#endif

namespace OldSaveConvert {

enum class Step { Backup = 1, WriteTemp = 2, Rename = 3, RemoveOriginal = 4 };

struct Hooks {
    // Test only. Returns true to make this step fail instead of running (nothing is done for it).
    std::function<bool(Step)> failStep;
    // Test only. Returns true to end the whole run right after this step succeeded, as if the process were killed.
    std::function<bool(Step)> killAfter;
    std::function<void(const std::string&)> log;
};

struct Summary {
    int converted = 0;  // new file written under the right code, original removed
    int repaired = 0;   // stuck file (name right, header wrong) fixed in place
    int collided = 0;   // right-code save already there: old file only backed up and removed
    int failed = 0;     // left alone
    bool killed = false;
};

namespace detail {

namespace fs = std::filesystem;

constexpr size_t kHeaderSize = 0x40;       // GCI directory entry
constexpr size_t kNameOffset = 8;
constexpr size_t kNameSize = 32;
constexpr uintmax_t kMaxFileSize = 17u * 1024 * 1024;  // 2043-block card + header

inline std::string Utf8(const fs::path& p) {
    const auto text = p.u8string();  // std::string before C++20, std::u8string after
    return std::string(text.begin(), text.end());
}

inline void Log(const Hooks& h, const std::string& line) {
    if (h.log) h.log(line);
}

inline bool Fails(const Hooks& h, Step s) { return h.failStep && h.failStep(s); }

// Reads at most `limit` bytes. false on any read error (a file that cannot be read is never "empty").
inline bool ReadFile(const fs::path& path, size_t limit, std::vector<uint8_t>& out) {
#ifdef _WIN32
    std::FILE* f = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out.clear();
    uint8_t buf[8192];
    bool ok = true;
    while (out.size() < limit) {
        const size_t want = std::min(sizeof(buf), limit - out.size());
        const size_t got = std::fread(buf, 1, want, f);
        out.insert(out.end(), buf, buf + got);
        if (got < want) {
            ok = !std::ferror(f);
            break;
        }
    }
    std::fclose(f);
    return ok;
}

// Writes the whole file and checks every result: fwrite, flush and close. Replaces `path` if it exists.
inline bool WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
#ifdef _WIN32
    std::FILE* f = _wfopen(path.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) return false;
    std::error_code ec;
    return fs::file_size(path, ec) == bytes.size() && !ec;
}

// Rename that never replaces an existing target.
inline bool RenameNoReplace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), 0) != 0;  // no MOVEFILE_REPLACE_EXISTING: fails if `to` exists
#else
    std::error_code ec;
    fs::create_hard_link(from, to, ec);  // fails if `to` exists
    if (!ec) {
        fs::remove(from, ec);
        return true;
    }
    if (ec == std::errc::file_exists) return false;
    // No hard links here (some file systems): the best left is a check, then the rename.
    if (fs::exists(to, ec)) return false;
    fs::rename(from, to, ec);
    return !ec;
#endif
}

struct Header {
    char game[4];
    char maker[2];
    char name[kNameSize + 1];
};

// First bytes of a .gci: game code 0-3, maker 4-5, internal file name 8-39.
inline bool ReadHeader(const fs::path& path, Header& h) {
    std::vector<uint8_t> b;
    if (!ReadFile(path, kNameOffset + kNameSize, b) || b.size() < kNameOffset + kNameSize) return false;
    std::memcpy(h.game, &b[0], 4);
    std::memcpy(h.maker, &b[4], 2);
    std::memcpy(h.name, &b[kNameOffset], kNameSize);
    h.name[kNameSize] = 0;
    return true;
}

// "ScoobyDoo" + 2 digits, nothing after.
inline bool IsOurName(const char* name) {
    return std::strncmp(name, "ScoobyDoo", 9) == 0 && name[9] >= '0' && name[9] <= '9' && name[10] >= '0' &&
           name[10] <= '9' && name[11] == 0;
}

inline bool IsOurs(const Header& h) { return std::memcmp(h.maker, "78", 2) == 0 && IsOurName(h.name); }

inline fs::path BackupPathFor(const fs::path& cardRoot, const fs::path& backupRoot, const fs::path& file) {
    fs::path rel = file.lexically_relative(cardRoot);
    if (rel.empty() || *rel.begin() == "..") rel = file.filename();
    fs::path p = backupRoot / rel;
    p += ".bak";
    return p;
}

struct Job {
    fs::path path;
    Header header;
    fs::path target;
    bool stuck = false;  // target is this very file
};

}  // namespace detail

// Converts old-code saves under `cardRoot` (the "MemoryCard" folder: "<region>/Card A" and "Card B" below it).
// `backupRoot` must be outside `cardRoot`. `game` is the booted game's code (4 characters, e.g. "GIHE").
inline Summary Run(const std::filesystem::path& cardRoot, const std::filesystem::path& backupRoot, const char* game,
                   const Hooks& hooks = {}) {
    namespace fs = std::filesystem;
    using namespace detail;
    Summary sum;
    std::error_code ec;
    if (!fs::is_directory(cardRoot, ec)) return sum;

    // Find candidates: .gci files directly inside a folder called "Card A" or "Card B".
    std::vector<Job> jobs;
    fs::recursive_directory_iterator it(cardRoot, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        Log(hooks, "[card] old saves: cannot list " + Utf8(cardRoot) + ": " + ec.message());
        return sum;
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            Log(hooks, "[card] old saves: listing stopped in " + Utf8(cardRoot) + ": " + ec.message());
            break;
        }
        const fs::path p = it->path();
        std::error_code e2;
        if (p.extension() != ".gci" || !it->is_regular_file(e2)) continue;
        const std::string parent = Utf8(p.parent_path().filename());
        if (parent != "Card A" && parent != "Card B") continue;
        Header h;
        if (!ReadHeader(p, h)) continue;  // too short or unreadable: not ours to touch
        if (!IsOurs(h) || std::memcmp(h.game, game, 4) == 0) continue;
        Job j;
        j.path = p;
        j.header = h;
        std::string name = "78-";
        name.append(game, 4);
        name += "-" + std::string(h.name) + ".gci";
        j.target = p.parent_path() / fs::path(name);  // ASCII only: safe under any code page
        j.stuck = fs::exists(j.target, e2) && fs::equivalent(j.target, p, e2);
        jobs.push_back(std::move(j));
    }
    // Stuck files first (a fixed file is a right-code save for the others), then by name for a stable order.
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
        if (a.stuck != b.stuck) return a.stuck;
        return a.path < b.path;
    });

    for (const Job& j : jobs) {
        const std::string label = Utf8(j.path.filename());
        auto fail = [&](const char* what) {
            Log(hooks, "[card] old save " + label + ": " + what + "; left alone");
            ++sum.failed;
        };
        auto killed = [&](Step s) {
            if (hooks.killAfter && hooks.killAfter(s)) {
                sum.killed = true;
                return true;
            }
            return false;
        };

        std::vector<uint8_t> bytes;
        if (!ReadFile(j.path, kMaxFileSize, bytes) || bytes.size() < kHeaderSize || bytes.size() >= kMaxFileSize) {
            fail("cannot read it");
            continue;
        }

        const bool collision = !j.stuck && fs::exists(j.target, ec);
        if (collision) {
            Header th;
            if (!ReadHeader(j.target, th) || std::memcmp(th.game, game, 4) != 0) {
                fail("the target name is taken by something that is not a save for this game");
                continue;
            }
        }

        // 1: backup (outside the card folders). An identical earlier backup (an interrupted run) is reused.
        fs::path backup = BackupPathFor(cardRoot, backupRoot, j.path);
        if (Fails(hooks, Step::Backup)) {
            fail("backup failed (injected)");
            continue;
        }
        bool backedUp = false;
        fs::create_directories(backup.parent_path(), ec);
        if (!ec) {
            for (int n = 0; n < 1000 && !backedUp; ++n) {
                fs::path candidate = backup;
                if (n) candidate += "." + std::to_string(n);
                std::vector<uint8_t> existing;
                if (fs::exists(candidate, ec)) {
                    backedUp = ReadFile(candidate, kMaxFileSize, existing) && existing == bytes;
                    if (backedUp) backup = candidate;
                } else if (WriteFile(candidate, bytes)) {
                    backup = candidate;
                    backedUp = true;
                    break;
                } else {
                    break;  // cannot write there: no point trying other names
                }
            }
        }
        if (!backedUp) {
            fail("backup failed");
            continue;
        }
        if (killed(Step::Backup)) return sum;

        if (!collision) {
            // 2: temp file with the new header, next to the target, not ending in .gci.
            fs::path temp = j.target;
            temp += ".tmp";
            std::vector<uint8_t> fixed = bytes;
            std::memcpy(&fixed[0], game, 4);
            if (Fails(hooks, Step::WriteTemp) || !WriteFile(temp, fixed)) {
                fs::remove(temp, ec);
                fail("could not write the new file");
                continue;
            }
            if (killed(Step::WriteTemp)) return sum;

            // 3: into place. A stuck file is replaced by its fixed copy (the backup holds the original bytes);
            //    anything else never replaces a file.
            bool placed;
            if (Fails(hooks, Step::Rename)) {
                placed = false;
            } else if (j.stuck) {
                fs::rename(temp, j.target, ec);
                placed = !ec;
            } else {
                placed = RenameNoReplace(temp, j.target);
            }
            if (!placed) {
                fs::remove(temp, ec);
                fail("could not rename the new file into place");
                continue;
            }
            if (killed(Step::Rename)) return sum;

            if (j.stuck) {
                Log(hooks, "[card] repaired " + label + " (header code now " + std::string(game, 4) + "); backup " +
                               Utf8(backup));
                ++sum.repaired;
                continue;
            }
        }

        // 4: only now remove the original from the card folder.
        if (Fails(hooks, Step::RemoveOriginal)) {
            fail("could not remove the original");  // the next run backs it up again and removes it
            continue;
        }
        fs::remove(j.path, ec);
        if (ec || fs::exists(j.path)) {
            fail("could not remove the original");
            continue;
        }
        if (killed(Step::RemoveOriginal)) return sum;

        if (collision) {
            Log(hooks, "[card] old save " + label + " not converted: " + Utf8(j.target.filename()) +
                           " already exists and was kept; old file moved to backup " + Utf8(backup));
            ++sum.collided;
        } else {
            Log(hooks, "[card] converted " + label + " -> " + Utf8(j.target.filename()) + "; backup " + Utf8(backup));
            ++sum.converted;
        }
    }
    return sum;
}

}  // namespace OldSaveConvert
