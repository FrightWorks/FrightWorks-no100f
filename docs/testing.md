# Testing and dev tools

How to build FrightWorks yourself, run the tests, and use the debug tools to check a change or chase a bug. Everything
here is Windows 10/11 x64 and PowerShell, run from the repository root.

The tool reference (every script, one line each) is [`tools/debug/README.md`](../tools/debug/README.md); this page is
the walkthrough.

**Never commit game data:** no disc image, `Disc/`, extracted or dumped files, logs or recordings. `Disc/`, `Play/`,
`autotest/` and `tools/debug/local/` are git-ignored for that reason.

## 1. One-time setup

You need Python 3 and Git, plus your own **Night of 100 Frights, GameCube USA v1.01 (GIHE78, revision 1)** image. The
build downloads its own compiler, CMake and Ninja into `toolchain\` the first time.

```powershell
# Build the launcher and use it to extract your disc image into Disc\DATA
powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
& "launcher\publish\FrightWorks.exe" --extract "C:\path\to\game.iso" Disc\DATA

# First full build of the game (translates main.dol, then compiles) -> Play\
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1
```

The debug scripts also use Python packages: `pip install numpy pillow capstone` (`capstone` is for the disassembly
tools). Video capture needs ffmpeg on PATH, or run `tools\debug\capture\get-ffmpeg.ps1` once.

## 2. Build and play

For widescreen edge-culling regressions, the focused `no100f_model_culling_tests` CMake/CTest target checks sphere
classification and the linked-model visibility gate. The GIHE78 rev 1 repair applies only to the main camera's
widescreen entity pre-cull: a character's root atomic can be a small hat rather than a bound for the whole character.
The entity stays eligible while a visible linked model intersects the frustum; individual atomic culling, shadow
cameras and Original 4:3 retain their existing behavior. For a visual check, use an isolated `f001` dock session and
move the developer camera across both horizontal edges, checking that the groundskeeper's body does not disappear
when his hat leaves view, then repeat in Original 4:3. Do not change or load an existing save for this check.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1 -SkipTools                  # full build
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1 -SkipTools -SkipTranslate   # faster: native code only
& "Play\FrightWorks Game.exe"
```

- Every build also puts the HD texture pack (`texture_pack\ScoobyHD`, downloaded from the Internet Archive by the first
  build, plus `tools\hd-pack-additions`) and the button prompt icons into `Play\texture_replacements\ScoobyHD` and
  `Play\button_icons`, beside the game like a release (`tools\stage-assets.ps1`; only what changed is copied or
  rebuilt; old copies in `Play\UserData` are removed). The game indexes `UserData\texture_replacements` and mods
  first, so those win over the built-in pack. The icons need Python and ImageMagick; without them the build warns and
  goes on.
- `-SkipTools` skips the toolchain and dependency checks (fine once the first build has downloaded them).
- `-SkipTranslate` reuses the translated game code. Only use it when you changed native code (`wiicompiled/runtime`,
  `wiicompiled/aurora-main`). **Do a full build** (no `-SkipTranslate`) when you add or remove a native override
  (`PPC_NATIVE_OVERRIDE`), switch to a branch that does, or after any build with a debug patch that adds overrides
  (the dev menu does). Otherwise the game stops at boot with "Fatal Error" and PC=0.
