#!/usr/bin/env python3
"""Populates buildtools/artifacts/dependencies (or --destination): the pinned, unpatched upstream source trees (plus the
prebuilt, patched Dawn package and generated C++/WinRT headers) that the native build uses with FETCHCONTENT_FULLY_DISCONNECTED=ON.
Every pin is the one aurora-main's own CMake declares; the "pins" below are asserted against those files so the two
cannot drift apart silently.

    python wiicompiled/buildtools/Prepare-Dependencies.py [--destination <folder>] [--cppwinrt-input local|sdk|<version>]

(Prepare-Dependencies.ps1 runs this on Windows.) --cppwinrt-input is the Windows metadata source for cppwinrt.exe:
'local' (this machine's WinMetadata), 'sdk', or an installed SDK version such as 10.0.26100.0.
The source trees work anywhere. The Dawn package is the Windows amd64 build and the C++/WinRT headers come from a Windows
program: on other systems both are skipped (with a note). Python 3.8+, standard library only. Failures print
"ERROR: <text>" and exit 1.
"""
import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOWS = sys.platform == "win32"
REPO = os.path.dirname(HERE)                      # wiicompiled/
CMAKE = os.path.join(REPO, "aurora-main", "CMakeLists.txt")
EXTERN = os.path.join(REPO, "aurora-main", "extern", "CMakeLists.txt")
DAWN = os.path.join(REPO, "aurora-main", "cmake", "AuroraDawnProvider.cmake")
LIBUSB = os.path.join(REPO, "aurora-main", "cmake", "AuroraLibUSB.cmake")
SDL = os.path.join(REPO, "aurora-main", "cmake", "AuroraSDL3Provider.cmake")

# name = directory under the destination; FetchContent maps it back through FETCHCONTENT_SOURCE_DIR_<UPPERCASE NAME>
# (NativeBuildFlags.ps1), so the names are the declared FetchContent names, not the upstream project names.
# pins = (file, text that file must still contain).
# sha256 = digest of the downloaded file (checked on every use, so a mutable release asset or a stale cache can't slip
# through); runtime = (path in the tree, digest) of a file that proves the extracted tree is the right one, so an older
# Dependencies folder holding another build of the package is replaced.
PACKAGES = [
    dict(name="SDL", file="SDL3-3.4.4.tar.gz",
         uris=["https://github.com/libsdl-org/SDL/releases/download/release-3.4.4/SDL3-3.4.4.tar.gz"],
         pins=[(CMAKE, 'set(AURORA_SDL3_VERSION "3.4.4"'),
               (SDL, 'releases/download/release-${AURORA_SDL3_VERSION}/SDL3-${AURORA_SDL3_VERSION}.tar.gz')]),
    dict(name="abseil-cpp", file="abseil-cpp-20240722.0.tar.gz",
         uris=["https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz"],
         pins=[(EXTERN, "https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz")]),
    dict(name="dawn_prebuilt", file="dawn-v20260603.191052-msaa-windows-amd64.tar.gz", windows_only=True,
         sha256="67c507f218736f750be8d61c7f7a444bedafcfd6b184bc55a07961bfd001151e",
         runtime=("bin/webgpu_dawn.dll", "84caa209a4dd34656bdc24dc97affc3e1484e20a00e371ff5fddeb266d9ffb64"),
         uris=["https://github.com/FrightWorks/dawn-build/releases/download/v20260603.191052-msaa/"
               "dawn-windows-amd64.tar.gz"],
         pins=[(CMAKE, 'set(AURORA_DAWN_VERSION "v20260603.191052"'),
               (DAWN, "SHA256=67c507f218736f750be8d61c7f7a444bedafcfd6b184bc55a07961bfd001151e"),
               (DAWN, '"84caa209a4dd34656bdc24dc97affc3e1484e20a00e371ff5fddeb266d9ffb64"'),
               (DAWN, "FrightWorks/dawn-build/releases/download/v20260603.191052-msaa/")]),
    dict(name="fmt", file="fmt-11.1.4.tar.gz",
         uris=["https://github.com/fmtlib/fmt/archive/refs/tags/11.1.4.tar.gz"],
         pins=[(EXTERN, "https://github.com/fmtlib/fmt/archive/refs/tags/11.1.4.tar.gz")]),
    dict(name="freetype", file="freetype-2.14.3.tar.gz",
         uris=["https://files.twilitrealm.dev/freetype-2.14.3.tar.gz",
               "https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.gz",
               "https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.gz"],
         pins=[(EXTERN, "https://files.twilitrealm.dev/freetype-2.14.3.tar.gz")]),
    dict(name="imgui", file="imgui-1.91.9b-docking.tar.gz",
         uris=["https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz"],
         pins=[(EXTERN, "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz")]),
    dict(name="libusb", file="libusb-1.0.30.tar.bz2",
         uris=["https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2"],
         pins=[(CMAKE, 'set(AURORA_LIBUSB_VERSION "1.0.30"'),
               (LIBUSB, "releases/download/v${AURORA_LIBUSB_VERSION}/libusb-${AURORA_LIBUSB_VERSION}.tar.bz2")]),
    dict(name="png", file="libpng-1.6.58.tar.gz",
         uris=["https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz"],
         pins=[(EXTERN, "https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz")]),
    dict(name="sqlite3", file="sqlite-amalgamation-3510300.zip",
         uris=["https://sqlite.org/2026/sqlite-amalgamation-3510300.zip"],
         pins=[(EXTERN, "https://sqlite.org/2026/sqlite-amalgamation-3510300.zip")]),
    dict(name="tracy", file="tracy-a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz",
         uris=["https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz"],
         pins=[(EXTERN, "https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz")]),
    dict(name="xxhash", file="xxHash-0.8.4.tar.gz",
         uris=["https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz"],
         pins=[(EXTERN, "https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz")]),
    dict(name="zlib", file="zlib-1.3.2.tar.gz",
         uris=["https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz"],
         pins=[(EXTERN, "https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz")]),
    dict(name="zstd", file="zstd-1.5.7.tar.gz",
         uris=["https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"],
         pins=[(EXTERN, "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz")]),
]

