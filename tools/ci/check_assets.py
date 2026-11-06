#!/usr/bin/env python3
"""Fails when a change adds game assets to the repository.

    python tools/ci/check_assets.py [--base REV] [--head REV] [--max-mib N]

Looks at every file the commits in BASE..HEAD (default origin/main..HEAD) bring in, including files a later commit in the
same range deletes again: the history would carry them. (If a merge commit of a pull request is checked out, use
--base HEAD^1 --head HEAD^2.) A file is refused when
  - its path is in a folder that only ever holds game data or build output (Disc/, Play/, texture_pack/, ...),
  - its extension is a disc image, a disc file type the game reads (.hip, .bik, .dol, .tpl, ...) or an archive,
  - it is bigger than the size cap (default 8 MiB), or
  - its first bytes are the magic number of one of those formats, whatever the file is called.
tools/hd-pack-additions/ is the one accepted exception to the path, extension and size rules (our own Creeper frames);
the magic-number check still applies there.

What this cannot catch: game data renamed AND re-encoded (text dumps, base64 or hex in source,
a custom container, split into pieces under the cap, a changed header), an asset whose type is not in the lists, a
single DOL or game file with an unlisted extension and no recognisable header (a .dol has no magic number), a change
that edits this script's lists (anything running the script from the change itself trusts the change), and assets that reach the repository some other way than a pull request (a direct push by
someone with bypass rights, Releases, LFS, another repository). It is a safety net, not a scanner.

Python 3.8+, standard library only. Exit 0 = clean, 1 = something refused, 2 = could not run.
"""
import argparse
import subprocess
import sys

DEFAULT_MAX_MIB = 8

# Only game data or build output ever goes in these (they are git-ignored; this stops `git add -f`). Compared in lower case.
FORBIDDEN_PREFIXES = (
    "disc/", "game/", "texture_pack/", "autotest/", "toolchain/", "dist/", "build-logs/",
    "wiicompiled/generated/", "wiicompiled/native-build", "wiicompiled/dependencies/",
    "tools/debug/local/",
)
# Play/ is the build output folder; two small player helper files in it are tracked on purpose.
PLAY_ALLOWED = ("play/portable.txt", "play/run with log.cmd")

EXEMPT_PREFIX = "tools/hd-pack-additions/"

FORBIDDEN_EXTENSIONS = {
    # disc images
    "iso", "gcm", "ciso", "rvz", "wia", "gcz", "nfs", "tgc", "wbfs", "wad",
    # the game's own file types (the disc holds .hip, .bik, .bin, .tpl, .scc, .img, .dol, .bnr, ...)
    "dol", "rel", "hip", "hop", "bik", "tpl", "bnr", "img", "scc",
    # archives (a whole disc or pack fits in one)
    "zip", "7z", "rar", "tar", "gz", "tgz", "xz", "bz2", "zst",
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


def git(args, stdin=None, binary=False):
    result = subprocess.run(["git"] + args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), result.stderr.decode("utf-8", "replace").strip()))
    return result.stdout if binary else result.stdout.decode("utf-8", "surrogateescape")


def changed_files(base, head):
    """Every (path, blob sha) the range brings in: the final diff (exact paths) plus all blobs of all commits (history)."""
    found = {}
    raw = git(["diff", "--raw", "--no-abbrev", "-z", "--no-renames", "--diff-filter=AMT", base, head])
    fields = raw.split("\0")
    index = 0
    while index + 1 < len(fields) and fields[index].startswith(":"):
        meta = fields[index].split()
        path = fields[index + 1]
        index += 2
        if meta[1] == "160000":  # a submodule pointer, not a file
            continue
        found.setdefault((path, meta[3]), "")
    # Blobs that exist only in the middle of the range (added, then removed or changed by a later commit).
    listing = git(["rev-list", "--objects", "%s..%s" % (base, head)])
    for line in listing.splitlines():
        sha, _, path = line.partition(" ")
        if path and not any(existing == sha for (_, existing) in found):
            found[(path, sha)] = " (in an earlier commit of the range)"
    return found


def blob_info(shas):
    """{sha: (type, size)} for the given objects."""
    out = git(["cat-file", "--batch-check"], stdin="".join(sha + "\n" for sha in shas).encode("ascii")).splitlines()
    info = {}
    for sha, line in zip(shas, out):
        parts = line.split()
        if len(parts) == 3:
            info[sha] = (parts[1], int(parts[2]))
    return info


def blob_head(sha):
    data = git(["cat-file", "blob", sha], binary=True)
    return data[:MAGIC_BYTES]


def check(base, head, max_bytes):
    problems = []
    files = changed_files(base, head)
    info = blob_info(sorted({sha for (_, sha) in files}))
    for (path, sha), note in sorted(files.items()):
        kind, size = info.get(sha, ("", 0))
        if kind != "blob":
            continue  # a tree listed by rev-list
        lower = path.lower().replace("\\", "/")
        exempt = lower.startswith(EXEMPT_PREFIX)
        if not exempt:
            if lower.startswith("play/") and lower not in PLAY_ALLOWED:
                problems.append("%s%s: Play/ is the build output folder" % (path, note))
            for prefix in FORBIDDEN_PREFIXES:
                if lower.startswith(prefix):
                    problems.append("%s%s: %s only holds game data or build output" % (path, note, prefix))
                    break
            extension = lower.rsplit(".", 1)[-1] if "." in lower.rsplit("/", 1)[-1] else ""
            if extension in FORBIDDEN_EXTENSIONS:
                problems.append("%s%s: .%s is a game data or archive file type" % (path, note, extension))
            if size > max_bytes:
                problems.append("%s%s: %.1f MiB is over the %g MiB cap" % (path, note, size / 1048576, max_bytes / 1048576))
        if size >= 4:
            first = blob_head(sha)
            for offset, magic, what in MAGIC:
                if first[offset:offset + len(magic)] == magic:
                    problems.append("%s%s: starts like a %s" % (path, note, what))
                    break
    return files, problems


def main(argv=None):
    parser = argparse.ArgumentParser(description="Fails when a change adds game assets.")
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--max-mib", type=float, default=DEFAULT_MAX_MIB)
    args = parser.parse_args(argv)
    try:
        files, problems = check(args.base, args.head, int(args.max_mib * 1048576))
    except RuntimeError as error:
        print("ERROR: %s" % error, file=sys.stderr)
        return 2
    unique = sorted(set(problems))
    if unique:
        print("Game assets (or files that look like them) found in %s..%s:" % (args.base, args.head))
        for line in unique:
            print("  " + line)
        print("\nNever commit the disc, extracted game files, translated game code or build output. If a file is a genuine\n"
              "part of the project, the maintainers change the lists in tools/ci/check_assets.py in their own review.")
        return 1
    print("No game assets in %s..%s (%d file versions checked)." % (args.base, args.head, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
