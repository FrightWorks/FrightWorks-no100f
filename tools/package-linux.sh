#!/usr/bin/env bash
# Linux release package: an AppImage made with Velopack (vpk), the Linux counterpart of `package-scooby.ps1 -Installer`.
#   tools/package-linux.sh [--build] [--version <v>] [--feed <dir>] [--no-download]
# Needs the same things as tools/build-no100f-linux.sh (clang 18, Ninja, Python 3 with Pillow, ImageMagick, curl, bsdtar,
# the X11 and ALSA headers) plus squashfs-tools, zstd and patchelf for vpk and the packaging. The .NET 10 SDK is downloaded into toolchain/ and vpk into
# dist/.vpk. tools/package-linux-docker.ps1 runs this in an Ubuntu 22.04 container (from Windows, or anywhere with Docker),
# which is how the release package is made: the build machine's glibc sets the oldest Linux the package runs on.
#
# --build first builds the game (tools/build-no100f-linux.sh) and the launcher (tools/build-launcher.sh); without it the
#   Play/ and launcher/publish/ already there are packed.
# The package holds the same program files as the Windows installer: the launcher on top ("FrightWorks"), the game and its
# runtime files in data/ (no portable marker, no disc, saves or settings). Velopack puts it in an AppImage; the installed
# game keeps the player's files outside it (launcher/DataPaths.cs: $XDG_DATA_HOME/WiiCompiled).
# Into --feed (default dist/installer-linux): FrightWorks-linux.AppImage, FrightWorks-<ver>-linux-full.nupkg, a -delta.nupkg
# against the previous Linux release when one exists, and releases.linux.json / assets.linux.json, which the installed
# launcher reads from GitHub Releases. The Linux channel ("linux") has its own files, so they sit beside the Windows ones
# in a release. Unless --no-download, the latest release's Linux package is downloaded into the feed first for the delta
# (none yet before the first Linux release: fine).
# --version defaults to <Version> in the launcher's .csproj. Uploading is a separate step (docs/testing.md).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
vpk_version=1.2.161  # keep in step with the Velopack PackageReference in the launcher's .csproj and with package-scooby.ps1
repo_url=https://github.com/FrightWorks/FrightWorks-no100f

build=0 download=1 version="" feed=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) build=1 ;;
        --no-download) download=0 ;;
        --version) version="${2:?--version needs a value}"; shift ;;
        --feed) feed="${2:?--feed needs a folder}"; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
if [[ -z "$version" ]]; then
    version="$(sed -n 's|.*<Version>\([^<]*\)</Version>.*|\1|p' "$root/launcher/FrightWorks.Launcher.csproj" | head -n 1)"
fi
[[ -n "$version" ]] || { echo "No <Version> in launcher/FrightWorks.Launcher.csproj." >&2; exit 1; }
feed="${feed:-$root/dist/installer-linux}"

if [[ $build -eq 1 ]]; then
    "$root/tools/build-no100f-linux.sh"
    "$root/tools/build-launcher.sh"
