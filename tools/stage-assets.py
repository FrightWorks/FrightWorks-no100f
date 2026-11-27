#!/usr/bin/env python3
"""Puts the files the game ships besides the program into its folder (beside "FrightWorks-No100F-Game.exe"): the button prompt
icons in button_icons (tools/make-button-icons.py). They are program files: an installed game's folder is replaced on
every update.

No build ships an HD texture pack since 2026-11-24: packs hold pictures made from the game's textures, so the launcher
downloads one only when the player chooses it (launcher/TexturePacks.cs, into UserData/texture_packs). A copy of
dogerki's pack that an older build staged into the game folder (texture_replacements/ScoobyHD) is removed here; a link
the debug kit put there (texture_pack/ScoobyHD, tools/debug/get-hd-pack.py) is left alone.

    python tools/stage-assets.py --game-dir <folder> [--required]

(tools\\stage-assets.ps1 -GameDir <folder> [-Required] runs this on Windows.) Used by build-no100f.ps1 (Play\\, after
every build) and package-scooby.ps1 (the release's data\\). Only what changed is copied or rebuilt. --required
(packaging) stops on anything missing; without it a missing piece is a warning ("WARNING: ..."), so a test build still
finishes. Failures print "ERROR: <text>" and exit 1.

Python 3.8+, standard library only. The button icons need Pillow and ImageMagick (magick), like make-button-icons.py.
"""
import argparse
import glob
import os
import shutil
import stat
import subprocess
import sys

WINDOWS = os.name == "nt"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.dirname(os.path.abspath(__file__))


class Failure(Exception):
    """Stops the script (exit 1)."""


def say(text):
    print(text, flush=True)


def fail(text, required):
    """A missing piece: an error when --required (packaging), else a warning."""
    if required:
        raise Failure(text)
    say("WARNING: " + text)


def is_link(path):
    """A symlink, or on Windows also a junction (any reparse point)."""
    try:
        info = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def remove_tree(path):
    def writable(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if is_link(path):
        try:
            os.rmdir(path)      # a Windows directory link: removes the link, never what it points to
        except OSError:
            os.unlink(path)
    elif os.path.isdir(path):
        shutil.rmtree(path, onerror=writable)
    elif os.path.lexists(path):
        os.chmod(path, stat.S_IWRITE)
        os.remove(path)


def find_magick():
    """The directory to add to PATH for magick, '' when it's on PATH already, None when there is none."""
    if shutil.which("magick") or (not WINDOWS and shutil.which("convert")):   # Ubuntu's ImageMagick 6 has only `convert`
        return ""
    if WINDOWS:
        # ImageMagick's installer doesn't always put itself on PATH.
        for folder in sorted(glob.glob(os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                                                    "ImageMagick-*")), key=os.path.basename, reverse=True):
            if os.path.isdir(folder) and os.path.isfile(os.path.join(folder, "magick.exe")):
                return folder
    return None


def stage(game_dir, required):
    os.makedirs(game_dir, exist_ok=True)

    # Builds before 2026-11 staged both into UserData, where the old copy would now win over the new one: remove them.
    # A link (the debug kit links the pack there) is only unlinked, never emptied.
    for old in (os.path.join("UserData", "texture_replacements", "ScoobyHD"), os.path.join("UserData", "button_icons")):
        path = os.path.join(game_dir, old)
        if not os.path.lexists(path):
            continue
        remove_tree(path)
        say("Removed the old copy in " + path)

    # 1. An HD pack an older build staged here (a real folder): no build ships one now. A link is the debug kit's.
    old_pack = os.path.join(game_dir, "texture_replacements", "ScoobyHD")
    if os.path.isdir(old_pack) and not is_link(old_pack):
        remove_tree(old_pack)
        say("Removed the HD pack an older build staged in %s (packs are a launcher download now)" % old_pack)
        try:
            os.rmdir(os.path.dirname(old_pack))   # only when nothing else is in texture_replacements
        except OSError:
            pass

    # 2. Button prompt icons, drawn from tools/button-art in the HD pack's layout. Rebuilt when the tool or the art is
    # newer than the icons (or the folder has the old layout).
    icons = os.path.join(game_dir, "button_icons")
    layout = os.path.join(icons, "layout.txt")
    tool = os.path.join(TOOLS, "make-button-icons.py")
    inputs = [tool]
    for folder, _dirs, files in os.walk(os.path.join(TOOLS, "button-art")):
        inputs.extend(os.path.join(folder, name) for name in files)
    newest = max(os.stat(path).st_mtime_ns for path in inputs)
    if os.path.isfile(layout) and os.stat(layout).st_mtime_ns >= newest:
        say("Button icons: %s (up to date)" % icons)
        return
    magick = find_magick()
    if magick is None:
        fail("button icons need Python and ImageMagick (magick, or convert on Linux) on PATH: %s not updated" % icons, required)
        return
    remove_tree(icons)
    env = dict(os.environ)
    if magick:
        env["PATH"] = magick + os.pathsep + env.get("PATH", "")
    # Same interpreter as this script (the one the wrapper found). Not -I: make-button-icons.py needs Pillow, which may
    # live in the user's site-packages.
    result = subprocess.run([sys.executable, tool, "--out", icons], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if result.returncode != 0:
        output = result.stdout.decode("utf-8", "replace").strip()
        raise Failure("make-button-icons.py failed" + (":\n" + output if output else ""))
    say("Button icons: %s (rebuilt)" % icons)


def main(argv):
    parser = argparse.ArgumentParser(description="Stages the button icons into a game folder.")
    parser.add_argument("--game-dir", required=True, help="the folder beside FrightWorks-No100F-Game.exe")
    parser.add_argument("--required", action="store_true", help="stop on anything missing instead of warning")
    args = parser.parse_args(argv)
    try:
        stage(os.path.abspath(args.game_dir), args.required)
    except Failure as error:
        print("ERROR: %s" % error, flush=True)
        return 1
    except OSError as error:
        print("ERROR: %s" % error, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as error:   # a bug or something unforeseen: say so on stdout, where the .ps1 wrapper reads it
        import traceback
        print("ERROR: unexpected %s: %s" % (type(error).__name__, error))
        traceback.print_exc(file=sys.stdout)
        sys.exit(1)
