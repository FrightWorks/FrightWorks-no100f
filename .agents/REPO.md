# FrightWorks: shared notes for agents

Rules and facts every agent working on this repo needs, whichever tool it runs in.
This file is public.
People and agents also keep their own git-ignored `AGENTS.md` at the repo root for anything specific to their machine (see "Your local AGENTS.md").

## Using this file

- **Read it before changing anything.**
  The skills in `.agents/skills/` go deeper: read the matching one first.
  Claude Code looks for skills in `.claude/skills/` (git-ignored): run `python tools/setup-agents.py` once per checkout to link it to `.agents/skills/`.
  `worktree.ps1 new` and `open` do that for a worktree.
- **Change it in the PR that makes the change true.**
  A new rule or fact ships in the same diff as the work behind it.
  Standalone corrections are fine as their own small PR.
- **Rules and lasting facts only.**
  Who is working on what belongs in GitHub issues and PRs.
  Case histories belong in the skills.
  How to build and test belongs in `docs/testing.md`: if a detail fits better there, put it there and point to it from here.
- **It's public, and agents act on it.**
  No personal paths, machine details, credentials, logs or game data.
  Review edits to `.agents/` as carefully as code.
- **One sentence per line.**
  Two people editing different sentences of one paragraph then don't conflict.

## Your local AGENTS.md

Keep a root `AGENTS.md` for your own setup.
The repo's `.gitignore` already lists it and `CLAUDE.md`: never commit either.
Put a line in it pointing here, and fill in:

| Item | Value |
| --- | --- |
| You | Your GitHub name. |
| Git identity | The name and email your commits use. Check it before committing. |
| GitHub CLI | The `gh` login you use for this repo. `gh auth setup-git` makes git use it for github.com. |
| Checkout | Where your clone lives. |
| Remotes | `origin` = https://github.com/FrightWorks/FrightWorks-no100f.git. If you are a contributor, `fork` = your fork. |
| OS and GPU | Windows or Linux, and your graphics card (it matters for MSAA and test results). |
| Game | Your `Play/` is your own game and settings. Tests don't run from it (see Testing). |

## The project

GameCube port of Scooby-Doo! Night of 100 Frights: a static recompilation built on WiiCompiled + Aurora.
GPLv3 (from WiiCompiled), which can't be relicensed.
Split from Scooby-Doo: Refrightened by JosiahG at v1.0.2.
The goal is a clean remaster, not matching Dolphin: Dolphin and the disc's data show what the game intends.

