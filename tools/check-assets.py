#!/usr/bin/env python3
"""Before-PR check: fails when a change adds game assets to the repository.

    python tools/check-assets.py [--base REV] [--head REV] [--staged-only | --no-staged] [--max-mib N]

Run it by hand before opening a pull request (it is not a git hook; there is no CI). It looks at
  - the files staged in the index (what `git commit` would add), and
  - every file the commits in BASE..HEAD bring in (default: the first of origin/main, upstream/main, main that exists,
    up to HEAD), including files a later commit in the same range deletes again: the history would carry them.
    (If a pull request's merge commit is checked out, use --base HEAD^1 --head HEAD^2.)
`--staged-only` checks just the index, `--no-staged` just the commits. Files that are only in the working tree, untracked or
git-ignored, are not looked at (git-ignored is where game data belongs); `git add -f` is what this catches.

A file is refused when
  - its path is in a folder that only ever holds game data or build output (Disc/, Play/, texture_pack/, ...),
  - its extension is a disc image, a disc file type the game reads (.hip, .bik, .dol, .tpl, ...), a save or an archive,
  - it is a local-only file (root AGENTS.md or CLAUDE.md, *.log),
  - it is bigger than the size cap (default 16 MiB), or
  - its first bytes are the magic number of one of those formats, whatever the file is called.
tools/hd-pack-additions/ is the one accepted exception to the path, extension and size rules (our own Creeper frames);
the magic-number check still applies there.

What this cannot catch: game data renamed AND re-encoded (text dumps, base64 or hex in source, a custom container, split
into pieces under the cap, a changed header), an asset whose type is not in the lists, a single game file with an unlisted
extension and no recognisable header (a .dol has no magic number), a change that edits this script's lists, and assets
that reach the repository some other way (a direct push, Releases, LFS, another repository). It is a safety net, not a scanner.

Python 3.8+, standard library only, Windows and Linux. Exit 0 = clean, 1 = something refused, 2 = could not run.
"""
import argparse
import subprocess
import sys

DEFAULT_MAX_MIB = 16

# Only game data or build output ever goes in these (they are git-ignored; this stops `git add -f`). Compared in lower case,
# from the repository root only (tools/debug/game/ is ours). A worktree's link to such a folder is a file named like the
# folder (git stores a symlink as a blob), so the bare name is refused too.
FORBIDDEN_PREFIXES = (
    "disc/", "game/", "texture_pack/", "autotest/", "toolchain/", "dist/", "build-logs/", ".claude/",
    "wiicompiled/generated/", "wiicompiled/native-build", "wiicompiled/dependencies/",
    "tools/debug/local/",
)
# Play/ is the build output folder; two small player helper files in it are tracked on purpose.
PLAY_ALLOWED = ("play/portable.txt", "play/run with log.cmd")

# Local-only files by exact (lower case) path or by extension.
FORBIDDEN_PATHS = ("agents.md", "claude.md")

EXEMPT_PREFIX = "tools/hd-pack-additions/"

FORBIDDEN_EXTENSIONS = {
    # disc images
    "iso", "gcm", "ciso", "rvz", "wia", "gcz", "nfs", "tgc", "wbfs", "wad",
    # the game's own file types (the disc holds .hip, .bik, .bin, .tpl, .scc, .img, .dol, .bnr, ...)
    "dol", "rel", "hip", "hop", "bik", "tpl", "bnr", "img", "scc",
    # saves (memory card images and single saves)
    "gci", "gcs", "sav", "raw",
    # archives (a whole disc or pack fits in one)
    "zip", "7z", "rar", "tar", "gz", "tgz", "xz", "bz2", "zst",
    # logs
    "log",
}