- Settings, saves and mods are in `Play\UserData\` (`Config.toml`; in game **F10**, **F11** fullscreen):
  `Play\portable.txt` makes the dev tree portable, like the zip. Without that marker the game uses the installed
  layout: `%LOCALAPPDATA%\FrightWorks-Data` (settings, logs, cache, disc, mods), `Saved Games\FrightWorks`,
  `Pictures\FrightWorks` / `Videos\FrightWorks` for F2 / F3. `[paths] saves_dir` in Config.toml moves the saves in
  either layout (the launcher's **Extras > Folders** sets it). To test the installed layout without an installer,
  copy `Play\`'s exe, DLLs, `dsp_coef.bin`, `button_icons` and `texture_replacements` into a folder named `data`
  (no `portable.txt`), with `FrightWorks.exe` beside it.
- Packaging a release zip: `tools\package-scooby.ps1 -Build -Zip` → `dist\`. It also needs ImageMagick (`magick`) on
  PATH and the HD texture pack in `texture_pack\ScoobyHD` (not in git).
- **Installer** (Velopack): `tools\package-scooby.ps1 -Installer` → `dist\installer\`: `FrightWorks-win-Setup.exe`,
  `FrightWorks-<ver>-full.nupkg`, a `-delta.nupkg` against the previous release, and `releases.win.json` /
  `assets.win.json`. It first runs `vpk download github` so the delta can be made against the latest GitHub release.
  vpk (pinned in the script and in the launcher's Velopack `PackageReference`: change both together) is installed into
  `dist\.vpk`. The installer puts the game in `%LOCALAPPDATA%\FrightWorks` (`current\` = the program, replaced on
  every update; the whole folder goes on uninstall) with Start menu and desktop shortcuts; the game's own data goes to
  the installed layout above.
- **Testing an update without GitHub** (a local feed): pack two versions into one folder, install the first, point the
  launcher at the folder.
  ```powershell
  tools\package-scooby.ps1 -Installer -Version 1.0.5-test.1 -Feed dist\testfeed -NoDownload
  dist\testfeed\FrightWorks-win-Setup.exe --silent       # installs only (--silent doesn't start the launcher)
  tools\package-scooby.ps1 -Installer -Version 1.0.5-test.2 -Feed dist\testfeed -NoDownload   # makes the delta too
  $env:FRIGHTWORKS_UPDATE_SOURCE = "$PWD\dist\testfeed"
  & "$env:LOCALAPPDATA\FrightWorks\current\FrightWorks.exe"   # asks to update to test.2
  ```
  Uninstall afterwards in Windows Settings > Apps (or `%LOCALAPPDATA%\FrightWorks\Update.exe --uninstall`); the data
  folders stay (delete `%LOCALAPPDATA%\FrightWorks-Data` and `Saved Games\FrightWorks` by hand for a clean slate).
- **Release with the installer**: after the zip steps, `-Installer` on the same clean `main` (version from the
  `.csproj`), then put `dist\installer\*` (Setup.exe, the nupkgs, `releases.win.json`, `assets.win.json`) on the same
  draft release as the zip: `gh release upload v<ver> dist\installer\FrightWorks-win-Setup.exe ...`. Installed games
  look for `releases.win.json` in the published (non-pre-release) releases, so every release from now on carries
  these files; a release without them isn't offered as an update. The zip's launcher reads the latest release's tag.

## 3. Unit tests

The tests are CMake targets in `wiicompiled/runtime/CMakeLists.txt`; the game build doesn't build them. After a game
build has created `wiicompiled\native-build`:

```powershell
$tc = "$PWD\toolchain"; $env:PATH = "$tc\llvm-mingw\bin;$tc\Ninja;$tc\CMake\bin;$env:PATH"
cmake --build wiicompiled\native-build --parallel          # builds everything, tests included
ctest --test-dir wiicompiled\native-build --output-on-failure
```

One test: `cmake --build wiicompiled\native-build --target <name>`, then run `wiicompiled\native-build\<name>.exe`.
New pure logic (no SDL, no guest memory) belongs in a header with a test next to the existing ones.

## 4. Playtest a level

`tools\debug\manual-session.ps1` opens one game window straight into a level, for looking at a bug and taking
screenshots.

Every test runs with the same settings (windowed 1600x900, 60 fps, MSAA 4, HD pack off unless `-HdTextures`, sound
muted unless `-Sound`, rumble always off), not yours. `start` saves your own settings first (`Play\UserData\Config.toml` and the
`*.controller` files where F10 controller remaps are kept) to `tools\debug\local\user-setup\`, and `stop` puts them back and deletes the
`Play\UserData\mods\zz-manual` boot mod. If a test didn't finish, the saved copy stays (a new test keeps it rather than
saving the test settings over it); `restore` puts it back.

```powershell
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 start -Boot i001 -AllPowerups
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 shot -Label curtains   # -> autotest\manual\
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 stop                   # your settings back
```

- `-Boot <scene>` picks the level (default `e001`); `-AllPowerups` unlocks every gadget; `-HdTextures` turns the HD pack
  on; `-Sound` turns the sound on at 5% (`[audio] volume = 0.05`; without it `muted = true`). Use it when you want to
  hear the game or are working on audio. Mute and volume only change what SDL plays: `AUDIO_RAW_DUMP` captures are the
  same either way.
- Controller rumble is off in every test run (`[controller] rumble = false`), with no switch to turn it on. The game's
  default is on, so a script that starts the game has to set it: `manual-session` writes it, `level-sweep.py` writes it
  in its workers' configs, and `play-patched`, `iterate` and `capture-window` set it for the run and put your
  `Config.toml` back afterwards. The helpers are in `tools\debug\kit-common.ps1`.
  Scene IDs, level names and which scenes suit which tests: [`tools/debug/game/levels.md`](../tools/debug/game/levels.md),
  or `python tools\debug\game\levels.py i004`. Common ones: `h001` (hub, smoke test), `i001`, `i004` (boot it directly),
  `e001`.
- `status` shows the running session; `stop-tests` kills leftover automation.
- Settings not in the generated config, or launching the game yourself (e.g. to capture its stderr): run `prepare`
  (same options as `start`; saves your settings, writes the config and boot mod, doesn't start the game), add your
  section to `Config.toml`, launch the exe, and `stop` when done (`shot` captures any running game). Example:
  `tools\debug\holiday-shots.ps1`. The other scripts in `tools\debug` that start the game do the same.
- Don't run several game windows or level-sweep automation for visual bugs: one window, looked at by a person.

## 5. Dev menu (F8)

The dev menu is part of the game, off unless **Extras > Developer tools** is on in the launcher (`[game] dev_tools =
true` in `Config.toml`) or `NO100F_DEV_TOOLS=1` is set; the scripts in `tools/debug` set that variable for their
sessions. With it on, the game shows **DEV** and its build in the bottom-right corner. `play-patched.ps1` builds,
starts the game with the dev menu on, the sound at 5% and rumble off (your volume, mute and rumble lines go back when it
closes) and, for
branches from before the dev menu was built in, applies
`tools/debug/patches/devmenu.patch` and takes it out again when you close the game:

```powershell
powershell -ExecutionPolicy Bypass -File tools\debug\play-patched.ps1              # dev menu
powershell -ExecutionPolicy Bypass -File tools\debug\play-patched.ps1 -HdTextures  # with the HD pack, like the release
```

In game, **F8**: current level and Scooby's position, go to any level (Levels tab), save spots and go back to them, god mode,
monsters off (all or one by one), power-ups on/off (all at once or one by one), a Textures tab (find a texture, mark it), an Effects tab and a free camera. Spots are
saved in `Play\UserData\devmenu_spots.txt`. Travel is one transfer at a time: asked for while paused or in a
cutscene, it waits for normal play (**Cancel travel** drops it). **Skip** presses the game's own skip button for a
recorded cutscene (A) or the title intro / attract demo (Start). More commands (scripted through `Play\devmenu.txt`) are in the
`game-logic-debug` skill.

**F3** (developer tools on) starts / stops a video of the game: `Videos\FrightWorks_<date>_<time>.mp4` beside the
launcher (`Play\Videos` in a source build) and a `.txt` with the build, picture size, Scooby's position at the start and
the end, and the `[video]` / `[game]` settings. The frames come from the renderer at the window's size (at most 1080
lines, 60 fps), so other windows on top don't get in; a red REC light shows in the top-right corner while it records
(never in the video). F3 again, closing the game or 30 minutes stops it. Two toggles at the top of the F8 dev menu (saved in
`[game]`): **Record audio** (`record_audio`, on by default) adds the game's own sound, taken before the volume and
mute settings; **Menus in screenshots and videos** (`capture_overlay`, off by default) puts the F8/F10 menus and
notices in F3 videos and F2 screenshots too; off, only the DEV mark is in them. Resizing the window (or F11) during a recording
freezes the video until the window is back to its old size. It needs ffmpeg: on PATH, next to the launcher (`ffmpeg\bin`) or the kit's
`tools\debug\local\ffmpeg`; turning the developer tools on in the launcher offers to download it there (the same
SHA-256-checked build as `tools\debug\capture\get-ffmpeg.ps1`).

The Textures and Effects tabs list textures from `Play\UserData\devmenu_textures.txt`, one per line, tab-separated:
`id  file  severity  asset  scenes  level  [effect]` (`scenes` space-separated, `boot` = every scene; `#` starts a
comment). Lines whose seventh column starts with `effect` go on the Effects tab: textures drawn at runtime (bonks,
pickups, particles), not a switch for gameplay effects. Edit the file while the game runs and press **Reload**. The
free camera turns itself off when the scene changes.

