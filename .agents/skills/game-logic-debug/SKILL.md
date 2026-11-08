---
name: game-logic-debug
description: >-
  Root-cause gameplay, timing and simulation bugs in the Scooby-Doo recomp: things that behave differently at 60 fps
  ([video] fps60), particles that flicker, pop or double, objects in the wrong place, anything where the game's own
  state (not GPU state) goes wrong. Covers parking Scooby at a fixed spot (mkhip.py + F9 spots), reading/poking guest
  memory live (postool), counting particles per system/emitter, and measuring flicker from recordings.
---

# Game-logic / frame-rate debugging

GPU state bugs (wrong geometry, textures, blend) → `hle-divergence-debug`. This skill is for when the *game's* data is
what changes: positions, timers, spawn counts, pools. The usual cause in this port is a game loop that assumed 30 Hz.

## How fps60 works (and how it breaks things)

- `[video] fps60`: `VIGetRetraceCount` doubles the count for `lr == 0x800AD690`, so the main loop runs twice per 30 Hz frame
  and the game steps everything by the measured `dt` (half as large).
- Code that scales by `dt` is fine. Code that acts **per call** (emit N particles, add a fixed step, count frames) does it twice
  as often. Find the per-call site and make it run at the game's 30 Hz rate; don't touch the consumers.
