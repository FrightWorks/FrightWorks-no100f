# Testing and dev tools

How to build FrightWorks yourself, run the tests, and use the debug tools to check a change or chase a bug. Everything
here is Windows 10/11 x64 and PowerShell, run from the repository root.

The tool reference (every script, one line each) is [`tools/debug/README.md`](../tools/debug/README.md); this page is
the walkthrough.

**Never commit game data:** no disc image, `Disc/`, extracted or dumped files, logs or recordings. `Disc/`, `Play/`,
`autotest/` and `tools/debug/local/` are git-ignored for that reason.

## 1. One-time setup

You need Python 3.8 or newer and Git, plus your own **Night of 100 Frights, GameCube USA v1.01 (GIHE78, revision 1)** image. The
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
  goes on. The staged copy of the pack is then fixed by `tools\hd-pack-fixes.py` (Python, standard library only): it edits
  dogerki's file for the Smuggler's Cove sign (#107: no alpha channel, so the small ship's sails on its roof drew as opaque planks) by SHA-256,
  taking the alpha from the disc's own texture in `Disc\DATA` (nothing from the pack or the game is in git; `texture_pack`
  stays as downloaded). A skipped fix is a log line, or a failure when packaging (`-Required`).
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
  PATH, the HD texture pack in `texture_pack\ScoobyHD` (not in git) and the extracted disc in `Disc\DATA`: the pack fix
  (`tools\hd-pack-fixes.py`, run with `--required`) stops the packaging if it can't be applied.
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
- **Dawn on Linux** (2x and 8x MSAA): WebGPU allows only 1 and 4 samples, and stock Dawn aborts on 2 and 8 on Vulkan.
  Linux (x86_64 and aarch64) therefore downloads the Dawn package FrightWorks builds and hosts: release
  `v20260603.191052-msaa` on `FrightWorks/dawn-build`, URL and SHA-256 in
  `wiicompiled/aurora-main/cmake/AuroraDawnProvider.cmake`. It is the same Dawn commit as the stock package plus a
  patch (`FrightWorks/dawn`, branch `frightworks/v20260603.191052`, commit `dd7b19d`) that maps 2 and 8 to themselves
  when the GPU's Vulkan limits support them and to 4 otherwise. The workflow that builds it is on `FrightWorks/dawn-build`,
  branch `frightworks/build`. Other platforms, Windows included, use the stock package. When `AURORA_DAWN_VERSION`
  changes: rebase the patch onto the new Dawn commit, build and publish a new package on our release, then update
  the URL and hashes in the provider; until then Linux gets the stock package and 2x/8x abort again on Vulkan.

## 3. Unit tests

**Avalonia launcher (.NET 10).** The suites use Avalonia's headless platform with CPU Skia drawing and disposable
portable data. Windows and Linux use the same managed test projects; no disc/game is needed. Run with an existing
.NET 10 SDK and an owned scratch directory:

```powershell
dotnet run --project tools\launcher-controller-tests\LauncherControllerTests.csproj -c Release -- autotest\launcher-controller
dotnet run --project tools\launcher-layout-tests\LauncherLayoutTests.csproj -c Release -- autotest\launcher-layout
```

Test windows must pass their owned `DataPaths` and `enableServices: false`. That skips SDL/controller polling, game
launching, updates and OS animation lookup; tests inject controller samples/foreground state rather than OS input.
The controller suite checks neutral/repeat/deadzone gates, bumper tab switching (and the old bumper focus move where there are no tabs), owned modal routing, menu accessibility, settings and
same-value native selection intent, cancellation, browsing/text/folder creation, scrolling and persisted auto/manual
layout policy. Generic navigation regressions cover narrow buttons beneath wide fields, horizontal button rows,
ordinary slider precision and opt-in `ControllerSlider` coarse steps. Coarse steps retain the selected device's input
source, give horizontal D-pad input precision priority, clamp to bounds and preserve repeat/ownership/neutral gates.
The HDR window is covered too: the pad opens it from Display, the left stick steps paper white by 10 nits and peak by 100,
the D-pad by 0.1, ends clamp, the dropdown uses the themed popup and Back closes it.
The layout suite deliberately provokes save/background/UI faults to check error routing; those expected
errors appear on stderr even when the suite passes. Screenshots are synthetic headless evidence, not a hardware test.

