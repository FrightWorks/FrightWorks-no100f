#!/usr/bin/env python3
"""Downloads dogerki's HD texture pack into texture_pack/ScoobyHD (git-ignored) for the debug kit: play-patched.ps1
-HdTextures, manual-session.ps1 -HdTextures, look.ps1, hunt.ps1, spots-run.ps1, loop-capture.ps1, smoke-test.py and
the texhunt tools link or read it from there. Players get it from the launcher instead (Extras > Textures,
launcher/TexturePacks.cs); no build ships it since 2026-11-24.

    python tools/debug/get-hd-pack.py            (nothing to do when texture_pack/ScoobyHD is there)

It is dogerki's pack as published: the game fixes the Smuggler's Cove sign's alpha as it loads (texture_replacement.cpp
add_original_alpha). The Creeper window frames that builds used to add are in the separate menu pack now
(Loneranger419/no100f-menu-pack).

Python 3.8+, standard library only. The pack is a RAR: unpacking needs bsdtar (Windows' tar.exe, or libarchive's
bsdtar on Linux), 7-Zip or unrar. Exit code 0 when the pack is there afterwards, 1 when not.
"""
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import time
import urllib.request

WINDOWS = os.name == "nt"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACK_URL = ("https://archive.org/download/dolphin-scooby-doo-night-of-100-frights-hd-texture-pack/"
            "Dolphin%20Scooby%20Doo%20Night%20of%20100%20Frights%20HD%20Texture%20Pack.rar")
PACK_SHA1 = "1E55E7F802BD0E5AD00538FB21D17A66417F723B"

class Failure(Exception):
    """Stops the script (exit 1)."""


def say(text):
    print(text, flush=True)


def remove_tree(path):
    def writable(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if os.path.isdir(path):
        shutil.rmtree(path, onerror=writable)


def sha1_of(path):
    digest = hashlib.sha1()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def download(url, path, attempts=3):
    request = urllib.request.Request(url, headers={"User-Agent": "FrightWorks-build-scripts"})
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=60) as response, open(path, "wb") as out:
                shutil.copyfileobj(response, out, 1 << 20)
            return
        except OSError:
            if attempt == attempts:
                raise
            time.sleep(2 * attempt)


def run_quiet(command):
    """Exit code of a command whose output isn't wanted (-1 when it can't start)."""
    try:
        return subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode
    except OSError:
        return -1


def unpack_rar(rar, unpack):
    """RAR5: libarchive's tar reads it; 7-Zip (or unrar) if that fails."""
    if WINDOWS:
        tars = [os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")]
    else:
        tars = [shutil.which("bsdtar")]
    for tar in tars:
        if tar and run_quiet([tar, "-xf", rar, "-C", unpack]) == 0:
            return
    candidates = [shutil.which(name) for name in ("7z", "7zz", "7za", "7zr")]
    if WINDOWS:
        candidates.append(os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "7-Zip", "7z.exe"))
    seven_zip = next((c for c in candidates if c and os.path.isfile(c)), None)
    if seven_zip:
        if run_quiet([seven_zip, "x", "-y", "-o" + unpack, rar]) != 0:
            raise Failure("7-Zip failed to unpack the RAR")
        return
    unrar = shutil.which("unrar")
    if unrar:
        if run_quiet([unrar, "x", "-y", rar, unpack + os.sep]) != 0:
            raise Failure("unrar failed to unpack the RAR")
        return
    raise Failure("can't unpack the RAR: %s failed and neither 7-Zip nor unrar is installed"
                  % ("Windows tar" if WINDOWS else "bsdtar (libarchive) is missing or"))


def download_pack(pack_source):
    """texture_pack/ScoobyHD = dogerki's pack, downloaded from the Internet Archive the first time."""
    rar = os.path.join(ROOT, "texture_pack", "ScoobyHD.rar")
    unpack = os.path.join(ROOT, "texture_pack", "ScoobyHD.unpack")
    try:
        os.makedirs(os.path.dirname(rar), exist_ok=True)
        if not (os.path.isfile(rar) and sha1_of(rar) == PACK_SHA1):
            say("HD pack: downloading dogerki's HD texture pack (604 MB) from archive.org...")
            partial = rar + ".partial"
            try:
                download(PACK_URL, partial)
                if sha1_of(partial) != PACK_SHA1:
                    raise Failure("the download's SHA-1 doesn't match")
                os.replace(partial, rar)
            except OSError as error:
                raise Failure("download failed (%s)" % error)
            finally:
                if os.path.exists(partial):
                    os.remove(partial)
        remove_tree(unpack)
        os.makedirs(unpack)
        unpack_rar(rar, unpack)
        # The textures sit in one game-ID folder (GIHP78).
        folders = sorted(e.path for e in os.scandir(unpack) if e.is_dir())
        folder = folders[0] if folders else None
        if not folder or not any(e.name.lower().endswith(".png") and e.is_file() for e in os.scandir(folder)):
            raise Failure("no texture folder in the RAR")
        os.rename(folder, pack_source)
        os.remove(rar)
        say("HD pack: unpacked to " + pack_source)
    except Exception as error:
        raise Failure("HD pack: %s. Download it by hand from %s and put the PNGs in %s" % (error, PACK_URL, pack_source))
    finally:
        remove_tree(unpack)


def main():
    pack_source = os.path.join(ROOT, "texture_pack", "ScoobyHD")
    if os.path.isdir(pack_source):
        say("HD pack: %s is there already" % pack_source)
        return 0
    try:
        download_pack(pack_source)
    except Failure as error:
        say("ERROR: %s" % error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
