# FrightWorks: Night of 100 Frights

A statically recompiled PC port of **Scooby-Doo! Night of 100 Frights** (GameCube, 2002). The game's PowerPC code is
translated ahead of time to C++ with [WiiCompiled](https://github.com/patchzyy/Wiicompiled) and compiled into a Windows
program, so there is no CPU emulator; the console's graphics, audio and system calls are reimplemented underneath.

FrightWorks continues [Scooby-Doo: Refrightened](https://github.com/josiahg2502-glitch/Scooby-Doo-Refrightened) by JosiahG,
from which it split off on October 31, 2026. This repository is the GameCube version; ports of the other console versions
may follow.

[![Listed on PlayPorted](https://playported.com/badge.svg)](https://playported.com/ports/1163-scooby-doo-night-of-100-frights/)

> **Artists wanted:** the launcher needs new art in the style of the original show. [Details below](#wanted-an-artist-for-the-launcher).

![Mystic Manor in widescreen at 4K](docs/screenshots/courtyard.jpg)

<p>
  <img src="docs/screenshots/cinematic.jpg" width="32%" alt="Cinematic 2.39:1">
  <img src="docs/screenshots/menu.jpg" width="32%" alt="Main menu in widescreen">
  <img src="docs/screenshots/pause-ultrawide.jpg" width="32%" alt="Pause menu at 21:9 with Xbox button prompts">
</p>

## Features

- Up to 4K, true widescreen (16:9, fill, cinematic 2.39:1) or original 4:3
- 30 or 60 fps, MSAA anti-aliasing, motion blur, ambient occlusion, cartoon ink outlines, bloom (experimental), HDR
- Added Creeper window animation from the XBOX edition of the game to the Mystic Manor's Mystery Machine courtyard
- Built-in HD texture pack (can be switched off)
- Gamepads and keyboard, remappable in the launcher (**Settings** > **Controls**) or in game (**F10** > Controls);
  button prompts match your Xbox, PlayStation or Nintendo controller and follow remapped face buttons
- Optional original Xbox controls: X head smash, B dig / talk and back in menus, LB soap bubble, RB sneak (together with
  the triggers, as on the Xbox); the button prompts and tutorial hints follow. Black and White can also go on other
  buttons. The launcher can automatically match Xbox controllers to Original Xbox and GameCube adapters to GameCube,
  until you choose a layout manually; existing saved layouts ask once before opting in
- GameCube memory-card saves
- Option to skip the intro movies; mods that replace game files or textures, or that [add things to a level](docs/mod-patches.md)
  using your own disc, so no game data is shared
- The game's holiday events follow the real date, or pick a holiday, confetti fireworks or none of them on the launcher's **Extras** page

![Scooby watching confetti fireworks over Mystic Manor](docs/screenshots/confetti-fireworks.gif)

**No game data is included.** You need your own disc image.

The courtyard animation reuses the retained GameCube textures and their original timing, keeping the
GameCube window colors. No Xbox image or imported assets are needed; existing nonempty material-map mods
are left unchanged.

## Playing

Requires Windows 10/11 (64-bit) and **Night of 100 Frights, GameCube USA v1.01 (GIHE78, revision 1)** as `.iso`,
`.gcm` or `.ciso`.

1. From the [latest release](https://github.com/FrightWorks/FrightWorks-no100f/releases/latest), run
   `FrightWorks-win-Setup.exe` (installs for your Windows user, no admin rights; Start menu and desktop shortcuts), or
   unzip `FrightWorks-<version>.zip` anywhere if you'd rather keep everything in one folder.
2. Open **FrightWorks** and press **Play**. The first time, it asks for your disc image (**Extras**).

Windows may warn that the setup is from an unknown publisher (it isn't code-signed): **More info** > **Run anyway**.

**Linux (beta).** The release also has `FrightWorks.AppImage` (x86_64, built to run on any Linux with glibc 2.34 or
newer: Ubuntu 22.04 and newer, Debian 12, Fedora, Arch, SteamOS). Make it executable (`chmod +x FrightWorks.AppImage`) and run it; it asks
for your disc image like the Windows version and updates itself from the releases the same way. It needs a Vulkan driver
for your GPU, and FUSE to start (`fusermount` or `fusermount3`, from the fuse or fuse3 package, which desktops usually
have; `libfuse2` isn't needed); without FUSE run `./FrightWorks.AppImage --appimage-extract-and-run`.
Your saves, settings and game files are in `~/.local/share/WiiCompiled`, screenshots (**F2**) in `~/Pictures/FrightWorks`.
It is a beta: [tell us what happens](https://github.com/FrightWorks/FrightWorks-no100f/issues) on your setup.

**Updates.** The installed game looks for a new version when the launcher starts and asks before updating (**Extras >
Updates** turns that off, or checks right away). Updating keeps your saves and settings. The zip can't update itself:
its launcher tells you when a new release is out; unzip it over the old folder.

**Where your files are**

| | Installed | Zip |
| --- | --- | --- |
| Saves | `Saved Games\FrightWorks` | `data\UserData\Saves` |
| Settings, mods, logs, game files from your disc | `%LOCALAPPDATA%\FrightWorks-Data` | `data\UserData`, `data\Disc` |
| Screenshots (**F2**) / videos (**F3**) | `Pictures\FrightWorks` / `Videos\FrightWorks` (Linux AppImage: the same names in your Pictures and Videos folders) | `Screenshots` / `Videos` beside the launcher |

**Extras > Folders** opens these, can put the saves in another folder (say, one that's backed up), and copies saves and
settings over from an earlier FrightWorks or Refrightened folder. Uninstalling (Windows Settings > Apps) removes the
program only; your saves and settings stay.

Settings are in the launcher (**Settings**) and in game (**F10**, or the controller's **Back** / **Select** button, as
long as no mapping uses it). **F11** toggles fullscreen, **F2** saves a
screenshot and **F1** opens this page in your browser. The title screen and main menu list
these keys in the corner. With more than one monitor, **Settings** > **Display** > **Fullscreen monitor** (or **F10** >
Video) picks the one fullscreen uses.

**Launcher controller navigation.** D-pad or left stick moves focus, South confirms (Xbox A, PlayStation Cross,
Nintendo B), East goes back, the bumpers (LB/RB) switch tabs where a page has them (Settings: Display and Controls)
and otherwise move to the previous/next control, and right-stick Y scrolls. Sliders and numeric
fields use left/right. This works through Settings, Mods, Extras, owned questions, local file/folder browsers and
on-screen text entry, independently of the game's bindings. Input is ignored while another window owns focus or the
game owns the controllers; reconnecting and returning from a dialog/game require neutral first.

**Automatic layout.** Settings > Controls can match reliably identified Xbox controllers (including Xbox One) to the
existing **Original Xbox** scheme, and GameCube adapters to **GameCube**. PlayStation, Switch, unknown or conflicting
connected types leave the current scheme alone. A manual choice, even the current value, stops matching; the checkbox
can re-enable it. Custom bindings are kept. An older saved layout is left alone unless you accept the one-time opt-in.
New native F10 layout choices also stop matching; older game binaries can only be detected when they change the saved
layout value, not when they choose the same value again.

Controller-operated pickers use the launcher's themed local browser, including folder creation and path entry.
Mouse/keyboard retain native storage pickers (including Linux portals). A native picker opened with the mouse needs
OS keyboard/mouse input: cancel it and invoke the launcher action with the controller to use the local browser.
The on-screen keyboard enters ASCII; existing Unicode text is preserved and physical keyboards can enter other characters.

For testing: **Extras > Developer tools** in the launcher turns on the **F8** dev menu (go to any level, saved spots,
free camera, god mode, power-ups on/off, texture finder), **F3** video recording of the game (needs
[ffmpeg](https://ffmpeg.org): the launcher offers to download it when you turn the tools on) and a **DEV** mark with the
build in the corner of the game. The build (version,
branch and commit of a test build) is also on the F10 **About** page and at the top of `console.log`.

Coming from an earlier zip of FrightWorks or from Scooby-Doo: Refrightened? On first start, the launcher looks for the
old folder (beside the new zip, or for the installed game in Downloads, Desktop and Documents) and offers to copy your
saves, settings, controls, mods and game files over; the old folder is left as it is. If it isn't found, use **Extras >
Folders > Copy from an earlier version**.

**Version numbers** continue from Scooby-Doo: Refrightened. Its last release, v1.0.2, is the commit FrightWorks split
off from, so FrightWorks starts at v1.0.3.

![The launcher](docs/screenshots/launcher.gif)

## Wanted: an artist for the launcher

The launcher's haunted-mansion art is a placeholder. It was made with AI to work out how the launcher should look and
behave: the signs are the menu, the windows light up, curtains move and silhouettes pass by. It does the job, but the
goal is to have a real artist redraw it, closer to the look of the original *Scooby-Doo, Where Are You!*: painted
backgrounds, spooky colors, a creepy old mansion on a stormy night.

What's needed:

- The mansion on a transparent background, with the window glass cut out (the launcher draws the lights, curtains and
  silhouettes behind the house)
- Five signs for the menu
- Silhouettes for the windows. They shouldn't show the characters directly, but should look a bit like them: mainly
  Scooby and Shaggy, and the rest of the gang is welcome too. One that looks like the Mastermind would be great, and
  any of the monsters are welcome.

The current pieces are in [`launcher/art`](launcher/art), so you can see the sizes and layout.

The artist will be credited here, on the launcher's About page, on the in-game credits screen and in the release
notes. If you're interested, email [therider@unmarkedrider.com](mailto:therider@unmarkedrider.com).

## Building

Windows 10/11 x64 with Python 3.8 or newer and Git. The build downloads its own toolchain into the repo.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
& "launcher\publish\FrightWorks.exe" --extract "C:\path\to\game.iso" Disc\DATA
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1
powershell -ExecutionPolicy Bypass -File tools\package-scooby.ps1 -Zip          # or -Installer (Velopack)
```

The game builds to `Play\`; the package goes to `dist\`. The first build downloads dogerki's HD texture pack
(604 MB, from the [Internet Archive](https://archive.org/details/dolphin-scooby-doo-night-of-100-frights-hd-texture-pack))
into `texture_pack\ScoobyHD` (not in git) and adds the HD Creeper frames from `tools\hd-pack-additions`. Every build
copies the pack and the button prompt icons into `Play\` beside the game; the icons need Python and ImageMagick.

**Linux** (x86_64, a native build from source; the release has an AppImage, see Playing). Needs a recent clang (18 builds it, 14 does not; 15 to 17
untried), Ninja, Python 3 with Pillow, ImageMagick (7's `magick`, or the ImageMagick 6 that Ubuntu packages, with
`librsvg2-bin`), curl with CA certificates, rsync and bsdtar (or 7z / unrar), plus the headers SDL3 is built from (X11;
ALSA for sound); SDL3 for controller detection in the launcher. CMake and the .NET 10 SDK are downloaded into
`toolchain/`. Ubuntu 24.04 (clang 18):

```sh
sudo apt install clang ninja-build python3 python3-pil imagemagick librsvg2-bin curl ca-certificates rsync \
    libarchive-tools pkg-config libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev \
    libxtst-dev libasound2-dev
```

Ubuntu 22.04's clang is 14, which fails on the code's C++20 (lambda capture of a structured binding, parenthesized
aggregate initialization, `std::source_location`): add LLVM's apt repository (<https://apt.llvm.org>), install
`clang-18 libstdc++-12-dev` instead of `clang`, and make `clang` and `clang++` on PATH point to clang-18 (the script
calls them by those names). These packages build SDL3 for X11 only (no Wayland headers: not checked) and the game needs
a Vulkan driver to run.

```sh
tools/build-no100f-linux.sh /path/to/game.iso   # extracts the disc the first time, then builds the game into Play/
tools/build-launcher.sh                         # the launcher: launcher/publish/FrightWorks
launcher/publish/FrightWorks                    # or start the game directly: "Play/FrightWorks Game"
```

On Linux the game doesn't record F3 videos or pick a fullscreen monitor yet, so the launcher leaves those out.

Tests, playtest sessions and the debug tools (dev menu, logs, scripted input): [`docs/testing.md`](docs/testing.md).

**Using an AI agent on this repo?** Have it read [`.agents/REPO.md`](.agents/REPO.md) first: it holds the repo's rules and how to test, and skills are in `.agents/skills/`.

## Credits

- FrightWorks: Loneranger419
- Fright Wrangler, Master of Mysteries, and Scooby Snack Technician - [BitsofBitsofBits](https://github.com/BitsofBitsofBitsofBits)
- Original port, [Scooby-Doo: Refrightened](https://github.com/josiahg2502-glitch/Scooby-Doo-Refrightened): JosiahG
- [WiiCompiled](https://github.com/patchzyy/Wiicompiled) by patchzyy and contributors (GPLv3); Aurora by Luke Street
  (MIT); other components in [`wiicompiled/THIRD-PARTY-NOTICES.md`](wiicompiled/THIRD-PARTY-NOTICES.md)
- HD texture pack by **dogerki**
  ([Dolphin forums](https://forums.dolphin-emu.org/Thread-scooby-doo-night-of-100-frights-hd-texture-pack-ai))
- Button prompts from [Kenney's Input Prompts](https://kenney.nl/assets/input-prompts) (CC0)
- Launcher and in-game menu lettering: [Luckiest Guy](https://fonts.google.com/specimen/Luckiest+Guy) by Astigmatic
  (Apache 2.0); in-game menu text: [Roboto](https://fonts.google.com/specimen/Roboto) by Christian Robertson (Apache 2.0)

## License

GPLv3 ([`LICENSE`](LICENSE)); bundled components keep their own licenses. FrightWorks is a modified version of
Scooby-Doo: Refrightened by JosiahG, changed by Loneranger419 from 2026-10-31 on (the git history lists every change).

Not covered by the GPL (these stay under their owners' terms):

- `launcher/fonts/LuckiestGuy-Regular.ttf` and `wiicompiled/runtime/assets/fonts/LuckiestGuy-Regular.ttf`: the Luckiest
  Guy font by Astigmatic, Apache License 2.0 ([`launcher/fonts/LuckiestGuy-LICENSE.txt`](launcher/fonts/LuckiestGuy-LICENSE.txt))
- `wiicompiled/runtime/assets/fonts/Roboto-Medium.ttf`: the Roboto font by Christian Robertson, Apache License 2.0
- `docs/`: screenshots of the game and the launcher

Scooby-Doo ™ & © Hanna-Barbera / Warner Bros. Night of 100 Frights © THQ / Warner Bros. Interactive, developed by
Heavy Iron Studios. Unofficial fan project, not affiliated with or endorsed by them. The repository contains no game
code or assets; the game's data is read from your own copy.
