# Player's guide

## Installing

From the [latest release](https://github.com/FrightWorks/FrightWorks-no100f/releases/latest):

- **Installer** (`FrightWorks-win-Setup.exe`): installs for your Windows user, no admin rights, with Start menu and
  desktop shortcuts. It isn't code-signed, so Windows may warn about an unknown publisher: **More info** > **Run
  anyway**. Uninstalling (Windows Settings > Apps) removes the program only; saves and settings stay.
- **Zip** (`FrightWorks-<version>-Windows-x64.zip`; `FrightWorks-<version>.zip` up to 1.2.0): unzip anywhere;
  everything stays in that folder.
- **Linux (beta)**: the AppImage, or the portable `.tar.gz`; see [Linux](#linux-beta).

Or install it from [RecompLauncher](https://recomplauncher.com), which installs and updates the zip (on Linux, the
portable `.tar.gz`), checks your disc image, sets it up, shows the game's settings on its page and in its Big Picture
mode, and starts the game directly. It's the zip underneath, so your files are where the zip keeps them; if you also
have the installed game, the first setup copies its saves and settings over. The FrightWorks launcher in that folder
still works for what RecompLauncher doesn't do (HD texture packs, HDR brightness, button mapping, mods, PS2 and Xbox
extras).

Open **FrightWorks** and press **Play**. The first time, it asks for your disc image ([which one](disc.md)).

## Updates

The installed game (and the AppImage) looks for a new version when the launcher starts and asks before updating;
**Extras > About** turns that off or checks right away. Updates keep your saves and settings. The zip and the Linux
`.tar.gz` can't update themselves: their launcher tells you when a release is out, and you extract the new one over the
old folder.

## Where your files are

| | Installed | Zip (and the Linux `.tar.gz`) | Linux AppImage |
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
mapping uses it). The title screen and main menu list the keys in a corner. In play, F10 pauses the game (a cutscene
finishes first, then it pauses) and closing it carries on.

F10 is made for a controller: **LB**/**RB** (or **Q**/**E**) change the page, the D-pad or left stick picks a setting and
left/right changes it, **A** switches or opens it, **B** or **Start** goes back to the game and **Y** quits. The panel on
the right explains the selected setting, and the bottom row shows the buttons for it. A mouse and the arrow keys work too.

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

The launcher opens in borderless fullscreen. **Settings > Display > Launcher window** shows it in a window or maximized
instead; it opens that way next time. **F11** in the launcher switches fullscreen too.

**Texture packs.** **Extras > Textures** lists every pack in `UserData\texture_packs`: the ones the launcher downloads
and any pack folder you put there yourself (**Open the texture packs folder**). Higher in the list wins. Left/right
switch a pack off or on; A picks it up to move it (up/down, then A to put it down, B to put it back), or drag it by its
name with the mouse. The textures from your own Xbox disc are a pack here too ([PS2 and Xbox extras](ps2-xbox-extras.md)).
X removes a downloaded pack; the launcher never deletes a folder you added. To keep the order it
numbers the folders (`10-`, `20-`, ...). Your own `texture_replacements` folder and mods still win over every pack.

**Discord.** With the Discord desktop app running, your Discord status shows the launcher, or in the game the level and
area you're in, your Scooby Snacks, the main menu, paused or the credits, and how long you've played. Friends also see
two buttons to get the game. The launcher and the game only talk to the Discord app on your PC. Discord's **User
Settings > Activity Privacy** decides who sees it; **Settings > Game > Show in my Discord status** in the launcher
switches it off (`[discord] enabled = false` in `UserData\Config.toml`).

## Controllers and keyboard

Gamepads work right away. Remap them in the launcher (**Settings > Controls**) or in game (**F10** > Controller, or Keyboard). Button
prompts match your Xbox, PlayStation or Nintendo controller and follow remapped face buttons.

Default keyboard: WASD move, arrows camera, Space A, Left Shift B, R X, F Y, C Z, Q/E L/R, Enter Start, IJKL D-pad.

**Original Xbox layout (optional).** X head smash, B dig / talk and back in menus, LB soap bubble, RB sneak (with the
triggers, as on the Xbox); the prompts and tutorial hints follow. Black and White can also go on other buttons.

**Automatic layout.** **Settings > Controls** can match Xbox controllers to the Original Xbox layout and GameCube
adapters to GameCube. Other or mixed controllers leave the layout alone, custom bindings are kept, and choosing a layout
yourself stops the matching (the **Match my controller** switch turns it back on).

**The launcher with a controller.** Settings, Mods and Extras work like F10: the D-pad or left stick picks a row and
left/right changes it, South switches or opens it (Xbox A, PlayStation Cross, Nintendo B), East goes back, the bumpers
(or **Q**/**E**) change the page and the right stick scrolls; the bar at the bottom shows the buttons for the selected
row. On the keyboard: arrows, Enter, Esc. File and folder choosers open the launcher's own browser with an on-screen keyboard; with a mouse you get
the system's file picker.

## Linux (beta)

`FrightWorks.AppImage` runs on x86_64 Linux with glibc 2.34 or newer (Ubuntu 22.04+, Debian 12, Fedora, Arch, SteamOS).
Make it executable (`chmod +x FrightWorks.AppImage`) and run it. It needs a Vulkan driver, and FUSE to start
(`fusermount` or `fusermount3`, which desktops usually have); without FUSE run
`./FrightWorks.AppImage --appimage-extract-and-run`.

The portable `FrightWorks-<version>-Linux-x64.tar.gz` is the same game as a folder, the Linux counterpart of the
Windows zip, with the same requirements but no FUSE. Extract it anywhere you can write to
(`tar -xzf FrightWorks-<version>-Linux-x64.tar.gz`) and run `./FrightWorks` in the `FrightWorks` folder; it's already
executable. Saves, settings and screenshots stay in that folder (the zip's column in
[Where your files are](#where-your-files-are)), and it doesn't update itself: extract a new release over the folder.
RecompLauncher uses it on Linux.

Video recording and the fullscreen monitor choice aren't there yet on Linux.
[Tell us how it goes](https://github.com/FrightWorks/FrightWorks-no100f/issues) on your setup.

## Developer tools

**Extras > Game > Developer tools** turns on:

- the **F8** dev menu: go to any level, saved spots, free camera, god mode, power-ups on/off, texture finder;
- **F3** video recording (needs [ffmpeg](https://ffmpeg.org); the launcher offers to download it);
- a **DEV** mark with the build in the corner.

The build (version, and the branch and commit of a test build) is also on the F10 **About** page and at the top of
`console.log`. More for testers: [testing.md](testing.md).