# (offset, bytes, what). Checked on the first 64 bytes of every file, whatever its name.
MAGIC = (
    (0x00, b"HIPA", "game archive (.hip)"),
    (0x1C, bytes.fromhex("C2339F3D"), "GameCube disc image"),
    (0x18, bytes.fromhex("5D1C9EA3"), "Wii disc image"),
    (0x00, b"CISO", "CISO disc image"),
    (0x00, b"RVZ\x01", "RVZ disc image"),
    (0x00, b"WIA\x01", "WIA disc image"),
    (0x00, b"WBFS", "WBFS disc image"),
    (0x00, bytes.fromhex("B10BC001"), "GCZ disc image"),
    (0x00, bytes.fromhex("0020AF30"), "texture palette (.tpl)"),
    (0x00, b"PK\x03\x04", "zip archive"),
    (0x00, b"7z\xbc\xaf\x27\x1c", "7z archive"),
    (0x00, b"Rar!\x1a\x07", "rar archive"),
    (0x00, b"\x1f\x8b", "gzip archive"),
    (0x00, bytes.fromhex("FD377A585A00"), "xz archive"),
    (0x00, b"BZh", "bzip2 archive"),
    (0x00, bytes.fromhex("28B52FFD"), "zstd archive"),
)
MAGIC_BYTES = 64
BASE_CANDIDATES = ("origin/main", "upstream/main", "main")


def git(args, stdin=None):
    """Runs git in the current folder; returns stdout as bytes. RuntimeError when git is missing or fails."""
    try:
        result = subprocess.run(["git"] + args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        raise RuntimeError("cannot run git: %s" % error)
    if result.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), result.stderr.decode("utf-8", "replace").strip()))
    return result.stdout


def text(args, stdin=None):
    return git(args, stdin).decode("utf-8", "surrogateescape")


def default_base():
    for name in BASE_CANDIDATES:
        try:
            git(["rev-parse", "--verify", "--quiet", name + "^{commit}"])
            return name
        except RuntimeError:
            continue
    raise RuntimeError("no %s to compare with; fetch it or pass --base" % ", ".join(BASE_CANDIDATES))


def parse_raw(raw, label, found):
    """Adds (path, blob sha) -> label for each entry of `git diff --raw -z` output (added, modified or type-changed)."""
    fields = raw.split("\0")
    index = 0
    while index + 1 < len(fields) and fields[index].startswith(":"):
        meta = fields[index].split()
        path = fields[index + 1]
        index += 2
        if meta[1] == "160000":  # a submodule pointer, not a file
            continue
        found.setdefault((path, meta[3]), label)


def changed_files(base, head, staged, commits):
    """{(path, blob sha): note}: the staged changes plus everything the commits in base..head bring in."""
    found = {}
    flags = ["--raw", "--no-abbrev", "-z", "--no-renames", "--diff-filter=AMT"]
    if staged:
        parse_raw(text(["diff", "--cached"] + flags + ["HEAD"]) if has_head() else text(["diff", "--cached"] + flags), " (staged)", found)
    if commits:
        # Against the merge base, so a main that moved on does not show up as changes of ours.
        fork = text(["merge-base", base, head]).strip()
        parse_raw(text(["diff"] + flags + [fork, head]), "", found)
        # Blobs that exist only in the middle of the range (added, then removed or changed by a later commit).
        have = {sha for (_, sha) in found}
        for line in text(["rev-list", "--objects", "%s..%s" % (base, head)]).splitlines():
            sha, _, path = line.partition(" ")
            if path and sha not in have:
                found[(path, sha)] = " (in an earlier commit of the range)"
    return found


def has_head():
    try:
        git(["rev-parse", "--verify", "--quiet", "HEAD^{commit}"])
        return True
    except RuntimeError:
        return False


def blob_heads(shas, max_bytes):
    """{sha: (type, size, first bytes)}. One `git cat-file --batch` process; contents are read only for blobs at or under
    max_bytes (a bigger one is refused for its size anyway, so a disc image is never pulled through the pipe)."""
    info = {}
    if not shas:
        return info
    try:
        proc = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
    except OSError as error:
        raise RuntimeError("cannot run git: %s" % error)
    # Requests are written from a thread so a full pipe in either direction cannot block both sides.
    import threading

    def feed():
        try:
            for sha in shas:
                proc.stdin.write((sha + "\n").encode("ascii"))
            proc.stdin.close()
        except OSError:
            pass
    writer = threading.Thread(target=feed, daemon=True)
    writer.start()
    for sha in shas:
        header = proc.stdout.readline().split()
        if len(header) != 3:
            info[sha] = ("missing", 0, b"")
            continue
        kind, size = header[1].decode("ascii"), int(header[2])
        head = b""
        if kind == "blob" and size <= max_bytes:
            body = proc.stdout.read(size)
            head = body[:MAGIC_BYTES]
        else:
            remaining = size
            while remaining:
                chunk = proc.stdout.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                remaining -= len(chunk)
        proc.stdout.read(1)  # the newline after each object
        info[sha] = (kind, size, head)
    writer.join()
    proc.stdout.close()
    proc.stderr.close()
    if proc.wait() != 0:
        raise RuntimeError("git cat-file failed")
    return info


