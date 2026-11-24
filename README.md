# FrightWorks: Night of 100 Frights

A native PC port of **Scooby-Doo! Night of 100 Frights** (GameCube, 2002) for Windows and Linux. The game's PowerPC code
is recompiled ahead of time to C++ with [WiiCompiled](https://github.com/patchzyy/Wiicompiled), so there is no CPU
emulator: the console's graphics, audio and system calls are reimplemented underneath.

FrightWorks continues [Scooby-Doo: Refrightened](https://github.com/josiahg2502-glitch/Scooby-Doo-Refrightened) by JosiahG,
which it split off from on October 31, 2026 (version numbers carry on from its v1.0.2).

[![Listed on PlayPorted](https://playported.com/badge.svg)](https://playported.com/ports/1163-scooby-doo-night-of-100-frights/)

> **Artists wanted:** the launcher needs new art in the style of the original show. [Details](docs/launcher-art.md).

![Mystic Manor in widescreen at 4K](docs/screenshots/courtyard.jpg)

<p>
  <img src="docs/screenshots/cinematic.jpg" width="32%" alt="Cinematic 2.39:1">
  <img src="docs/screenshots/menu.jpg" width="32%" alt="Main menu in widescreen">
  <img src="docs/screenshots/pause-ultrawide.jpg" width="32%" alt="Pause menu at 21:9 with Xbox button prompts">
</p>

## Features

**Graphics**
- Up to 4K, in true widescreen (16:9, fill, cinematic 2.39:1) or the original 4:3
- 30 or 60 fps, MSAA, HDR with brightness calibration
- Motion blur and cartoon ink outlines; ambient occlusion and bloom (experimental)
- Optional HD texture pack by dogerki, downloaded from the launcher when you choose it (Extras > HD texture packs)
- The Xbox version's Creeper window animation in the Mystic Manor courtyard

**Controls**
- Gamepads and keyboard, remappable in the launcher or in game; button prompts match your Xbox, PlayStation or
  Nintendo controller
- Optional original Xbox button layout, picked automatically for Xbox controllers if you like
- The launcher works fully with a controller

**Extras**
- GameCube memory-card saves; saves from Refrightened and earlier versions are brought over
- Skip the intro movies
- Holiday events that follow the real date, or pick one (or confetti fireworks)
- Mods that replace game files or textures, and [patch mods](docs/mod-patches.md) that add things to a level without
  sharing game data
- **From your own PS2 or Xbox disc (optional):** the live story cutscenes instead of the GameCube's movies (the Xbox
  disc also has the ending), and from an Xbox disc its uncompressed textures and hiss-free sound.
  [More](docs/ps2-xbox-extras.md)

![Scooby watching confetti fireworks over Mystic Manor](docs/screenshots/confetti-fireworks.gif)

## Getting started

**No game data is included.** You need your own **GameCube USA v1.01** disc image (Game ID `GIHE78`, revision 1) as
`.iso`, `.gcm` or `.ciso`. The launcher checks it for you; see [which disc version do I have?](docs/disc.md)

1. Download from the [latest release](https://github.com/FrightWorks/FrightWorks-no100f/releases/latest):
   - **Windows 10/11 (64-bit):** `FrightWorks-win-Setup.exe` (no admin rights; updates itself), or the zip to keep
     everything in one folder. Windows may warn about an unknown publisher: **More info** > **Run anyway**.
   - **Linux (beta):** `FrightWorks.AppImage` (x86_64, glibc 2.34+, Vulkan). `chmod +x` it and run it.
2. Open **FrightWorks** and press **Play**. The first time, it asks for your disc image.

| Key | |
| --- | --- |
| **F10** (or the controller's **Back**/**Select**) | Settings |
| **F11** | Fullscreen |
| **F2** / **F3** | Screenshot / video (video needs Developer tools) |
| **F8** | Dev menu (needs **Extras > Developer tools**) |
| **F1** | Opens this page |

More in the [player's guide](docs/playing.md): file locations, updates, Linux, controllers, moving from an earlier
version, developer tools.

![The launcher](docs/screenshots/launcher.gif)

## Building and contributing

- [Building](docs/building.md) on Windows or Linux
- [Testing and debugging](docs/testing.md)
- [Patch mod format](docs/mod-patches.md)
- Using an AI agent? Have it read [`.agents/REPO.md`](.agents/REPO.md) first: the repo's rules and how to test.

## Credits

- FrightWorks: Loneranger419
- Fright Wrangler, Master of Mysteries, and Scooby Snack Technician - [BitsofBitsofBits](https://github.com/BitsofBitsofBitsofBits)
- Linux port, the launcher for Windows and Linux, motion blur, ambient occlusion, ink outlines and bloom -
  [Junkaholic55](https://github.com/Junkaholic55)
- Original port, [Scooby-Doo: Refrightened](https://github.com/josiahg2502-glitch/Scooby-Doo-Refrightened): JosiahG
- [WiiCompiled](https://github.com/patchzyy/Wiicompiled) by patchzyy and contributors (GPLv3); Aurora by Luke Street
  (MIT); other components in [`wiicompiled/THIRD-PARTY-NOTICES.md`](wiicompiled/THIRD-PARTY-NOTICES.md)
- HD texture pack (optional download) by **dogerki**
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

## Other FrightWorks projects

[COMING SOON! FrightWorks: UnMasked](https://github.com/FrightWorks/FrightWorks-unmasked)
