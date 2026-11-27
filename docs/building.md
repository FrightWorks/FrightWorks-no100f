# Building

Building needs your own disc image too: the build reads the game from it. Tests and debugging: [testing.md](testing.md).

## Windows

Windows 10/11 x64 with Python 3.8 or newer and Git. The build downloads its own toolchain into the repo.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
& "launcher\publish\FrightWorks-No100F-Launcher.exe" --extract "C:\path\to\game.iso" Disc\DATA
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1
powershell -ExecutionPolicy Bypass -File tools\package-scooby.ps1 -Zip          # or -Installer (Velopack)
```

The game builds to `Play\`; packages go to `dist\`. Every build copies the button prompt icons into `Play\`; they
need Python and ImageMagick. No build includes an HD texture pack: players download one in the launcher (**Extras > HD
texture packs**) into `UserData\texture_packs`.

## Linux

x86_64, a native build from source (the release AppImage is built in Docker: [testing.md](testing.md)). Needs:

- clang 18 (14 fails; 15 to 17 untried), Ninja, Python 3 with Pillow
- ImageMagick (7's `magick`, or Ubuntu's ImageMagick 6 with `librsvg2-bin`)
- curl with CA certificates, rsync, and bsdtar (or 7z / unrar)
- the headers SDL3 builds from (X11; ALSA for sound), and SDL3 for the launcher's controller detection
- a Vulkan driver to run the game

CMake and the .NET 10 SDK are downloaded into `toolchain/`. On Ubuntu 24.04:

```sh
sudo apt install clang ninja-build python3 python3-pil imagemagick librsvg2-bin curl ca-certificates rsync \
    libarchive-tools pkg-config libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev \
    libxtst-dev libasound2-dev
```

Ubuntu 22.04's clang 14 fails on the code's C++20. Add [LLVM's apt repository](https://apt.llvm.org), install
`clang-18 libstdc++-12-dev` instead of `clang`, and point `clang` and `clang++` on PATH at clang-18 (the script calls
them by those names). These packages build SDL3 for X11 only.

```sh
tools/build-no100f-linux.sh /path/to/game.iso   # extracts the disc the first time, then builds the game into Play/
tools/build-launcher.sh                         # the launcher: launcher/publish/FrightWorks-No100F-Launcher
launcher/publish/FrightWorks-No100F-Launcher                    # or start the game directly: "Play/FrightWorks-No100F-Game"
```
