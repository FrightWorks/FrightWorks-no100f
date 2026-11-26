# Player's guide

## Installing

From the [latest release](https://github.com/FrightWorks/FrightWorks-no100f/releases/latest):

- **Installer** (`FrightWorks-win-Setup.exe`): installs for your Windows user, no admin rights, with Start menu and
  desktop shortcuts. It isn't code-signed, so Windows may warn about an unknown publisher: **More info** > **Run
  anyway**. Uninstalling (Windows Settings > Apps) removes the program only; saves and settings stay.
- **Zip** (`FrightWorks-<version>.zip`): unzip anywhere; everything stays in that folder.
- **Linux AppImage (beta)**: see [Linux](#linux-beta).

Or install it from [RecompLauncher](https://recomplauncher.com), which installs and updates the zip, checks your disc
image, sets it up, shows the game's settings on its page and in its Big Picture mode, and starts the game directly.
It's the zip underneath, so your files are where the zip keeps them; if you also have the installed game, the first
setup copies its saves and settings over. The FrightWorks launcher in that folder still works for what RecompLauncher
doesn't do (HD texture packs, HDR brightness, button mapping, mods, PS2 and Xbox extras).

Open **FrightWorks** and press **Play**. The first time, it asks for your disc image ([which one](disc.md)).

## Updates

The installed game (and the AppImage) looks for a new version when the launcher starts and asks before updating;
**Extras > About** turns that off or checks right away. Updates keep your saves and settings. The zip can't update
itself: its launcher tells you when a release is out, and you unzip the new one over the old folder.

## Where your files are

| | Installed | Zip | Linux AppImage |
| --- | --- | --- | --- |
| Saves | `Saved Games\FrightWorks` | `data\UserData\Saves` | `~/.local/share/WiiCompiled` |
| Settings, mods, logs, files from your disc | `%LOCALAPPDATA%\FrightWorks-Data` | `data\UserData`, `data\Disc` | `~/.local/share/WiiCompiled` |
| Screenshots (**F2**) / videos (**F3**) | `Pictures\FrightWorks` / `Videos\FrightWorks` | `Screenshots` / `Videos` beside the launcher | `FrightWorks` in your Pictures / Videos folders |

**Extras > Folders** opens these, can move the saves to another folder (say, one that's backed up), and copies saves and
settings over from an earlier version.

## Coming from an earlier version or Refrightened

On first start, the launcher looks for an earlier FrightWorks zip or Scooby-Doo: Refrightened folder (beside the new
zip, or for the installed game in Downloads, Desktop and Documents) and offers to copy your saves, settings, controls,
mods and disc files over. The old folder is left as it is. If it isn't found: **Extras > Folders > Copy from an earlier
version**. Old saves are converted, with a backup.

## Settings and keys

Settings are in the launcher (**Settings**) and in game (**F10**, or the controller's **Back**/**Select** as long as no
mapping uses it). The title screen and main menu list the keys in a corner.

| Key | |
| --- | --- |
| **F10** | Settings |
| **F11** | Fullscreen / window |
| **F2** | Screenshot |
| **F3** | Video recording (Developer tools) |
| **F8** | Dev menu (Developer tools) |
| **F1** | The project page |

With more than one monitor, **Settings > Display > Fullscreen monitor** (or **F10** > Video) picks the one fullscreen
uses.

## Controllers and keyboard

Gamepads work right away. Remap them in the launcher (**Settings > Controls**) or in game (**F10** > Controls). Button
prompts match your Xbox, PlayStation or Nintendo controller and follow remapped face buttons.

Default keyboard: WASD move, arrows camera, Space A, Left Shift B, R X, F Y, C Z, Q/E L/R, Enter Start, IJKL D-pad.

**Original Xbox layout (optional).** X head smash, B dig / talk and back in menus, LB soap bubble, RB sneak (with the
triggers, as on the Xbox); the prompts and tutorial hints follow. Black and White can also go on other buttons.

**Automatic layout.** **Settings > Controls** can match Xbox controllers to the Original Xbox layout and GameCube
adapters to GameCube. Other or mixed controllers leave the layout alone, custom bindings are kept, and choosing a layout
yourself stops the matching (the checkbox turns it back on).

**The launcher with a controller.** D-pad or left stick moves, South confirms (Xbox A, PlayStation Cross, Nintendo B),
East goes back, the bumpers switch tabs (or move to the previous/next control), right stick scrolls, and left/right
change sliders. File and folder choosers open the launcher's own browser with an on-screen keyboard; with a mouse you get
the system's file picker.

## Linux (beta)

`FrightWorks.AppImage` runs on x86_64 Linux with glibc 2.34 or newer (Ubuntu 22.04+, Debian 12, Fedora, Arch, SteamOS).
Make it executable (`chmod +x FrightWorks.AppImage`) and run it. It needs a Vulkan driver, and FUSE to start
(`fusermount` or `fusermount3`, which desktops usually have); without FUSE run
`./FrightWorks.AppImage --appimage-extract-and-run`. Video recording and the fullscreen monitor choice aren't there yet.
[Tell us how it goes](https://github.com/FrightWorks/FrightWorks-no100f/issues) on your setup.

## Developer tools

**Extras > Game > Developer tools** turns on:

- the **F8** dev menu: go to any level, saved spots, free camera, god mode, power-ups on/off, texture finder;
- **F3** video recording (needs [ffmpeg](https://ffmpeg.org); the launcher offers to download it);
- a **DEV** mark with the build in the corner.

The build (version, and the branch and commit of a test build) is also on the F10 **About** page and at the top of
`console.log`. More for testers: [testing.md](testing.md).