Options: `-Patch devmenu,postool` (several patches), `-CleanBuild` (rebuild `Play\` without the patch afterwards),
`-Keep` (leave it applied to work on it; re-export the patch afterwards), `-Remove` (take the patches out if the
window was closed instead of the game), `-SkipTranslate` (only if the last build was from the same branch with the
same patches), `-NoBuild` (play what's in `Play\`).

A dev-menu build of another branch: `tools\debug\texhunt\build-devmenu.ps1 -Branch <branch>`, then
`play-patched.ps1 -NoBuild` (branches with the built-in dev menu are built as they are). The patch is only for older
branches now; change the dev menu in `wiicompiled/runtime/src/no100f_devmenu.cpp`.

**On an older branch, after a dev-menu patch build the next normal build must be a full build** (no `-SkipTranslate`),
or use `-CleanBuild`.

Every build knows what it was built from: the F10 **About** page, the DEV mark and the first lines of `console.log`
show the version and, for a test build, `branch@commit` plus `+xxxx` when it had uncommitted changes (a hash of
them, so two builds with different edits differ). The launcher shows the same ID bottom left. It gets it at build time
from the portable CMake in `toolchain\`, so a launcher built before the first game build says "unknown build":
rebuild it once the toolchain is there.

## 6. Other diagnostic patches

`tools/debug/patches/` holds diagnostics that never go into a fix PR. Apply them with `play-patched.ps1 -Patch <name>`
(or `git apply`, and `git apply -R` before committing). The table with what each one does is in
[`.claude/skills/manual-yn-bisect/strip-before-pr.md`](../.claude/skills/manual-yn-bisect/strip-before-pr.md). The most
useful:

| Patch | What it adds |
| --- | --- |
| `devmenu` | F8 dev menu for branches from before it was built in (above) |
| `postool` | Live memory: write `dump NAME`, `peek ADDR [N]` or `poke ADDR HEX...` lines to `Play\postool.txt` (run every 20 frames, output in `Play\postool\`); **F9** saves Scooby's position to `Play\postool\spots.txt` |
| `audio-diagnostic` | Per-stage audio dumps (`AX_DIAG_DIR`) |
| `real-date` | Real clock for old builds that always start on 2000-01-01 |

## 7. Logs, scripted input and other switches

- **Logs**: `Play\UserData\Logs\<time>\console.log`; the newest one shows which level files load (`[dvdread] ...
  xNNN.HIP`). Runtime messages (`RT_LOGF`) go to stderr: launch with
  `Start-Process "Play\FrightWorks Game.exe" -WorkingDirectory Play -RedirectStandardError err.txt`.
- **Scripted input** (no controller needed): set `NSMBW_INPUT_SCRIPT` before starting the game, e.g.
  `$env:NSMBW_INPUT_SCRIPT = '10000:PLUS,12000:DOWN,13500:A'` = press Start at 10 s, D-pad down at 12 s, A at 13.5 s
  (ms after the game first reads input; optional third field = hold time in ms, default 150). Buttons: `A`, `B`,
  `PLUS` (Start), `ONE` (X), `TWO` (Y), `MINUS` (Z), `UP`/`DOWN`/`LEFT`/`RIGHT`.
- **Button prompts**: `$env:BUTTON_ICONS = 'xbox'` (or `playstation`, `nintendo`) forces a prompt set; the game puts
  the prompt picture together from one icon per pad button, following player 1's mapping. The icons are built by
  `python tools\make-button-icons.py --out Play\button_icons` (needs ImageMagick and the HD pack).
- **Audio capture**: `$env:AUDIO_RAW_DUMP = 'C:\temp\out.raw'` writes the mixed output; analysis tools in
  `tools\debug\audio\`.
- Remove test variables afterwards: `Remove-Item Env:NO100F_*, Env:AX_*, Env:NSMBW_*, Env:AUDIO_*, Env:BUTTON_ICONS -ErrorAction SilentlyContinue`.
- **Launcher screenshots** without opening it: `& "launcher\publish\FrightWorks.exe" --shots <dir>`
  draws every page to `<dir>\<Page>.png`.
- **Isolated launcher layout regression** (Windows, .NET 10 SDK or newer):
  `dotnet run --project tools\launcher-layout-tests\LauncherLayoutTests.csproj -c Release -- autotest\launcher-layout`.
  This builds the launcher and checks Settings/Display, Controls, Mods and Extras at normal, minimum and smaller
  window sizes, reopening and growing/shrinking layouts, long mod names, nested scrolling, dropdown widths, keyboard
  button activation, transparent button corners (normal, hover, pressed and disabled, including the gradient-backed
  close button), and preservation of custom HDR and unknown config values. It creates a disposable portable
  fixture and page screenshots only in the output directory; it never starts the game or uses the player's data.
  It also moves its own test window across existing monitors and back, logging actual DPI transitions without
  changing display settings. An optional trailing zero-based monitor index selects the starting monitor.
  Additional font/size stress at 125%, 150%, 200% and back to 100% is **simulated**, separate from those native
  transitions. Also check first-open painting interactively. The rounded board region should only change on resize, not on ordinary layout,
  and page content should scroll vertically without hiding dropdown arrows or producing horizontal scrollbars.
- **Screen recording**: `tools\debug\capture\record.ps1`.

## 8. Looking at the game's code and data

- `python tools\debug\dol.py func 8000F5E8` disassembles a function of `main.dol`; `callers <addr>`, `dis <addr> <n>`,
  `name <addr>` (needs `capstone`).
- `python tools\debug\overrides.py` lists the native overrides (functions the port replaces).
- `tools\debug\game\addrrefs.py LO HI` finds code that builds an address in a range.
- `tools\debug\audio\hip.py` lists the assets in a level file (`Disc\DATA\files\*.HIP`).
- Dolphin is useful as a reference for what the game intends; compare against it rather than copying it.

## 9. Before you open a PR

- Fix-only: minimal diff, no diagnostics (revert any patch: `play-patched.ps1 -Remove -Patch <name>` or `git apply -R`),
  no unrelated changes, no heuristics that hide a symptom.
- Run the unit tests, and a full build if you touched overrides.
- Smoke test: boot `h001` and one level you changed something in.
- Open it as a draft and say plainly what you tested and what you didn't.

## 10. Working on several branches at once

Each task can get its own git worktree: a second folder with its own branch, build and `Play\`, sharing the big
read-only folders (disc files, HD pack, toolchain) with the main checkout through links. Each worktree gets its own
Claude Code session in a Windows Terminal tab; one session in the main checkout, named `coordinator`, coordinates them
(the `parallel-work` skill). Not `main`: in SendMessage, "main" means the sender's own conversation.

```powershell
powershell -ExecutionPolicy Bypass -File tools\debug\worktree.ps1 open -Name main
powershell -ExecutionPolicy Bypass -File tools\debug\worktree.ps1 new -Name installer -Branch feat/installer -Task "Implement #55"
powershell -ExecutionPolicy Bypass -File tools\debug\worktree.ps1 list
powershell -ExecutionPolicy Bypass -File tools\debug\worktree.ps1 open -Name installer
powershell -ExecutionPolicy Bypass -File tools\debug\worktree.ps1 remove -Name installer
```

- `open -Name main` opens a tab in the main checkout running the coordinating session (`coordinator`). Ask it to
  start, check on or finish tasks; it runs the commands below and can message the other sessions. Workers send it
  their questions and approval requests (PRs, options, visual checks), and it answers or asks you (`AGENTS.md`:
  "Questions go through the coordinator"). Commits and pushes to a task's own branch need no approval.
- `new` creates `..\FrightWorks-worktrees\<name>` from `origin/main` (or checks out an existing branch) and opens a
  tab there running `claude -n <name>` (`-NoOpen` to skip). With `-Task`, the goal goes into the worktree's `TASK.md`
  and the session starts on it. Its first build must be a full one: `toolsuild-no100f.ps1` without
  `-SkipTools`/`-SkipTranslate` (or `new ... -Build`).
- `open -Name <name>` starts a closed session again: a new conversation that picks the task up from `TASK.md`
  (`-Continue` to resume the last conversation instead). `new`/`open` start the tab without the Claude Code
  variables of the shell that runs them, so a session started from the coordinator is its own session: listed by
  ListAgents, its conversation saved for `-Continue`.
- `TASK.md` in the worktree holds that task's notes; the agent notes (`AGENTS.md`, local, not in git) are shared.
- To try a worktree's build, run its `Play\FrightWorks Game.exe`, or build its launcher there
  (`toolsuild-launcher.ps1`) and run its `launcher\publish\FrightWorks.exe`: it starts that worktree's game, with its
  own settings.
- Builds can run side by side. Game runs take turns: `manual-session prepare/start` and `play-patched.ps1` wait while
  another checkout's test has the game open. Scripts only close the game they started from their own checkout; your
  own game (launcher or `Play\` in the main checkout) is never closed by a test.
- Never delete a worktree folder by hand: `Disc`, `texture_pack`, `toolchain` there are links, and some delete tools
  follow links into the main checkout. `remove` unlinks them first; it refuses while the worktree has uncommitted work
  or a program running from it (leftover dotnet build servers it stops itself), and while its Claude session runs:
  close that tab first.

## Claude Code skills

`.claude/skills/` has step-by-step guides that Claude Code loads, readable by anyone: `hle-divergence-debug`
(graphics and state bugs), `game-logic-debug` (60 fps, timing and gameplay bugs, dev menu, postool),
`manual-yn-bisect` (yes/no visual bisects, the fix-only PR workflow) `audio-debug` (crackle, clicks, fuzz) and `parallel-work`
(worktrees and their sessions, run from the coordinator).