Physical verification needs a separate isolated runtime allocation: test held confirm across modal transitions,
foreground loss/return, disconnect/reconnect, scheme matching/manual persistence and game launch/exit ownership.
Repeat on Windows and Linux (including X11/Wayland and native storage portals as applicable); Windows headless results
do not establish Linux hardware, SDL availability, native portal or compositor behavior.

The HDR launcher checks run with an existing .NET 10 SDK and an owned scratch directory:

```powershell
dotnet run --project tools\launcher-hdr-tests\LauncherHdrTests.csproj -c Release -- autotest\launcher-hdr
```

They use services-disabled portable fixtures and CPU-rendered Avalonia windows, not SDL/game/GPU input. Windows display
metadata is synthetic; unsupported-platform gating, missing/partial settings, saved precision, slider endpoints,
keyboard increments, modal ownership and save failures are covered. The opt-in controller slider metadata is checked
independently; stick-versus-D-pad dispatch is in the controller suite above.
A query that throws is covered too: it ends like a returned failure and the launcher still starts.
The native decision to stay SDR on a display that reports HDR off is `hdr_output_allowed` in `hdr_presentation_tests`;
`gx_readback_tests` also runs the HDR present shader (the real WGSL, an RGBA16Float target, both calibration patches).
The decision itself, made at device setup from the real display, shows only in the game log (an SDR PC logs "showing the standard picture" when `hdr = true`); the HDR-on side needs an HDR PC.
Compilation/headless checks do not establish HDR hardware or Linux compositor support.

The tests are CMake targets in `wiicompiled/runtime/CMakeLists.txt`; the game build doesn't build them. After a game
build has created `wiicompiled\native-build`:

```powershell
$tc = "$PWD\toolchain"; $env:PATH = "$tc\llvm-mingw\bin;$tc\Ninja;$tc\CMake\bin;$env:PATH"
cmake --build wiicompiled\native-build --parallel          # builds everything, tests included
ctest --test-dir wiicompiled\native-build --output-on-failure
```

One test: `cmake --build wiicompiled\native-build --target <name>`, then run `wiicompiled\native-build\<name>.exe`.
New pure logic (no SDL, no guest memory) belongs in a header with a test next to the existing ones.

**Aurora tests (Windows).** They are Aurora's own build, configured from `wiicompiled/aurora-main` (not its `tests/` folder), and the game build doesn't make them.
`tools/aurora-tests.py` (Python 3.8+, no packages) does all of this: it finds a built `Play` folder (or takes `--play DIR`), checks that SDL3 and the other libraries the test loads are there before running anything, configures only when the build folder isn't configured yet, builds the two targets, runs CTest with `Play` on `PATH` (Windows) or `LD_LIBRARY_PATH` (Linux) for that run only, and prints a summary.
`os_alloc_tests_NOT_BUILT` is listed as not run, not as a pass or a failure.
The exit code is non-zero on any failure.

```powershell
python tools/aurora-tests.py                       # every test; build folder autotest/aurora-tests-build
python tools/aurora-tests.py --filter gx_readback  # the GPU test alone
python tools/aurora-tests.py --play DIR --build-dir DIR2 --reconfigure
```

The script's own tests need no build: `python -m unittest discover -s tools/testkit -p "test_aurora*.py"`.
The script was run on Windows only; the Linux side (library path, the file check for libSDL3, clang) has not been run.

The same by hand, which ran on Windows only. From the checkout root:

```powershell
cmake -S wiicompiled/aurora-main -B BUILDDIR -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BUILD_GPU_TESTS=ON
cmake --build BUILDDIR --parallel --target gx_fifo_tests gx_readback_tests
# With a built Play folder and the toolchain CMake bin first on PATH, for this run only:
ctest --test-dir BUILDDIR --output-on-failure --timeout 300
```

Configuring downloads googletest and Dawn.
Add `-R gx_readback` to run the GPU test alone.
Without `Play` on PATH, `gx_readback_tests` stops with a Windows "SDL3.dll not found" dialog.
With only those two targets built, CTest also lists an `os_alloc_tests_NOT_BUILT` entry: it is a placeholder, and it counts as not run.