- Fix pattern (PR #14, `hle/no100f_particles.cpp`): native override of the per-frame caller that accumulates `dt` per object
  and calls the original once the sum reaches 0.75/30 s, passing the summed `dt`. Gate on `RuntimeConfigFile::Fps60()` so 30 fps
  is byte-for-byte unchanged. A 30 fps frame (slowdown) still reaches the threshold alone.

## Order of work

1. **Measure the symptom** with a fixed view: park Scooby (below), record 6 s at fps60 on/off, `flashcount.py` each. Check
   Dolphin too (`hle-divergence-debug` step 5) — "Dolphin is smooth" separates port bugs from game bugs.
2. **Find the data**: postool `dump` MEM1, `findpos.py` for a known world position, `addrrefs.py` for code that builds an
   address, `dol.py callers/func` for who writes it. `trace ADDR N` follows a linked list per frame.
3. **Count, don't eyeball**: per-system pool use (`parsys.py`), per-emitter spawn rate at 30 vs 60 (`births-run.ps1` + `births.py`).
4. **Fix at the per-call site**, re-measure with the same tools (rates within noise of 30 fps, flash count 0), then human visual
   check if the change is visible, and the smoke test in `.agents/REPO.md` (Testing: `python tools/smoke-test.py --scenes <changed level>`).

## Parking Scooby at a fixed spot (`tools/debug/game/mkhip.py`)

Pick the scene from `tools/debug/game/levels.md` (scene IDs, level names, which scenes have torches/fog/rain/...).

The camera can't be turned, so a fixed spawn gives a fixed view. `mkhip.py` writes a patched level HIP into the `zz-manual`
boot mod that `manual-session start` creates (game data: local only).

```powershell
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 prepare -Boot i004 -AllPowerups   # saves user setup, writes zz-manual + Config
python tools\debug\game\mkhip.py i004 --spot spot1            # spawn there, all NPCs moved off-map (--npc none keeps them)
#   or: mkhip.py i004 --pos X Y Z --yaw R
Start-Process 'Play\FrightWorks Game.exe' -WorkingDirectory Play   # not manual-session start: it rewrites zz-manual
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 stop   # when done: game closed, user setup back
```

- Spots: with `postool.patch` built in, **F9** (global hotkey) appends Scooby's position + yaw to `Play/postool/spots.txt`
  (beeps). Copy good ones to `tools/debug/game/spots.txt` (i004 grate spots are there). F9 saves the feet; mkhip adds 0.3 to Y.
- NPCs: clearing `baseFlags & 1` does nothing (NPC code ignores it), so mkhip moves them to (2000+50i, -500, 2000).
- Poking only Scooby's matrix doesn't stick (the frame rebuilds it); the dev menu's teleport (below) does, or use the spawn.
- Clean up afterwards: `manual-session.ps1 stop` (or `restore`) puts the user's Config/controller files back and deletes
  `Play\UserData\mods\zz-manual`. Don't `stop` between `prepare` and launching: it restores the user's config.

## Dev menu (`no100f_devmenu.cpp`; `tools/debug/patches/devmenu.patch` for older branches)

Built into the game, active with `[game] dev_tools = true` (launcher Extras > Developer tools) or `NO100F_DEV_TOOLS=1`
(set by the kit's scripts). Run: `powershell -ExecutionPolicy Bypass -File tools\debug\play-patched.ps1` builds and
starts the game with it on; on a branch from before it was built in, it applies the patch and takes it out of the
source when the game closes (`-SkipTranslate` only if the last build was this branch;
`-CleanBuild` rebuilds `Play\` without it afterwards; `-Keep` to work on the menu, then re-export the patch and `-Remove`).
`-HdTextures` plays with the HD texture pack like the release does (`texture_pack\ScoobyHD` linked into
`Play\texture_replacements` if a build hasn't staged it there, `texture_replacements = true`; both put back afterwards). The user's config has HD off,
so a bug seen in the release but not in a dev build may be an HD texture: retry with `-HdTextures`.

**F3** in game (same gate, `no100f_recorder.cpp`): video to `Play\Videos\*.mp4` + `.txt` (build, start/end position,
settings). Frames come from aurora (`aurora_set_video_capture`: presentation image re-rendered at window size, max 1080
lines, read back async, 60 fps CFR piped raw to ffmpeg); `[game] record_audio` (default on) muxes the game's output in
by wall-clock time; `[game] capture_overlay` (default off) = F8/F10 in F3 videos and F2 shots, else only the DEV mark
(ImGui windows `##CaptureStamp` / `##CaptureHidden`, aurora.h). Needs ffmpeg (PATH or `tools\debug\local\ffmpeg`; the
launcher downloads it when dev tools get turned on). Handy for the human to record a bug while playing; test driver
`autotest\f3-recording\rec-test.ps1`.

**F8** in game: current scene + Scooby's position, go to any scene (start position), save/delete spots and go to them
(another scene loads first; one transfer at a time, queued until in play with no cutscene, Cancel travel, 60 s
timeout; travel turns the free camera off), Skip button (presses the game's own A in a recorded cutscene, Start on the
title intro / attract demo, for up to 10 s), god mode (no damage, green Scooby), Levels tab, Monsters tab (all off in every level, or one
by one in this scene), Player tab (health/snacks and power-ups: unlock/lock all or one by one). Spots: `UserData/devmenu_spots.txt` (`I004 x y z yawRad name`). Scriptable through
`Play/devmenu.txt` (every 30 frames, then deleted; results in `Play/devmenu.log`; the file is left alone until in play with
no scene/spot transfer pending, except diagnostic-only files below, `skip`, or, with nothing pending, `scene`): `scene ID`, `skip`, `spot NAME`,
`save NAME`, `tp X Y Z [YAWDEG]`, `where`, `peek ADDR [N]`, `frame`, `god on|off`, `powerups [all|none|HEX]`, `powerup NAME on|off` (sd2.ini name without `eSPECIAL_`), `monsters [on|off]` (list / all),
`monster N|NAME on|off`, `ents` (scene entity list), `textures`, `freecam on|off`, `cam X Y Z YAW PITCH` (free camera at
this pose, degrees; pitch > 0 looks down), `camspin DEG` / `cammove DX DY DZ` (the free camera turns or moves this
much every frame: steady motion for frame-to-frame checks; 0 stops). These commands don't edit warp flags or save files.
`cutout N` (cut-out draw mode, needs aurora's `AURORA_HAS_CUTOUT_MODE`; F7 cycles), `perf` (draw calls + present
timing), `mvview MODE [SCALE]` (motion vectors view: 0 off, 1 motion only, 2 over the picture; SCALE = pixels per frame
for full color), `stageview MODE` (scene stage view: 0 off, 1 tint what is drawn before the stage, 2 the solid scene's
depth, 3 the ambient occlusion alone, 4 the ink outlines alone), `ao PERCENT` (ambient occlusion strength for this run,
the setting unchanged), `ink LEVEL` (ink outlines for this run: 0 off, 1 thin, 2 bold, the setting unchanged),
`bloom PERCENT` (bloom strength for this run, the setting unchanged), `bloomview MODE` (0 off, 1 the glow alone, 2 its
source: the light draws alone), `lighttex add|del KEY | list | clear` (the bloom's light textures for this run, by
texture pack name; `NO100F_TEXLOG=1` logs every texture a level draws to `devmenu_texlog.csv`),
`cutscene` (`cutscene yes|no control yes|no`).

**Player tab:** `player`, `health 1..5`, `snacks 0..99999` read/change live stats. Balance edits are separate from
collection progression; 99999 is a conservative tool limit, not the game's cap. No kill/resurrection; pending
snack-gate payments reject edits. Advanced `snackareas` lists named areas/counts/live targets; `snackarea INDEX COUNT`
stages one area-counter edit, `snackconfirm` applies it once, `snackcancel` cancels. Scene/availability/count/target
changes invalidate confirmation. This can affect natural picture-gallery access and later normal saves, but does
not collect/remove/respawn snacks, alter spendable balance or reconcile scene-local pickup counts. No save/load
invocation. The old Power-ups controls remain inside Player, with unchanged Map-first order/keys and commands.

**Warps / Gallery / Diagnostics / Resources tabs:** `warps` lists the 26 discovery entries;
`warp E004 on|off` changes one live discovery flag (approach can unlock it again; normal saves can retain it).
Map, gadgets and room discovery remain separate. `gallery` reads natural state and overrides in H003;
`gallery pictures|movies natural|locked|unlocked` tests a section without changing snacks, watched flags or story
progress. Overrides clear on leaving/loading/title; reopen/browse the gallery to refresh visibility. Rule changes
do not dispatch UI events. Normal playback may still update watched state. Missing/modified schemas fail closed.
Edits require stable gameplay/control with no cinematic/movie/transfer; diagnostic-only files are consumed with
explicit rejection, never queued into a later scene. `receipt` and `readiness` are read-only local log reports.
Resources samples this process's CPU/RAM about once per second while visible (CPU normalized to machine and one core;
working set and private committed bytes distinct). Process GPU utilization is unavailable with a stated reason, not
replaced by FPS/system usage. Full usage and persistence caveats: `tools/debug/README.md`.

Level intros (first visit only): a recorded cutscene sets the cutscene manager pointer `0x802459F0` (globals + 0x860) and
is skipped with pad A once the game allows it (per-cutscene delay); `0x80245540` (globals + 0x3B0) is 1 only while the
player has control (0 in cutscenes and in-world camera pans such as the i001 key, which can't be skipped; dialogue boxes
like w028's Fred are in control and advance with A). The game counts all of these as "in play". Button presses for
scripts: `Play/devmenu_press.txt`, read every 30 frames even outside play, lines `BUTTON[+BUTTON] [FRAMES] [cutscene]`
(`cutscene` = only while one plays), injected into pad 1 by PADRead. `tools\debug\texhunt\look.ps1` presses A every 2 s
while `cutscene yes`, then waits for `control yes`.

**Stick and timed input without dev tools:** `tools/debug/patches/pad-script.patch` adds a scripted pad 1 (`devmenu_press.txt` needs dev tools and `NO100F_INPUT_SCRIPT` has no stick). A route file has one step per line: `wait MS`, `waitmode MODE STATE`, `waitplay SCENE`, `do SX SY BUTTONS PERIOD_MS DURATION_MS [until SCENE | untilmode MODE STATE]`; `#` starts a comment. The full format is at the top of `no100f_pad_script.h`, which the patch adds; read it in the patch file. One bad line means no step of that file runs, and stderr says which line. `NO100F_PAD_SCRIPT_LIVE=<file>` takes more steps while the game runs. Routes: `tools/debug/game/pad-walk-check.txt`, `tools/debug/audio/issue14-route.txt`.

```powershell
git apply tools\debug\patches\pad-script.patch
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1 -SkipTools -SkipTranslate   # first build in a checkout: full
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 prepare -Boot e001  # muted, rumble off, boot mod
$env:NO100F_DEV_TOOLS = '0'
$env:NO100F_PAD_SCRIPT = (Resolve-Path tools\debug\game\pad-walk-check.txt).Path            # full path: the game runs in Play\
Start-Process 'Play\FrightWorks Game.exe' -WorkingDirectory Play -RedirectStandardError autotest\pad-script.log
# wait on the [padscript] step lines in the log; manual-session.ps1 shot takes a picture
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 stop
Remove-Item Env:NO100F_*
git apply -R tools\debug\patches\pad-script.patch                                           # next build: full translate
```

For dev tools on, `manual-session start` passes the variable to the game. A real pad still counts, and the game reads it without focus, so leave it alone during a run. On Linux `manual-session` isn't available: set the same variables, set `[audio] muted` and `[controller] rumble = false` yourself and run `Play/FrightWorks Game`.

Textures tab (for HD pack audits): reads `UserData/devmenu_textures.txt` (tab-separated `id file severity asset scenes level`,
optional seventh column starting with `effect`, any case = Effects tab instead; `#` comments; the Reload button re-reads
it; each tab keeps its own "This level only" / "Hide fine" filters) and lists those replacement textures per scene, each marked "drawn" once the game has used it since start
(aurora `aurora_texture_replacement_used(name)`, added by the patch: names are recorded when a replacement is looked up).
Each row can save a spot named after the ID or go to it; suspects whose scenes include `boot` count in every scene.
Find (row button or `find <id>`) draws that replacement solid magenta and writes each distinct GX state it is drawn with to
`Play\devmenu_drawstate.txt` (needs `texture_replacements = true` and a pack file for the key).

Texture log ("Log every texture" checkbox, or env `NO100F_TEXLOG=1` at start): the first draw of each texture key with
each distinct sampler + GX state in each scene appends a row to `Play\devmenu_texlog.csv` (scene, key, replacement, texmap,
size, wrap, filters, mips/LODs, alpha compare, blend, z test/func/write, z-before-tex, colour/alpha write, uploaded size).
Disc textures without a pack file are logged too (every static texture is named at upload, `TextureRef::debugKey`).
Take it with HD on: aurora keeps palette (TLUT) mirrors only when `texture_replacements = true`, so with HD off every
C4/C8/C14X2 key gets a zero palette hash. Rows tagged `none` were drawn before the scene ID was set (boot, first frames).

Scripts around the dev menu: `tools\debug\texhunt\` (`look.ps1` open at a spot, `spots-run.ps1` shots, `hunt.ps1` sessions;
list in `tools\debug\README.md`). "Which geometry samples this texture?": `texhunt\tint-pack.py --hip <HIP> --match <name>
--out autotest\texhunt\tint-x`, then `look.ps1 ... -Extra autotest\texhunt\tint-x` (found the dormer roof + sill using the
Creeper frames). i004 fog bank from a running dev menu build: `tools\debug\game\fogdump.ps1 -Label x`.

- Scene switch = the game's portal path: write a portal + asset (+0x08 camera, +0x0C marker, +0x10 yaw deg, +0x14 scene ID)
  in a reserved page (0x816FF000, lowers the arena by 4 KB) and set the pending portal `[[0x80245CF4]+0x70]`. The scene
  update fades out (state 4), handler `0x800ACD30` (mode 0xB, state 5) loads it. Marker 0 = the scene's own start.
- Teleport: player entity frame `[0x802453C8]`: matrix (pos +0x30), previous pos +0x80, yaw angles +0xA4 and +0xC4; plus the
  model matrix `[[0x802453B4]+0x20]` and the camera target (0x801BD0A0/AC). Writing only the matrix gets rebuilt.
- In play = mode `[0x80270E18]` 0xB and state `[0x802703C8]` 1. Current scene ID `[[0x80245CF4]+0x6C]`.
- Power-ups: live mask `0x80245748` (globals + 0x5B8), bits 0-14 in sd2.ini read order: BubbleGum, SoapBar, StickyBoots,
  Plungers, Slippers, LampShade, BlackKnight, FlowerPot, DivingHelmet, Spring, LightningBolt, FootballHelmet, Umbrella,
  Shovel, LightningBoltStun. Pickup ORs `1 << [pickup+5]` in (`0x8004F0FC`); pause cheat `0x800ABF08` sets all; a new
  game copies the eSPECIAL_* mask (globals + 0x820) in (`0x80057748`); kept on a scene change (tested); save block `PLYR`
  read in `0x800CF2B8`.
- God mode: sd2.ini `TakeDamage` lives at `0x802459B4` (globals + 0x824) and is read on every hit (`0x8005A430`: health
  +0x2E8 -= damage only when nonzero); held at 0. Tint: player instances `[0x802453B4]` (+0 next, +0x10 RpAtomic), geometry
  `[atomic+0x18]`: flags +8 `|= 0x40` (modulate material colour), materials `[geom+0x20]` x `[geom+0x24]`, colour RGBA at
  `mat+4` (`0x78FF78FF` = green). Scooby has 12 atomics, one material each, all `FFFFFFFF`. Human-checked: no health
  lost, but the hit reaction (knockback animation + freeze) still plays; it isn't gated on TakeDamage.
- Monsters = base type 0x02 (NPC) in the scene list `[scene+0x78]` x `[scene+0x74]` (gang NPCs like SHAGGYRUN too).
  Entity: +0x10 asset (first word = ID), +0x2C model instances (+0x20 matrix), +0x30 update (null = the scene loop skips it),
  +0x40 frame, +0x4C bound centre. Off = update nulled + parked at (2000+50i, -500, 2000) every frame; on = restored. Names:
  the scene HIP's ADBG records (`files/<L>0/<id>.HIP`, type `NPC `). Human-checked: parked monsters can't hit or block.
- The game's own warp gates: table 0x801C68B0 (26 x {index, unlocked, scene}); unlock-all `0x8004C690` (only called by a
  blanked-out cheat). Pause map rooms (`E 05 SELECTED`) only pick the stats shown, they don't warp.

## postool (`tools/debug/patches/postool.patch`, `hle/vi.cpp`)

Every 20 main-loop frames it runs the lines in `Play/postool.txt`, then deletes the file; output in `Play/postool/`.

| Command | Output |
| --- | --- |
| `dump NAME` | MEM1 big-endian → `NAME.bin` (for `findpos.py`, `parsys.py`) |
| `peek ADDR [N]` | N words → `peek.txt` |
| `poke ADDR HEX...` | write words (floats as hex) |
| `trace ADDR N` | N frames of the list whose head is `[ADDR]`: length + first 4 particles → `trace.txt` |
| `births SECONDS` | particle births + birth speed per owner emitter asset → `births.txt` (`p1`/`p2` tags only with probe lines in the overrides) |

## Measuring flicker (`tools/debug/capture/`)

- `record.ps1 -Out x.mp4 [-Seconds 6] [-Method ddagrab|gdigrab] [-Toggle]`: the game's client rect (window capture of the
  swapchain is black). ddagrab (default) makes the window topmost and works on any monitor; gdigrab = the lossless grab
  the PR #14 flicker counts used.
  `-Toggle` holds `Play/fogstat.txt` for the duration (for `fogstat-probes.patch`).
- `flashcount.py x.mp4`: drops duplicate frames, counts cells in the lower half that jump >3 for exactly one frame.
- `triplet.py x.mp4 K out.png [box]`: frames K-1/K/K+1 + amplified difference, to see what flashed.

## Game facts

- **Particles**: one pool, 2000 × 0x50 at `0x80210858..0x80237958`. Particle: +0 next, +4 prev, +8 life left, +0xC colour,
  +0x10 pos, +0x1C size, +0x20 vel, +0x3C owner (emitter asset in RAM; first word = asset ID).
  - Systems: pointer array at `0x80237958`; system +0 list head, +8 count, +0x11 priority, +0x12 flags.
  - Allocator `fn_80032728`: free list (`fn_800325C8`); when empty it **steals the head particle of a lower-priority system**.
    Pool exhaustion shows up as the lowest-priority effect (i004 floor fog, priority 0) losing particles.
  - Emit `fn_800C4B04(emitter r3, dt f1)`: timer at +0x1C; when ≤ 0 emits `asset+0xA` ± `asset+0xB` and resets to `asset+0xC`.
    Interval 0 = emits on **every call** (256 of 433 disc emitters; mostly one-shots).
  - Callers: `fn_800C4AAC` per-frame update (pause check `fn_800AD3B4`, enabled bit +0x18; called from `fn_800CE760`) —
    fixed by PR #14. `fn_800C5C0C` "emit with overrides", 89 entity-code callers — `fix/particles-fps60-path2` (`fog-pr14.md`).
  - Emit sets particle velocity = asset velocity x dt; MOVE adds it every frame (no dt). So dt passed to emit must be the
    real frame time: a summed or fixed 1/30 dt doubles particle speed at 60 fps. `births` prints `speed` to check this.
- **Particles at fps60** (`no100f_particles_update.cpp`): the target is the game at 30 fps including its rounding.
  With fps60 off the native code must match the game's code byte for byte on live data.
  Each PARS command's run rate and time come from one table, `CommandTimeFps60` (`no100f_particles_update.h`).
  Fast-math would erase the rounding in `PPC_Fmadds`, so a port that needs it goes in `RECOMP_PPC_SEMANTIC_RUNTIME_SOURCES` (`wiicompiled/runtime/CMakeLists.txt`).
  Command 6, 20 and 27 timing has no numbers from the game, only the runtime's maths.
- **Fireworks**: the holiday code is `fn_800AED5C`, burst emission `fn_800ADAC0`; launch sites are a fixed table of 5 plus drift, not the player's position.
  `[game] holiday = "confetti"` gives each new FIREWORKS1/2 particle the next of 8 colours; they go pale during the game's bright sky flash.
  Sparks over the mansion are correct geometry (same on `01-01`).
- **HIP assets** (`audio/hip.py` lists, `game/mkhip.py` has an `assets()` walker): xEntAsset pos at +0x1C; PLYR yaw at +0x10.
  PARE +9 type, +0xA count, +0xB variance, +0xC interval, +0x30 PARS ID. PARS +0xC parent, +0x16 maxPar.
- Scooby's model matrix (xMat4x3: right, up, at, pos) at `[0x802453C8]`; yaw = `atan2(at.x, at.z)`.
- `sd2.ini` in the boot mod sets `TakeDamage = 0` (`-AllPowerups` also unlocks gadgets).

## Traps

- `powershell -File script.ps1 -Levels a,b` passes ONE string `"a,b"`; a bad `BOOT=` crashes the game ("Missing indirect jump
  target"). `births-run.ps1` splits and validates; do the same in new scripts and abort on a `crash_*` file in the log dir.
- Wait for a level by polling the newest `Play/UserData/Logs/*/console.log` for `<level>.HIP`, not with fixed sleeps.
- At 60 fps the recorder sees each game frame ~twice; always compare unique frames.

Case history: `fog-pr14.md`.