| Path | What |
| --- | --- |
| `wiicompiled/translator` | C#: `main.dol` to C++ |
| `wiicompiled/runtime` | Native runtime and game overrides (C++) |
| `wiicompiled/aurora-main` | Graphics, window, input (SDL3, WebGPU through Dawn) |
| `launcher/` | Launcher (C#, Avalonia 12, Windows and Linux) |
| `tools/`, `tools/debug/` | Build scripts; debug kit, listed in `tools/debug/README.md` |
| `docs/testing.md` | How to build, test and debug |
| `Disc/`, `Play/`, `autotest/`, `tools/debug/local/` | Git-ignored: disc files, build output, work data, machine-local files |

## Rules

**Git and pull requests**
- Contributors work in their own fork: push a branch per topic there and open the PR from it.
  Maintainers with write access may push topic branches to `origin`.
  Maintainers review and merge.
- Everything reaches `main` through a PR, small docs changes too.
  Rulesets have no bypass, even for admins.
  Never push to `main`.
  Never force-push.
  `archive*` branches and tags are read only and can't be moved or deleted once created.
- After a rebase or any history rewrite, check that the branch's diff against `origin/main` contains only your own changes.
  Resetting onto a `main` that moved, while your files still match the old one, silently undoes the new commits.
- Fix-only PRs branch off `main` with minimal diffs and no heuristics that hide symptoms.
  Diagnostics never go in a fix PR (patches live in `tools/debug/patches/`).
  The debug kit changes in its own PRs.
- Open PRs as drafts.
  Say plainly what was and wasn't tested.
- PR bodies mention other PRs only if they were tested together, with conflict status.
  No tool-generated footer.
- Only @-mention someone who needs to act.
- There is no CI: never check or report CI status.
- Before editing a PR or issue body, fetch the current one and edit that: someone may have added media.
- `gh` still cannot upload attachments.
  Screenshots go through `tools/debug/pr-media.py` to FrightWorks/pr-media, which needs write access to that repo.
  People without that access use the drop-marker route: add `<!-- drop <file> here -->` markers and collect the files in one folder for a person to drag in.
  Agents must look at each picture before upload.
  No personal paths, names or addresses on screen.
  Screenshots only, never disc or game files.
  Uploaded pictures stay public.
  mp4 and other video stay drag and drop, because GitHub only plays attachments.
  GitHub won't attach `.wav` or `.mp3`: use `.mp4`.
  If the permission system refuses the push as a public upload, report the exact command and stop: don't work around it or retry another way.
- PR images must be readable at GitHub's display size: zoomed crops or a few large panels, not grids of small screenshots.
- Keep contributors as commit authors when moving their work.
- Issue labels: at most one per category (`type:`, `area:`, `priority:`, `status:`).
  No GitHub issue types.

**Never commit**
- A disc image, `Disc/`, or anything extracted or dumped from the game.
- Saves or local builds.
- The HD pack.
  A pack texture that needs fixing is edited by a script on every build (`tools/hd-pack-fixes.py`), with no pack image data in git.
  The Creeper frames in `tools/hd-pack-additions` are the only exception.
- Root `AGENTS.md` or `CLAUDE.md`, logs, recordings, or personal paths.
- One-off scripts and their output: they stay in the git-ignored `autotest/` folder.
  Only curated tools go in the debug kit, with no game data, logs, recordings or personal paths.
  Check `tools/debug/README.md` before adding a new tool.

**Docs**
- README is for players and builders only.
- Implementation notes go in code comments and PR bodies.
- Build tips go in `docs/testing.md` (keep it in step when tools change).
- Player-visible fixes go in release notes.
- A new F10 video setting with a lasting effect belongs in the launcher too.

**No bypassing security warnings:** do not bypass antivirus or OS warnings, add exclusions or restore quarantined files.

**No telemetry anywhere:** builds, tools, launcher, game, dependencies.
Check new dependencies for it.
Avalonia's `Avalonia.BuildServices` posts on every compile, so it's pinned and blocked through the root `Directory.Build.targets` (bumping Avalonia may need that pin bumped).
Set `DOTNET_CLI_TELEMETRY_OPTOUT=1` when running `dotnet` by hand.

**Credits:** in game "Made by Loneranger419 with the help of BitsofBitsofBits" (`no100f_credits.h`; later helpers are appended to `kHelpers`).
JosiahG in the README, launcher Extras > About and `tools/package-README.txt`.
ScoobyHD by dogerki in the README.

## Testing

Keep game launches short and critical.
Test code without launching the game where that can work.

| Change | Run |
| --- | --- |
| Docs | Nothing |
| Launcher | Layout tests; a person checks it only if it looks different |
| Patch mods (`launcher/ModPatches/`) | `tools/hip-patch-tests`, with its `--disc` and `--export` modes if level-file code changed (`docs/mod-patches.md`) + layout tests |
| Translator | Translator tests + smoke test |
| Game runtime or overrides | Runtime tests + smoke test; a person checks it only if behaviour changes |
| Graphics (Aurora) | Aurora tests + smoke test + a person checks it |
| Audio | Runtime tests + smoke test + a person listens |

The commands for the runtime tests (CTest) and the launcher layout tests are in `docs/testing.md`.
The Translator tests are the xUnit project `wiicompiled/translator/tests/Translator.Tests`.
Aurora's tests are its standalone build: `gx_fifo_tests`, and `gx_readback_tests` (GPU pixels) with `-DAURORA_BUILD_GPU_TESTS=ON`; both register with CTest.
The Windows command is in `docs/testing.md`.
Anything beyond the table needs a stated reason.

**Not built yet:** the smoke test script, shared run slots (game runs take turns for now), the shared wait-until-playable routine (`texhunt\look.ps1` has its own), controller isolation for test runs and the full sweep before a release.
Until they exist, a smoke test means booting `h001` and the changed level with `manual-session` from a fresh build, checking the build ID, the level load and that the game stays up.
Nothing runs these automatically, so the PR says what was run.

**Habits**
- A build passing, tests passing and a real game run are different evidence.
  Say which one you have, and do not claim more.
- A test program that is missing, not built, not registered with CTest, or that could not load is not a pass.
  Report it as not run.
- Show a bug on unmodified `main` first, then show it gone with your change.
- For renderer changes also run the Aurora tests, and run the game once with `AURORA_GFX_FLAGS=validate`: it keeps Dawn's validation and logs its errors instead of aborting on them.
- Renderer changes must preserve colour space, transparency, depth and capture behaviour.
- Wait on log lines or files, not fixed sleeps, where you can.
- Close only the test game.
  On Linux use `pkill -x "FrightWorks Gam"`: the process name is cut to 15 characters, and `pkill -f` with the game's name also kills the calling shell.

**Test runs**
- Never test from your own `Play/`, which holds your game and settings.
  Use a worktree (it has its own `Play/`) or a separate test folder.
  A test folder has links or copies of `Play/`'s assets, its own `UserData/Config.toml` (windowed, MSAA off, muted) and a boot mod.
  Copy the freshly built game into it.
- Give automated game runs a time limit.
  Afterwards, check that only the right process closed.
- Leave the user's setup as found: `manual-session` `prepare`/`start` save `Config.toml` and the `*.controller` files, `stop`/`restore` put them back.
- Rumble off (`[controller] rumble = false`), always.
  The game's default is on, so check before any launch.
- Sound muted (`[audio] muted = true`), unless a person is testing or playing it or the task is audio: then on at 5% (`[audio] volume = 0.05`).
  Muting only affects output: `AUDIO_RAW_DUMP` captures are unaffected.
- Don't use `level-sweep.py` or several windows for visual bugs: one window, looked at by a person.
- No on-screen sweeps through launcher or menu options.
  Compare options in code or headless, and sample one or two of each kind on screen.
- The game reads the controller without window focus (#123), so a test game can act on a pad someone is using.
  Check that nobody's game is running before a run on a shared desktop.

## Building

```sh
# Windows
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1       # full build, stages Play\
powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
# Linux
tools/build-no100f-linux.sh [path/to/game.iso]                          # full: CMake + .NET 10, translate, configure, compile, stage Play/
tools/build-launcher.sh                                                 # launcher/publish/FrightWorks
# Linux, after native-only changes
toolchain/CMake/bin/cmake --build wiicompiled/native-build-linux --target WiiCompiled --parallel "$(nproc)"
```

- The Linux incremental build (the last command) does not copy into `Play/`: copy the built `WiiCompiled` there as `FrightWorks Game`.
- F3 video recording and fullscreen monitor choice aren't on Linux yet.
- The rest of the build facts are under "Facts worth knowing".

## Driving the game

The dev menu (see Facts) is the way to script a run.
- **Command file:** `devmenu.txt` in the game's working folder (`Play/` for a normal run).
  It is read every 30 frames, then deleted, and results go to `devmenu.log`.
  It waits until the game is in play, except `skip` and `scene`.
  Useful commands: `scene ID`, `cam X Y Z YAW PITCH`, `camspin DEG`, `cammove DX DY DZ`, `freecam on|off`, `where`, `perf`, `save NAME`, `cutscene`, `skip`.
  The full list is at the top of `PollCommandFile` in `wiicompiled/runtime/src/no100f_devmenu.cpp` and in the `game-logic-debug` skill.
- **Presses:** `devmenu_press.txt`, read every 30 frames even outside play.
  Each line is `BUTTON[+BUTTON] [FRAMES] [cutscene]`, with buttons `a b x y z start up down left right` and 10 frames by default.
  `cutscene` means only while a recorded cutscene plays.
  `NSMBW_INPUT_SCRIPT` scripts pad input from the environment instead.
- **Boot mods:** a folder `UserData/mods/<name>/files/sd2.ini` with `BOOT=e001` starts a level directly.
  A mod folder holds `files` mirroring the disc and/or `textures`, and/or a `patch.json` (see "Patch mods" under Facts); when two mods change the same file, the later name wins.
  A file named `disabled` in the mod folder turns it off.
- **Logs:** `RT_LOGF` and Aurora's log go to stderr, so redirect it (`2> file`).
  The current level is in the newest `UserData/Logs/*/console.log` (`[dvdread] ...xNNN.HIP` lines).
- **Audio capture:** `AUDIO_RAW_DUMP=<file>` (built in); details in the `audio-debug` skill.
- **Port 1:** it counts as connected only with a controller or keyboard bindings (the keyboard has default bindings; changed ones are saved in `UserData/keyboard_bindings.dat`).
  With neither, the game sees an unplugged pad and pauses with its "controller removed" message.

## Skills and parallel work

Skills are in `.agents/skills/`.
Read the matching one first.

- **`hle-divergence-debug`:** geometry, state and GPU bugs (data integrity, `dol.py`, override vs original, draw probes, the Dolphin oracle).
- **`game-logic-debug`:** fps60, timing and gameplay (spawn spots, dev menu, live memory, particles, flicker).
- **`manual-yn-bisect`:** locked landing plus in-game Y/N for visual checks; `strip-before-pr.md` for fix-only PRs.
- **`audio-debug`:** crackle, clicks, fuzz, stuck loops (capture, per-stage dumps, disc reference decode).
- **`parallel-work`:** one git worktree per task, optionally each with its own Claude Code session coordinated from the main checkout.

Worktrees without sessions: `python3 tools/debug/worktree.py new --noopen --name NAME --branch BRANCH`, then build there; `list`; `remove --name NAME` when the branch is merged or dropped.
The tools are `tools/debug/worktree.ps1` (Windows) and `worktree.py`.
In a worktree, `Disc`, `texture_pack`, `toolchain`, `game` and `wiicompiled/Dependencies` are links to the main checkout: read only.
Never delete a worktree folder by hand; `remove` unlinks first.
Don't switch a checkout that has the `.claude/skills` link to a commit from before `.agents/` existed: git fails partway, after deleting tracked files.
To build old source there, set only the paths that differ with `git restore --source=<commit> --worktree --pathspec-from-file=<list>`.
A worktree's first build must be full.
Never name a session `main`.

## Facts worth knowing

**Build**
- Adding or removing overrides, switching between branches that differ in them, or building after a patch build needs a full translate (no `-SkipTranslate`), or the game stops at boot with "Fatal Error", PC=0.
- `build-no100f.ps1` builds the translator only if `bin\Release\net10.0\Translator.Cli.dll` is missing.
  Delete `bin\Release` after translator edits, or the output comes from a stale translator.
- An open launcher locks `Play\SDL3.dll`: the build fails at the DLL copy after the exe copy, which is harmless.
- Kit scripts must parse in PowerShell 5.1.
  `powershell -File x.ps1 -List a,b` passes one string, so split it in the script.
  A bad `BOOT=` crashes the game.
  New cross-platform scripts are Python 3.8+.
- Build ID (`runtime/cmake/build_info.cmake`): `<Version> branch@commit[+hash of uncommitted diff]`, shown in `console.log`, F10 About, the DEV mark and the launcher footer.

**Dev tools and input**
- `[game] dev_tools` or `NO100F_DEV_TOOLS=1`: F8 dev menu (`no100f_devmenu.cpp`), F3 recording, DEV mark.
- SendInput keys don't reach the game.
  Keyboard pad input needs window focus.
- Xbox layout swaps B and X in gameplay only (menus are modes 2, 4, 6, 7 and scene H003).
  If a button acts oddly, check `[controller]` in `Config.toml` first.
- F10 reads the physical pad, not `[controller]`, so a bad remap can't lock anyone out.
- Game state for UI: `no100f_game_state.h` (mode 2 state 0 = title/main menu; mode 2 state 1 = attract demo, which plays Bink movies).
- Pad update override `0x8000F5E8`: `hle/input/no100f_pad_update.cpp` (mapping in `hle/no100f_pad_actions.h`).
- Native overrides needing guest interrupts off: `OS__DisableInterrupts_801a65ac` / `OS__RestoreInterrupts_801a65d4`.

**Levels and saves**
- Scene IDs and which scenes suit which tests: `tools/debug/game/levels.md`; `python tools/debug/game/levels.py i004` gives level, part and area.
- Most used: `i001`, `i004`, `e001`, `h001`, `h002`, `f001` (Shock on the Dock!).
  Boot `i004` directly (`i003` is snack-gated).
  `h001` can stick on its intro.
  `e001` is part 1 of On Edge in the Hedge! (Hedge Maze); its blowing leaves (5.7 units/s) are a good speed check.
- The save object is the spinning Mystery Machine (pickup `SAVEPOINT`).
  In `g001` it's at -25.618 1.186 -1.515.
  Loading a save puts Scooby at -27 0.121 0 in `g001`.

**Rendering**
- The EFB isn't cleared at frame start (#78).
  The game's display copies never clear, because the game clears with its own drawn quad.
  The copy-clear colour stays at GXInit's white, so a draw-less frame shows white (#78).
  Suspect a clear for any "white frame" bug.
- Some meshes are skinned on the CPU (the manor curtains): the game rewrites their vertex arrays into a buffer pool every frame, so addresses say nothing about identity.
- Frame interpolation (#92) pairs draws across frames by an exact identity (it hashes the POS array contents, not addresses), then a stable identity, then nearest distance (`frame_interpolation.cpp`, `command_processor.cpp`).
  A pair moving more than 0.5 units in one frame with the camera still is a wrong pair.
  F3 recordings don't show interpolated frames.
- Cut-outs: `[video] cutout_mode`, default two passes.
  Fringes are deferred and flushed before non-depth-writing and z-ALWAYS draws (#46).
- MSAA: on Linux (x86_64 and aarch64) the Vulkan 2x and 8x modes need the patched Dawn package that the build downloads from FrightWorks/dawn-build (release `v20260603.191052-msaa`).
  Other platforms, Windows included, use the stock package.
  Pipelines cached for another MSAA setting are skipped, not failed.
- A launcher Display setting mirrors F10 Video (#91): a new F10 video setting with a lasting effect belongs in both.
- Monitor labels are duplicated in `aurora-main/lib/monitor_labels.hpp` and `launcher/Monitors.cs`: change both (#95).
- Aurora's standalone tests aren't built by the game build and nothing runs them, so they fall behind the code: build and run them when changing the renderer.

**Audio**
- GC audio runs on the game thread.
- Volume sliders work through a native override of MIXAdjustFader (#86, `music_attenuation.cpp`).
  Known limits: resident dialog counts as SFX, Bink audio is master only.

**Paths**
- Portable (`data\portable.txt`): everything in `data\UserData`, disc in `data\Disc\DATA`.
- Installed: settings, logs, cache, disc and mods in `%LOCALAPPDATA%\FrightWorks-Data`, saves in `Saved Games\FrightWorks`.
  The installer's program folder is deleted on uninstall.
- `[paths] saves_dir` overrides the saves folder.

**Patch mods** (format and error handling: `docs/mod-patches.md`)
- A mod's `patch.json` adds models and objects to a level with no game data in the mod: the launcher (`launcher/ModPatches/`) builds the level from the player's own disc into `UserData/mods/!patched`, and the game reads that like any other mod.
- The game has no code for this, and the build runs on players' PCs, so it's C#, not Python.
- `!patched` holds the player's disc data: never commit, share or ship it, and never write to `Disc` or the saves when building it.
- `HipFile.cs` must match `tools/debug/game/hipedit.py` byte for byte: a change to one needs the other and a run of `tools/hip-patch-tests/reference.py`.
- Budgets for added bytes are in `PatchBuilder.cs`: h001 and h002 1 MiB, h003 and every other level 256 KiB.
  Each hub's budget is no more than a size that ran there, and the other levels haven't been measured, so raise one only after measuring it.
- A material over 65,535 vertices crashes the game, whatever memory is free: the draw count is 16 bits, so it wraps and the remaining vertices run as commands.
  The launcher reads the counts from the model's own bytes and refuses it, and it refuses a model over 13000 triangles.
- Models take about 0.9 of their file size from the OS heap, not the main heap.
- Added objects aren't lit at run time and aren't in saves.

## Known non-bugs and parked work

- Not bugs (checked): the single fog puff in `i004` after the fan; the lighthouse beam coming and going (same as Dolphin).
- Parked, only when a maintainer asks: glow discs on level BSP (`e005` greenhouse); Shaggy statues flashing with Scooby on damage.

## Releases

Versions continue from upstream v1.0.2.
A maintainer runs releases:

1. Version PR: `<Version>` in `launcher/FrightWorks.Launcher.csproj` and the version line in `tools/package-README.txt`.
2. After the merge, on a clean `main`: local tag `v<ver>`, then `tools/package-scooby.ps1 -Build`, then `-Zip` as a second call (`-Build` resets PATH), then `-Installer`.
3. Smoke test: the zip has no game data, saves or settings; launcher `--shots` from the unzip; boot `h001` with HD and `i004`.
4. `gh release create v<ver> --draft --target <full SHA>` (a short SHA gets HTTP 422) with `FrightWorks-<ver>.zip`, every `dist\installer\*` file and the notes.
   A maintainer publishes.

Installed launchers only find updates in published releases carrying `releases.win.json`.
A local test feed is described in `docs/testing.md`.