fi
launcher="$root/launcher/publish/FrightWorks"
play="$root/Play"
[[ -x "$launcher" ]] || { echo "No $launcher: run with --build." >&2; exit 1; }
[[ -x "$play/FrightWorks Game" ]] || { echo "No $play/FrightWorks Game: run with --build." >&2; exit 1; }
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Program files only, as for the Windows installer: no portable.txt (the game then keeps the player's files in
# $XDG_DATA_HOME/WiiCompiled), no UserData, no zip README.
stage="$work/FrightWorks"
mkdir -p "$stage/data"
install -m 755 "$launcher" "$stage/FrightWorks"
install -m 755 "$play/FrightWorks Game" "$stage/data/"
for f in dsp_coef.bin cacert.pem initial_pipeline_cache.db; do
    [[ -f "$play/$f" ]] && install -m 644 "$play/$f" "$stage/data/"
done
# The game's own libraries: the build links libpng and zlib as shared libraries from its build folder (a RUNPATH there, so a
# source build runs from Play/). A player's system may not have them or may have another version, so the ones the game was
# built with go beside it and the game and they look there first ($ORIGIN). SDL3 and Dawn are linked statically.
game="$stage/data/FrightWorks Game"
runpath="$(patchelf --print-rpath "$game")"
for lib in $(patchelf --print-needed "$game"); do
    IFS=: read -r -a dirs <<<"$runpath"
    for dir in "${dirs[@]}"; do
        if [[ -n "$dir" && -e "$dir/$lib" ]]; then
            install -m 755 "$(readlink -f "$dir/$lib")" "$stage/data/$lib"
            patchelf --set-rpath '$ORIGIN' "$stage/data/$lib"
            echo "Bundled $lib (from the build folder)"
            break
        fi
    done
done
patchelf --set-rpath '$ORIGIN' "$game"

# libSDL3.so.0 for the launcher's controller detection (launcher/Gamepads.cs loads the one beside the game, else the system's,
# and most distributions don't have SDL3 yet; the Windows package ships SDL3.dll for the same reason). The game has SDL
# built in, so this is the same vendored SDL source built as a shared library, with the same compiler.
sdl_src="$root/wiicompiled/native-build-linux/_deps/sdl-src"
sdl_build="$root/wiicompiled/native-build-linux/sdl-shared"
cmake="$root/toolchain/CMake/bin/cmake"
[[ -d "$sdl_src" ]] || { echo "No $sdl_src: run with --build." >&2; exit 1; }
"$cmake" -S "$sdl_src" -B "$sdl_build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release     -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF >"$work/sdl-configure.log" 2>&1 ||
    { cat "$work/sdl-configure.log"; exit 1; }
"$cmake" --build "$sdl_build" --parallel "${FRIGHTWORKS_JOBS:-$(nproc)}" >"$work/sdl-build.log" 2>&1 || { tail -n 40 "$work/sdl-build.log"; exit 1; }
install -m 755 "$(readlink -f "$sdl_build/libSDL3.so.0")" "$stage/data/libSDL3.so.0"
echo "Bundled libSDL3.so.0 (SDL built from the vendored source)"
# The built-in HD texture pack and the button prompt icons: the same files every Play/ build gets, beside the game.
python3 "$root/tools/stage-assets.py" --game-dir "$stage/data" --required

# What the game and the launcher load at run time must be on the system or beside the game: list anything ldd can't find (the
# GPU stack, libvulkan and the drivers, is dlopen'd and never listed). Also say what glibc they need.
missing=0
for exe in "$stage/data/FrightWorks Game" "$stage/FrightWorks"; do
    if ldd "$exe" 2>&1 | grep -q 'not found'; then
        echo "Not found by ldd for $(basename "$exe"):" >&2; ldd "$exe" 2>&1 | grep 'not found' >&2; missing=1
    fi
done
[[ $missing -eq 0 ]] || { echo "The package needs libraries that are not on this machine." >&2; exit 1; }
for f in "$stage/data/FrightWorks Game" "$stage/data/"*.so* "$stage/FrightWorks"; do
    [[ -f "$f" ]] && echo "glibc needed by $(basename "$f"): $(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)"
done

# The icon: vpk wants a PNG on Linux; the launcher's .ico holds several sizes, the largest becomes the icon.
python3 - "$root/launcher/frightworks.ico" "$work/frightworks.png" <<'PY'
import sys
from PIL import Image
ico = Image.open(sys.argv[1])
sizes = sorted(ico.info.get("sizes") or {ico.size})
ico.size = sizes[-1]
ico.load()
ico.convert("RGBA").save(sys.argv[2])
PY

# vpk, a .NET tool, pinned and kept in dist/.vpk; it runs on the .NET 10 SDK in toolchain/dotnet.
dotnet="$(python3 "$root/tools/get-dotnet.py" | tail -n 1)"
export DOTNET_ROOT="$(dirname "$dotnet")"
vpk_dir="$root/dist/.vpk/$vpk_version"
vpk="$vpk_dir/vpk"
if [[ ! -x "$vpk" ]]; then
    "$dotnet" tool install vpk --version "$vpk_version" --tool-path "$vpk_dir"
fi

mkdir -p "$feed"
if [[ $download -eq 1 ]]; then
    # The latest release's Linux package, for the delta. Before the first Linux release there is none: vpk says so and
    # that is fine; any other failure (no network) stops here so a release is never made without its delta by accident.
    if ! "$vpk" download github --repoUrl "$repo_url" --channel linux --outputDir "$feed" >"$work/download.log" 2>&1; then
        cat "$work/download.log"
        if grep -qiE "no (releases|assets)|not found|no .*found" "$work/download.log"; then
            echo "No earlier Linux release to make a delta from: the package will be full only."
        else
            echo "vpk download github failed" >&2; exit 1
        fi
    else
        cat "$work/download.log"
    fi
fi

"$vpk" pack --packId FrightWorks --packVersion "$version" --packDir "$stage" --mainExe FrightWorks --runtime linux-x64 \
    --packTitle FrightWorks --packAuthors Loneranger419 --icon "$work/frightworks.png" --categories Game \
    --outputDir "$feed"

echo "DONE: $feed (FrightWorks $version)"
ls -l "$feed" | sed 1d | awk '{ printf "  %s  %.0f MB\n", $NF, $5 / 1048576 }'