Aurora's standalone test build also has `gx_readback_tests` (`AURORA_BUILD_GPU_TESTS=ON`), a ROM-free test of
actual GPU pixels.
It starts with the motion vectors: none while nothing asks for them, zero for a still quad, the expected 8 pixels for one moved by its matrix, and the targets freed again when they are no longer wanted.
Two copies of a mesh animated on the CPU, one step out of phase (each frame one has the shape the other had), each keep their own motion: neither pairs with the other copy. When the first copy stops being drawn, the second keeps its own previous shape.
At 180 FPS interpolation, a mesh animated on the CPU that also moves by its matrix reads a third, then two thirds, then all of its motion in the frame's three images, its vertices' motion as well as its matrix's.
It then checks that a motion setting change resolves the pipeline again with no GX state written, that an alpha-blended surface owns its depth only where the GPU has `float32-blendable`, and that the blurred picture can be copied for a screenshot.
Then the scene stage: with its tint view, a solid quad is tinted and a see-through world quad drawn after it is not; its depth view shows the solid quad's distance; with the view off nothing changes.
Then ambient occlusion: a wall standing on a floor darkens just above the crease, the wall high up and the open floor don't shade themselves, and applied it darkens only the crease; turned off, its targets are freed.
Then a solid surface drawn a second time see-through (the game's two-layer materials) is tinted with the solid scene, as part of it.
Then a late blended surface in front of a second layer stays after it, and the second layers of solid draws that merged are recognised too.
Then a frame split mid-way, three ways (between two flushes, with the tail pending, in the middle of the tail), has no stage at all, and the next whole frame has it again.
Then ink outlines: a solid quad in front of nothing gets a line on its own edge only (not a pixel inside, not on what is drawn after the stage), the outline view shows it black on white, and off nothing changes. Bold's line is wider with nothing inside it, something thinner than the line keeps its colour, and a flat wall and floor stay clean. With no camera that can be inverted, the stage leaves the picture alone.
CTest runs it three times: as is, with `AURORA_GFX_FLAGS=validate`, and with `AURORA_GFX_FLAGS=validate,nofloat32blend`, which hides that feature.
Its transparency cases submit a cloud before late opaque foliage and check blending, foreground
occlusion, soft cut-out fringes, translucent submission order, viewport/scissor snapshots and clear/HUD boundaries.
Each transparency case explicitly clears its color and depth; the EFB otherwise persists between frames.
With the enhanced two-pass cut-out modes, each render pass finishes solid geometry, then cut-out fringes, then
depth-tested translucent world draws. Clears, overlays and non-monotonic depth passes flush that ordering.
A blended, depth-writing draw with no alpha test that arrives after translucent draws joins them in submission order,
so its depth can't hide the draws the game made before it; the cases cover it being invisible and writing depth.
Original-game and single-cut modes retain the game's submission order; the test checks those controls too.
This intentionally improves inherited foliage/particle ordering in the enhanced modes, rather than claiming a
mismatch with console rendering. Compatible direct XYZ/RGBA/ST camera-facing triangle-pair particles are stably
sorted back-to-front across emitter batches, without changing their alpha or depth tests. This prevents distant
ground mist from blending over a nearer cloud everywhere except where opaque scenery blocks that mist.
Uniformly colored, rectangular-UV planar sprite quads also participate: rotating leaf particles must not
separate a nearer cloud from distant mist. Tilted sprites require four unique vertex records forming
a parallelogram with a shared diagonal and use its centre depth; this is particle-level sorting,
not per-pixel intersection sorting.
Particle index views share the original vertex/uniform uploads. The GPU regression includes indexed opaque
geometry and a batch containing particles on both sides of a separately submitted cloud, equal-depth
stability, and unsupported-geometry/blend/camera/scissor barriers. The same cases exercise the raw bridge,
ordinary `GXBegin`/`GXEnd`, and both mixed submission orders. Normal pass encoding, explicit deferred
flushes and detached sealed-frame encoding all apply the same particle sorter.
The same four paths check a rotating sprite between the cloud and mist at full, partial and zero cloud alpha,
including the sprite remaining visible after the cloud fades.
Sorting does not cross viewport/scissor changes, camera/projection changes, other translucent geometry or
clear/copy/pass boundaries. Non-planar, indexed, unusual-depth and frame-interpolated draws keep submission order.
The single-cut and Original/Game controls remain unchanged.

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
  default is on, so a script that starts the game has to set it: `manual-session` writes it, `level-sweep.py` and the smoke test
  write it in their own copies' configs (test kit, section 4a), and `play-patched`, `iterate` and `capture-window` set it for the run and put your
  `Config.toml` back afterwards. Sound is muted the same way in `iterate` and `capture-window` (no switch) and in the Python kit; those two wait for the
  game-run lock and give it back when the run ends. The helpers are in `tools\debug\kit-common.ps1`.
  The PowerShell scripts take turns through that lock, one at a time. The Python test kit (section 4a) uses shared run slots
  instead and respects the same lock both ways, so old and new scripts never overlap.
  Scene IDs, level names and which scenes suit which tests: [`tools/debug/game/levels.md`](../tools/debug/game/levels.md),
  or `python tools\debug\game\levels.py i004`. Common ones: `h001` (hub, smoke test), `i001`, `i004` (boot it directly),
  `e001`.
- `status` shows the running session; `stop-tests` kills leftover automation.
- Settings not in the generated config, or launching the game yourself (e.g. to capture its stderr): run `prepare`
  (same options as `start`; saves your settings, writes the config and boot mod, doesn't start the game), add your
  section to `Config.toml`, launch the exe, and `stop` when done (`shot` captures any running game). Example:
  `tools\debug\holiday-shots.ps1`. The other scripts in `tools\debug` that start the game do the same.
- Don't run several game windows or level-sweep automation for visual bugs: one window, looked at by a person.

## 4a. Smoke test and the test kit

`python tools/smoke-test.py` (Python 3.8 or newer, Windows and Linux, standard library; the stuck-music check needs numpy) boots the
game for real from a fresh build and checks it still works. Run it before a PR, after `toolsuild-no100f.ps1`.
Three jobs run at once, each from its own copy of `Play\`:

| Job | What it does and checks |
| --- | --- |
| `h001` | Boots the hub with the HD pack on, skips the intro, waits until it is playable. At least 20 of the pack's textures were really drawn (every pack file is registered with the dev menu's `textures` command, which lists the ones the game drew: not "the picture differs"). The audio dump is not silent and `tools/debug/audio/loopcheck.py` finds no stuck loop. The picture is not black. |
| `i004` | HD off: none of the pack's textures are drawn. A scripted A press moves Scooby. The dev menu's `scene e001` travels there and the game becomes playable again. |
| `g001` | Sets a known snack count, `tp`s onto the save point (-25.618 1.186 -1.515), saves with scripted presses, finds the file in `Saves\MemoryCard`, restarts from the same copy, loads from the title menu and checks Scooby is back at the save point with the same count. |

`--scenes e005,f001` adds a job per scene: boots it, waits until playable, checks the picture and that the game stays up.
Every job first checks that the game was built from the code under test: the build ID (`1.1.1 branch@commit`, in the exe and in `console.log`) must be this checkout's commit, or an older commit of the same branch with only
test and doc files changed since (`tools/testkit/`, `tools/smoke-test.py`, `docs/`, any `*.md`, `autotest/`). Any other change since the build says "rebuild".
All jobs fail on a crash, a fault in the log or a black picture. Only a timeout is retried, once from a fresh launch, and reported as "passed on retry" (`h001`
has a known intermittent intro hang). A check that cannot run (no numpy, no screenshot tool on Linux) is "not run", never a pass: exit code 3.
Exit codes: 0 passed, 1 failed, 2 wrong build, 3 only checks not run. The report is `autotest\smoke-<time>\REPORT.md` (with `results.json`, shots and logs).
Options: `--jobs`, `--scenes`, `--slots N`, `--timeout S` (playable limit per job, 150), `--keep` (keep the `Play` copies), `--exclusive` (the machine to yourself) with `--sequential` (one job after another: solo timings), `--activate-when-shown 0|1` (sets `SDL_WINDOW_ACTIVATE_WHEN_SHOWN`), `--skip-build-check` (manual override, said in the report).

Measured on one PC (Ryzen 9 7900X, RTX 3090, 2026-11-07, 1280x720 windows): the three jobs at once take about 47 s after the build (h001 27 s, i004 20 s, g001 47 s, each playable after 8-14 s); one after another 85 s (11.5 + 6 + 12 s to playable). Every window held 60 fps (p95 frame time under 17 ms) whether it was in front of the others or behind, with or without `--activate-when-shown 0`: nothing in the game or runtime sets `pauseOnFocusLost` (it is off), and background windows did not stall.
A fresh `Play` copy starts with an empty shader and pipeline cache (`UserData\Cache`), which costs about 3.4 s (h001 playable in 13.4 s cold, 10.0 s warm); each game also reads about 1 GiB of disc files into its read cache and starts 22 pipeline compile threads, so more than 3 at once will slow each one down.

The pieces are in `tools/testkit/` and are meant for every automated game run (the smoke test and the level sweep use them; the PowerShell scripts have not been moved onto them yet):
- `launch.py`: the one launch helper. Its own copy of `Play\` in a folder you give it (the HD pack and button icons are linked, never copied; deleting never follows a link),
  its own `Config.toml` (windowed, rumble off, muted), the environment scrubbed of leftover `NO100F_*`, `AX_*`, `AUDIO_*` switches and of
  `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS` (never set it: SDL then refuses the game's own hint call and the game treats that as an error).
  Controller isolation, per game process only, so your own game is unaffected: `NO100F_INPUT_SCRIPT` set (the game reports pad 1 connected, nothing pressed) and
  `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0xDEAD/0xBEEF`, an ID no controller has, so a real pad's presses do not reach the game
  (tried once with a person pressing a real pad throughout a full smoke run: all three jobs passed as without; the game logs no controller connects, so there is no log line to check).
- `slots.py`: shared run slots, 3 by default (`--slots`, or `FRIGHTWORKS_TEST_SLOTS`). `acquire(why, exclusive=True)` is for the human check and performance
  checks: it waits for every slot and blocks new ones. Files live in `%LOCALAPPDATA%\FrightWorks-kit\slots` (Linux `$XDG_STATE_HOME/FrightWorks-kit`), with a heartbeat so a
  crashed run's slot is taken over after 90 s. While any slot is held a `game-run.lock` exists in the old format (its `root` is the slots folder, so no checkout's old script takes it over), and a lock held by an old PowerShell script makes
  the kit wait: `python tools/testkit/slots.py status` shows who holds what.
- `devmenu.py` and `waitplay.py`: the dev menu's `devmenu.txt` / `devmenu_press.txt` as a client, and the one wait-until-playable routine (from `texhunt\look.ps1`): uses `cutscene`
  (`cutscene yes|no control yes|no`): skip (A) every 2 s while a cutscene plays; with no control and no cutscene it waits 4 s (a camera pan ends by itself), then presses A every 2 s, at most 12 times, for a dialogue box ("PRESS A TO CONTINUE"), logging each press; ready the moment the player has control; a time limit fails with the last state and a screenshot.
- `checks.py`: black-picture test (PNG, no Pillow), audio peak and loop check, the HD-drawn probe, the frame rate from the game's own `fps` log lines.
- `recorder.py`: the per-job / per-level record of checks (passed, FAILED, not run) that the smoke test and the sweep print into their reports.

Tests of the kit need no game: `python -m unittest discover -s tools/testkit -p "test_*.py"` (about a minute; uses a windowless stand-in game).

## 4b. Level sweep

`python tools/level-sweep.py` boots every level on the disc (87: every folder but `MN` and `fmv`) for real, one after another on up to 7 windows at once, and checks each.
Run it after a fresh build, before a release; it is not part of every PR (use the smoke test for that, section 4a).
Same kit as the smoke test: each worker has its own copy of `Play\` (rumble off, muted, controller isolation, own config), and a worker takes a shared run slot for each level, so a smoke test or another checkout's run shares the machine instead of waiting for the whole sweep.
The build ID must match the commit under test (same rule as the smoke test).

Per level: boot straight into it (`sd2.ini` `BOOT=<level>`, saves cleared), wait until it is playable (the kit's routine: skips a cutscene, waits out a camera pan, no fixed time, no blind presses),
keep running `--seconds` (8) more, then check:
- alive, and no `AccessViolation`, `OS panic`, `missing_target`, `Unhandled` or `FATAL` on stderr: a fault or an exit fails the level at once (`failed: crash (access violation | panic | exited)`);
- the dev menu says the scene is the level (`failed: in <level>`);
- the picture is not black (a screenshot, Windows only; "not run" elsewhere). A black one is shot again up to 4 times, 3 s apart, before it fails, and a retake that was lit is noted in the report. With nobody at the pad the monsters of `o005` and `p002` catch Scooby at the spawn within seconds (health 3, 2, dead, then back at 3 about every 13 s), and the screen is black during the game's own death fade until he respawns. A picture that stays black through all five shots (12 s) still fails;
- frame rate: the game's own `fps` lines (the sweep sets `NO100F_LOG_FPS`) after it was playable; the median must reach `--min-fps` (default half the target: 30 at 60 fps, 15 at 30 fps).
A level that does not become playable in `--timeout` (150 s) gets one more try from a fresh launch ("passed on retry"); a crash or a failed check is final.
A check that could not run is "not run" (`passed (some checks not run)`, exit code 3), never a pass.
A scene in `KNOWN_EXCEPTIONS` (in `level-sweep.py`, with the reason) is not run: `REPORT.md` lists it as "skipped (known)", never as passed, but it does not count against the run (exit code 0 when everything else passed); `--include-known` runs it anyway. Today that is `s006`, the ending (`OUTTRO` cutscene, not a playable level: it is the one scene on the disc missing from the level table in `tools/debug/game/levels.md`).

What the sweep sets, on top of the game's defaults (the kit's test config, the same as the smoke test): windowed 1280x720 with the render size following the window and 16:9, `skip_intro`, MSAA off (so 7 windows stay fast), HD textures off (the smoke test covers them),
`fps60 = true` (`--fps30` sets `fps60 = false`: 30 fps), muted, rumble off. Frame interpolation is not set, so it is off: the old script wrote a target of 120.

Options: `--levels h001,c003`, `--include-known`, `--workers N` (7), `--slots N` (as many as workers), `--seconds S`, `--timeout S`, `--fps30`, `--min-fps N`, `--keep`, `--exclusive` (the machine to yourself), `--out DIR`, `--activate-when-shown 0|1`, `--skip-build-check`.
Exit codes: 0 passed (known exceptions skipped), 1 a level failed or is missing, 2 wrong build, 3 nothing failed but a check could not run. Output: `autotest\level-sweep-<time>\` with `REPORT.md` (problems first, then one row per level: outcome, seconds, seconds to playable, fps median and min), `results.json`, `shots\<level>.png` and `logs\<level>-<attempt>.err.log` / `.console.log`.
Measured on one PC (2026-11-08): `h001`, `i004` and `w020` at once passed in 28 s (playable after 11-17 s, 60 fps median in each), and `i004` with `--fps30` in 19 s (30 fps). The first full run (87 levels, 7 windows, fps60) took 538 s: 83 passed and 4 failed. The four were `f003` (a dialogue box nobody dismissed), `s006` (the ending cutscene, which is not a level) and `o005` and `p002` (a black picture caught during the death fade); after the dialogue presses, the retake and the exception above, `f003`, `o005` and `p002` pass.

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

**Motion vectors view** (F8, or `mvview MODE [SCALE]` in `Play\devmenu.txt`: 0 off, 1 motion only, 2 over the picture) shows where every surface was last frame, as color.
Grey (or the picture) is still; the hue is the direction a surface came from and the saturation its speed, full at SCALE pixels per frame (16 by default).
It turns the motion vectors on while it shows, like the **Motion blur** setting (`[video] motion_blur`, 0 to 100 percent), which uses them.
Anything that should move but shows grey, or flashes from frame to frame, has lost its pairing with last frame's draw.

**Scene stage view** (F8, or `stageview MODE` in `Play\devmenu.txt`: 0 off, 1 tint, 2 depth, 3 occlusion, 4 outlines) shows where the solid scene ends in each frame.
Tint turns everything drawn before the stage blue: the see-through world draws (fog, particles, glow, the lit windows), the sky and the HUD come after it and keep their colour.
Depth shows the solid scene's distance from the camera, near = white.
Something solid that keeps its colour under the tint, or something see-through that turns blue, is drawn on the wrong side of the stage.
Occlusion shows the **Ambient occlusion** setting's shading alone at full strength (white = none): it belongs in creases, corners and under things, never across flat walls or floors.
`ao PERCENT` in `devmenu.txt` switches the setting's strength for one run to compare (0 off, 35 low, 65 medium).
Outlines shows the **Ink outlines** lines alone, black on white (Bold when the setting is off): lines belong on silhouettes, never across flat walls or floors, and grass and stems keep no solid black.
`ink LEVEL` in `devmenu.txt` switches them for one run (0 off, 1 thin, 2 bold).

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
[`.agents/skills/manual-yn-bisect/strip-before-pr.md`](../.agents/skills/manual-yn-bisect/strip-before-pr.md). The most
useful:

| Patch | What it adds |
| --- | --- |
| `devmenu` | F8 dev menu for branches from before it was built in (above) |
| `postool` | Live memory: write `dump NAME`, `peek ADDR [N]` or `poke ADDR HEX...` lines to `Play\postool.txt` (run every 20 frames, output in `Play\postool\`); **F9** saves Scooby's position to `Play\postool\spots.txt` |
| `audio-diagnostic` | Per-stage audio dumps (`AX_DIAG_DIR`) |
| `real-date` | Real clock for old builds that always start on 2000-01-01 |
| `pad-script` | Scripted pad 1 with the stick and timed buttons, dev tools on or off (`NO100F_PAD_SCRIPT=<file>`; usage in the `game-logic-debug` skill) |

## 7. Logs, scripted input and other switches

- **Logs**: `Play\UserData\Logs\<time>\console.log`; the newest one shows which level files load (`[dvdread] ...
  xNNN.HIP`). Runtime messages (`RT_LOGF`) go to stderr: launch with
  `Start-Process "Play\FrightWorks Game.exe" -WorkingDirectory Play -RedirectStandardError err.txt`.
- **Scripted input** (no controller needed): set `NO100F_INPUT_SCRIPT` before starting the game, e.g.
  `$env:NO100F_INPUT_SCRIPT = '10000:PLUS,12000:DOWN,13500:A'` = press Start at 10 s, D-pad down at 12 s, A at 13.5 s
  (ms after the game first reads input; optional third field = hold time in ms, default 150). Buttons: `A`, `B`,
  `PLUS` (Start), `ONE` (X), `TWO` (Y), `MINUS` (Z), `UP`/`DOWN`/`LEFT`/`RIGHT`.
- **Button prompts**: `$env:BUTTON_ICONS = 'xbox'` (or `playstation`, `nintendo`) forces a prompt set; the game puts
  the prompt picture together from one icon per pad button, following player 1's mapping. The icons are built by
  `python tools\make-button-icons.py --out Play\button_icons` (needs ImageMagick and the HD pack).
- **Audio capture**: `$env:AUDIO_RAW_DUMP = 'C:\temp\out.raw'` writes the mixed output; analysis tools in
  `tools\debug\audio\`.
- Remove test variables afterwards: `Remove-Item Env:NO100F_*, Env:AX_*, Env:AUDIO_*, Env:BUTTON_ICONS -ErrorAction SilentlyContinue`.
- **Launcher screenshots** without opening it: `& "launcher\publish\FrightWorks.exe" --shots <dir>` (Linux:
  `launcher/publish/FrightWorks --shots <dir>`) draws every page to `<dir>\<Page>.png`. The window takes the size the
  window manager gives it; for fixed sizes use the layout regression below.
- **Patch mods** (`docs/mod-patches.md`): `dotnet run --project tools\hip-patch-tests` builds levels from made-up data and
  tries bad recipes, wrong discs, clashes, killed builds and hostile zips. Needs only dotnet; the disc modes are in that page.
- **Isolated launcher layout regression** (Windows or Linux, .NET 10 SDK or newer):
  `dotnet run --project tools\launcher-layout-tests\LauncherLayoutTests.csproj -c Release -- autotest\launcher-layout`.
  It runs the launcher's real window on Avalonia's headless platform (no screen needed, drawn with Skia like on
  screen) and checks Settings/Display, Controls, Mods and Extras at 1180x830, the 960x680 minimum, 820x600 and 500x547
  (what a tiling window manager can force): nothing wider than its page, no horizontal scrolling, usable drop-downs,
  reopening keeps the layout, the wheel over a closed drop-down never changes a setting, long mod names, transparent
  plank corners (normal, hover, pressed, disabled), keyboard activation and accessible names, the Xbox-only rows, the
  fullscreen monitor row (Windows only), and that custom HDR and unknown config values survive. On Linux it also clicks
  Play with stand-in scripts for the game: it starts in its folder and the launcher minimises, and a game that
  exits with an error brings the launcher back with a message. It uses
  a disposable portable fixture and writes page screenshots only to the output directory; it never starts the real
  game or touches the player's data. Display scaling is Avalonia's (sizes are device-independent), so check a real
  window at 125-200% by eye too.
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
- Run what the change needs (the table in `.agents/REPO.md`, Testing): the unit tests it affects, and a full build if you touched overrides.
- Smoke test: build first, then `python tools\smoke-test.py` (add `--scenes <scene,...>` for a level you changed; section 4a); before a release, the level sweep (section 4b). Keep it short; nothing runs it automatically (no CI), so the PR says what was run, and what it could not run.
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
- `worktree.ps1` is a thin wrapper: the work is done by `tools\debug\worktree.py` (Python 3.8+, standard library only).
  The wrapper finds a real Python (not the Microsoft Store stub) and passes every argument through, so the commands
  above are unchanged. `remove` unlinks every junction/symlink first (at any depth), checks none is left, and only
  then lets git remove the folder: it never deletes recursively through a link, so `Disc\`, `toolchain\` etc. of
  the main checkout are safe. It refuses rather than guess when it cannot list the running programs.
- `-Model <name|full ID>` and `-Effort <low|medium|high|xhigh|max>` (on `new` and `open`, also with `-Continue`) are
  passed to `claude` as `--model` / `--effort`, only when given. They are not remembered: a later `open` without them
  starts on the defaults. No semicolons in `-Model` (Windows Terminal splits its command line there).
- `TASK.md` in the worktree holds that task's notes; the agent notes (`AGENTS.md`, local, not in git) are shared.
- To try a worktree's build, run its `Play\FrightWorks Game.exe`, or build its launcher there
  (`toolsuild-launcher.ps1`) and run its `launcher\publish\FrightWorks.exe`: it starts that worktree's game, with its
  own settings.
- Builds can run side by side. Game runs take turns in the PowerShell scripts: `manual-session prepare/start` and `play-patched.ps1` wait while
  another checkout's test has the game open. Scripts only close the game they started from their own checkout; your
  own game (launcher or `Play\` in the main checkout) is never closed by a test.
- Never delete a worktree folder by hand: `Disc`, `texture_pack`, `toolchain` there are links, and some delete tools
  follow links into the main checkout. `remove` unlinks them first; it refuses while the worktree has uncommitted work
  or a program running from it (leftover dotnet build servers it stops itself), and while its Claude session runs:
  close that tab first.

## Agent skills

`.agents/skills/` has step-by-step guides for coding agents, readable by anyone (the shared rules for agents are in `.agents/REPO.md`): `hle-divergence-debug`
(graphics and state bugs), `game-logic-debug` (60 fps, timing and gameplay bugs, dev menu, postool),
`manual-yn-bisect` (yes/no visual bisects, the fix-only PR workflow) `audio-debug` (crackle, clicks, fuzz) and `parallel-work`
(worktrees and their sessions, run from the coordinator).

Claude Code looks for skills in `.claude/skills/`, which is git-ignored. Run `python tools/setup-agents.py` once in each
checkout (`py -3 tools\setup-agents.py` on Windows) to link it to `.agents/skills/`: a junction on Windows, a symlink
elsewhere. It is safe to run again and refuses to replace a real folder that has files in it. `worktree.ps1 new` and `open` run it
for a worktree (skipped on a branch that has no `.agents/` yet); the main checkout runs it by hand once after it has pulled `.agents/`.