def judge(path, note, size, head, max_bytes):
    """The reasons one file (a path as git prints it, its size and first bytes) is refused."""
    reasons = []
    lower = path.lower().replace("\\", "/").strip("/")
    exempt = lower.startswith(EXEMPT_PREFIX)
    if not exempt:
        if (lower.startswith("play/") and lower not in PLAY_ALLOWED) or lower == "play":
            reasons.append("Play/ is the build output folder")
        for prefix in FORBIDDEN_PREFIXES:
            if lower.startswith(prefix) or lower == prefix.rstrip("/"):
                reasons.append("%s only holds game data or build output" % prefix)
                break
        if lower in FORBIDDEN_PATHS:
            reasons.append("this file is local to one checkout and never committed")
        name = lower.rsplit("/", 1)[-1].rstrip(". ")  # Windows drops trailing dots and spaces
        extension = name.rsplit(".", 1)[-1] if "." in name else ""
        if extension in FORBIDDEN_EXTENSIONS:
            reasons.append(".%s is a game data, save, log or archive file type" % extension)
        if size > max_bytes:
            reasons.append("%.1f MiB is over the %g MiB cap" % (size / 1048576, max_bytes / 1048576))
    if len(head) >= 4:
        for offset, magic, what in MAGIC:
            if head[offset:offset + len(magic)] == magic:
                reasons.append("starts like a %s" % what)
                break
    return ["%s%s: %s" % (path, note, reason) for reason in reasons]


def check(base, head, max_bytes, staged=True, commits=True):
    """(files, problems) for the staged changes and/or the commits in base..head."""
    files = changed_files(base, head, staged, commits)
    info = blob_heads(sorted({sha for (_, sha) in files}), max_bytes)
    problems = []
    for (path, sha), note in sorted(files.items()):
        kind, size, first = info.get(sha, ("missing", 0, b""))
        if kind != "blob":
            continue  # a tree listed by rev-list
        problems.extend(judge(path, note, size, first, max_bytes))
    return files, problems


def main(argv=None):
    parser = argparse.ArgumentParser(description="Before-PR check: fails when a change adds game assets.")
    parser.add_argument("--base", help="the branch to compare with (default: origin/main, else upstream/main, else main)")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--max-mib", type=float, default=DEFAULT_MAX_MIB)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--staged-only", action="store_true", help="check only the staged files")
    scope.add_argument("--no-staged", action="store_true", help="check only the commits, not the index")
    args = parser.parse_args(argv)
    staged = not args.no_staged
    commits = not args.staged_only
    try:
        git(["rev-parse", "--git-dir"])
        base = args.base or (default_base() if commits else "")
        files, problems = check(base, args.head, int(args.max_mib * 1048576), staged, commits)
    except RuntimeError as error:
        print("ERROR: %s" % error, file=sys.stderr)
        return 2
    scope_text = " and ".join(
        part for part in ("staged files" if staged else "", "%s..%s" % (base, args.head) if commits else "") if part)
    unique = sorted(set(problems))
    if unique:
        print("Game assets (or files that look like them) found in %s:" % scope_text)
        for line in unique:
            print("  " + line)
        print("\nNever commit the disc, extracted game files, saves, logs, translated game code or build output (.agents/REPO.md,\n"
              "\"Never commit\"). Unstage a file with `git restore --staged <file>`; for a commit, remove the file from your\n"
              "branch's history too (the history would still carry it). If a file really is part of the project, the\n"
              "maintainers change the lists in tools/check-assets.py in their own review.")
        return 1
    print("No game assets in %s (%d file versions checked)." % (scope_text, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