# The C++/WinRT compiler (NuGet package = zip) that generates the projection headers.
CPPWINRT_TOOL = dict(name="cppwinrt-tool", file="Microsoft.Windows.CppWinRT.3.0.260818.1.nupkg",
                     uris=["https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/3.0.260818.1"], pins=[])


class Failure(Exception):
    """Stops the script (exit 1)."""


def say(text):
    print(text, flush=True)


def remove_tree(path):
    def writable(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path, onerror=writable)
    elif os.path.lexists(path):
        os.chmod(path, stat.S_IWRITE)
        os.remove(path)


def assert_pinned(package):
    for file, text in package["pins"]:
        if not os.path.isfile(file):
            raise Failure("Pin source is missing: %s" % file)
        with open(file, "rb") as stream:
            content = stream.read().decode("utf-8-sig", "replace")
        if text not in content:
            raise Failure("%s is pinned to '%s' here but %s no longer declares it; update both."
                          % (package["name"], text, file))


def download(url, path):
    request = urllib.request.Request(url, headers={"User-Agent": "FrightWorks-build-scripts"})
    with urllib.request.urlopen(request, timeout=60) as response, open(path, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def check_digest(package, path):
    expected = package.get("sha256")
    if expected and file_sha256(path) != expected:
        raise Failure("%s does not match the pinned SHA-256 %s: %s" % (package["name"], expected, path))


def tree_is_current(package, target):
    runtime = package.get("runtime")
    if not runtime:
        return True
    path = os.path.join(target, *runtime[0].split("/"))
    return os.path.isfile(path) and file_sha256(path) == runtime[1]


def get_archive(package, downloads):
    archive = os.path.join(downloads, package["file"])
    if os.path.isfile(archive):
        check_digest(package, archive)
        return archive
    temporary = archive + ".partial"
    failures = []
    for uri in package["uris"]:
        if os.path.lexists(temporary):
            os.remove(temporary)
        say("Downloading %s from %s..." % (package["name"], uri))
        try:
            download(uri, temporary)
        except Exception as error:      # network, HTTP status, disk: try the next mirror
            failures.append("%s (%s)" % (uri, error))
            continue
        try:
            check_digest(package, temporary)
        except Failure:
            os.remove(temporary)
            raise
        os.replace(temporary, archive)
        return archive
    if os.path.lexists(temporary):
        os.remove(temporary)
    raise Failure("%s could not be fetched: %s" % (package["name"], "; ".join(failures)))


def check_member(name):
    parts = name.replace("\\", "/").split("/")
    if name.startswith(("/", "\\")) or ".." in parts or (len(name) > 1 and name[1] == ":"):
        raise Failure("unsafe path in the archive: %s" % name)


def extract_archive(archive, extract):
    if archive.endswith((".zip", ".nupkg")):
        with zipfile.ZipFile(archive) as bundle:
            for info in bundle.infolist():
                check_member(info.filename)
            bundle.extractall(extract)
            for info in bundle.infolist():          # keep the stored modification times, like tar does
                if not info.is_dir():
                    stamp = time.mktime(info.date_time + (0, 0, -1))
                    os.utime(os.path.join(extract, *info.filename.split("/")), (stamp, stamp))
        return
    with tarfile.open(archive) as bundle:
        # Symbolic links need a privilege Windows lacks; the only ones in these archives are zstd test aliases, and
        # none are build inputs. Skipped everywhere, so every system gets the same tree.
        members = []
        for member in bundle.getmembers():
            check_member(member.name)
            if not member.issym():
                members.append(member)
        if hasattr(tarfile, "data_filter"):
            bundle.extractall(extract, members=members, filter="data")
        else:
            bundle.extractall(extract, members=members)


def expand_package(archive, target):
    # Release archives wrap everything in one versioned directory (SDL3-3.4.4/, tracy-<sha>/) while dawn-build's package
    # is flat; either way the tree lands directly under the target.
    extract = os.path.join(os.path.dirname(target), ".extract-" + os.path.basename(target))
    remove_tree(extract)
    os.makedirs(extract)
    try:
        extract_archive(archive, extract)
    except (tarfile.TarError, zipfile.BadZipFile, OSError, EOFError) as error:
        raise Failure("Extracting %s failed: %s" % (archive, error))
    entries = os.listdir(extract)
    source = extract
    if len(entries) == 1 and os.path.isdir(os.path.join(extract, entries[0])):
        source = os.path.join(extract, entries[0])
    remove_tree(target)
    os.rename(source, target)
    remove_tree(extract)


def prepare(destination, cppwinrt_input):
    downloads = os.path.join(HERE, "artifacts", "downloads")
    os.makedirs(destination, exist_ok=True)
    os.makedirs(downloads, exist_ok=True)

    for package in PACKAGES:
        assert_pinned(package)
        target = os.path.join(destination, package["name"])
        if package.get("windows_only") and not WINDOWS:
            say("Skipped %s: it's the Windows amd64 build" % package["name"])
            continue
        if os.path.isdir(target):
            if tree_is_current(package, target):
                continue
            say("Replacing %s: it is not the pinned build" % package["name"])
        expand_package(get_archive(package, downloads), target)
        say("Prepared " + package["name"])

    cppwinrt = os.path.join(destination, "cppwinrt")
    base_header = os.path.join(cppwinrt, "winrt", "base.h")
    if not os.path.isfile(base_header):
        if not WINDOWS:
            say("Skipped cppwinrt: cppwinrt.exe only runs on Windows")
        else:
            tool_root = os.path.join(HERE, "artifacts", "cppwinrt-tool")
            compiler = os.path.join(tool_root, "bin", "cppwinrt.exe")
            if not os.path.isfile(compiler):
                expand_package(get_archive(CPPWINRT_TOOL, downloads), tool_root)
            if not os.path.isfile(compiler):
                raise Failure("cppwinrt.exe is missing: %s" % compiler)
            remove_tree(cppwinrt)
            os.makedirs(cppwinrt)
            say("Generating C++/WinRT headers (-input %s)..." % cppwinrt_input)
            process = subprocess.Popen([compiler, "-input", cppwinrt_input, "-output", cppwinrt],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for raw in process.stdout:
                say(raw.decode("utf-8", "replace").rstrip("\r\n"))
            code = process.wait()
            if code != 0:
                raise Failure("cppwinrt.exe failed with exit code %d." % code)
            if not os.path.isfile(base_header):
                raise Failure("cppwinrt.exe produced no winrt\\base.h under %s" % cppwinrt)
            shutil.copyfile(os.path.join(tool_root, "LICENSE"), os.path.join(cppwinrt, "LICENSE.txt"))
            say("Prepared cppwinrt")

    say("Pinned dependency sources ready: " + destination)


def main(argv):
    parser = argparse.ArgumentParser(description="Fetches the pinned dependency source trees.")
    parser.add_argument("--destination", default=os.path.join(HERE, "artifacts", "dependencies"))
    parser.add_argument("--cppwinrt-input", default="local")
    args = parser.parse_args(argv)
    try:
        prepare(os.path.abspath(args.destination), args.cppwinrt_input)
    except (Failure, OSError) as error:
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
