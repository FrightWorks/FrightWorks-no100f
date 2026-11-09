# Debug kit

Tools and agent skills for finding bugs in the FrightWorks static recomp. New here? Start with the walkthrough in
[`docs/testing.md`](../../docs/testing.md); this page is the reference. Only things that helped in a real
investigation (upstream PRs #8, #10–#15, #22, issue #24) or will help the next one. The build tools (`tools\build-no100f.ps1`,
`shot-pid.ps1`, ...) live one folder up; nothing here contains game data.

Until 2026-10-31 this kit was a separate repo (Scooby-Doo-Refrightened-debug) linked in with junctions. Its history was
merged in with the paths rewritten, so `git log -- tools/debug .agents/skills .claude/skills` shows it.

Layout (paths in the skills and scripts are written from the repo root):

| Path | What |
| --- | --- |
| `.agents\skills\` | the agent skills (tracked), with `.agents\REPO.md`, the shared rules. Claude Code looks in `.claude\skills\`: `tools\setup-agents.py` (run by `worktree.ps1 new` and `open`) links that to `.agents\skills\` (local, git-ignored) |
| `tools\debug\` | these tools |
| `tools\debug\local\` | git-ignored, machine-local: `user-setup\` (the user's Config.toml + controller files while a test runs; `manual-session.ps1 restore` puts them back), Dolphin disc copies, a downloaded ffmpeg, `kenney-input-prompts\` (the whole Kenney Input Prompts pack, CC0, from kenney.nl: pick new button art there and copy only the SVGs used into `tools\button-art\<family>\`) |
| `autotest\` | git-ignored work data and output: shots (`autotest\manual\`), texture-hunt data (`autotest\texhunt\`), audio dumps, logs |

## Skills

| Skill | Use for |
| --- | --- |
| `hle-divergence-debug` | Geometry/state/GPU bugs: guest data integrity, native override vs original, Dolphin DFF oracle. Cases: PR #8 (+ ghost regression), #10, #12/#13 |
| `game-logic-debug` | Frame-rate (fps60) and gameplay bugs: fixed spawn spots, F8 dev menu (any scene, saved spots, god mode, monsters off, power-ups on/off, free camera, Textures tab), live memory (postool), particle pool/emitter counts, flicker measurement. Case: PR #14 |
| `manual-yn-bisect` | Locked landing + in-game Y/N toast for human visual checks; fix-only PR workflow (`strip-before-pr.md`) |
| `audio-debug` | Crackle/clicks/fuzz/stuck loops: capture, per-stage dumps, disc reference decode. Case: PR #11 |
| `parallel-work` | Several tasks at once: worktrees and their Claude sessions, started, checked on, messaged and removed by the main checkout's session (`worktree.ps1`) |

## Tools

| Path | What |
| --- | --- |
| `play-patched.ps1` | Apply patches, build, play with the dev menu on (`NO100F_DEV_TOOLS=1`; the default `devmenu` patch only on branches from before it was built in); patches come out of the source when the game closes. Sound on at 5% and rumble off for the run (the user's volume, mute and rumble lines go back). `-Patch a,b`, `-HdTextures` (with the HD pack), `-Keep`, `-CleanBuild`, `-Remove` |
| `manual-session.ps1` | Single-window playtest: `start -Boot <level> [-AllPowerups] [-Sound]` (saves the user's settings, writes the test Config + `zz-manual` boot mod; sound muted, `-Sound` = on at 5% for human tests and audio work; rumble always off (`[controller] rumble = false`, no switch)), `shot`, `stop` (settings back), `prepare` (like start, no game), `restore`, `stop-tests`. Stays inside its own checkout; `prepare`/`start` wait for an exclusive game-run slot (held until `stop`/`restore`, 4 hours at most) |
| `worktree.ps1` | Parallel work (the `parallel-work` skill). A thin wrapper over `worktree.py` (Python 3.8+, standard library only; it skips the Microsoft Store stub; on Linux run `python3 tools/debug/worktree.py new --name ... --branch ...`: symlinks instead of junctions, and without `wt` the command to run is printed): `new -Name <n> -Branch <b> [-From origin/main] [-Task "goal"] [-Build] [-NoOpen] [-Model m] [-Effort e]` (a git worktree in `..\FrightWorks-worktrees\<n>`, or in the folder set once with `git config frightworks.worktreesDir ~/worktrees/frightworks` (`-Base` overrides it), with its own Play\, links to the main checkout's Disc\, texture_pack\, toolchain\, Dependencies and `local\` folders, and a Windows Terminal tab running `claude -n <n>`; `-Task` writes the goal into its TASK.md and the session starts on it), `list`, `open -Name <n> [-Continue] [-Model m] [-Effort e]` (`-Model` = `claude --model`, e.g. `sonnet`, `opus` or a full model ID; `-Effort` = `claude --effort`, `low`..`max`; added only when given, for that session only, never remembered: a later `open` without them uses the defaults; a new session that picks the task up from TASK.md, or `claude -c`; `-Name main` = the main checkout's coordinating session, named `coordinator`; `new`/`open` drop the Claude Code variables (`CLAUDECODE`, `CLAUDE_PID`, `AI_AGENT`, `CLAUDE_CODE_*`) of the calling shell, so the tab's session is its own, not a child of the coordinator: ListAgents sees it and `-Continue` can resume it), `remove -Name <n>` (stops leftover dotnet build servers; refuses with uncommitted work or a running program; unlinks before deleting; keeps the branch) |
| `../smoke-test.py`, `../testkit/` | The smoke test (`python tools/smoke-test.py`, 3 jobs at once, `docs/testing.md` section 4a) and the Python test kit it is built from: `launch.py` (the launch helper: own `Play` copy, rumble off, muted, controller isolation, build-ID check), `slots.py` (shared run slots; also the entry point of the PowerShell scripts: `hold` / `release`), `devmenu.py` + `waitplay.py` (the one wait-until-playable routine), `checks.py`. Tests: `python -m unittest discover -s tools/testkit -p "test_*.py"` |
| `game-run.ps1` | Dot-sourced by the scripts: `Get-/Stop-CheckoutGames <root>` (only games started from that checkout) and `Enter-/Exit-GameSlot <root> <why> [-Shared] [-Persist]`, which take and give back a run slot of the test kit (`../testkit/slots.py hold`): exclusive (the whole machine) by default, shared for runs that need neither the screen nor exact timing; a script that dies gives its slot back; builds run in parallel, the user's own game is never waited for or closed. Who takes what: `docs/testing.md` section 4 |
| `config-lines.ps1` | Dot-sourced by the scripts that start the game on the player's own `Config.toml`: `Set-ConfigLine <file> <section> <key> <line>`, `Set-RumbleOffForRun`, `Restore-ConfigBytes` (the config back as found) |
| `build-translator.ps1` | Rebuild the translator after switching to/from a branch that changes `wiicompiled\translator` (then build without `-SkipTranslate`) |
| `dol.py` | `callers` / `func` / `dis` / `name` on `game/main.dol` with MAP names (needs `capstone`) |
| `overrides.py` | Active native overrides; `--effects`, `--xref` (`__GXData` fields only the replaced original maintained) |
| `dff/` | Dolphin `.dff` FIFO log decoder, renders, pool/dynamic-array listers |
| `audio/` | `crackle.py` click detector, `adpcm_ref.py` disc reference decode + bit-exact compare, `hip.py` HIP asset lister, `loopcheck.py` (is the output stuck repeating a short loop? `--scan W` over a long dump), `loop-capture.ps1` (boot a scene with the audio dump on and sound at 5%, then `loopcheck.py`) |
| `game/` | `levels.md` (scene IDs ↔ level names, which scenes to test what in) + `levels.py` (reads the table from `main.dol`), `mkhip.py` (spawn spot / move NPCs via the boot mod), `spots.txt`, `findpos.py`, `addrrefs.py`, `parsys.py`, `births.py` + `births-run.ps1`, `fogdump.ps1` + `fogsum.py` (i004 fog bank state from a running dev menu build), `fanfog-walk.py` (walk one particle list from a pool peek), `hipedit.py` (rebuild a HIP with added assets; a no-op rebuild is byte identical), `rwdump.py` (RenderWare section tree of a file or a HIP asset) |
| `texhunt/` | HD texture work, see below |
| `capture/` | `get-ffmpeg.ps1` + `ffbin.py` (find or fetch ffmpeg), `record.ps1` (video of the game window: ddagrab + topmost by default, `-Method gdigrab`), `flashcount.py` (one-frame flash counter), `triplet.py` |
| `yn-bisect/` | `curtain-repro.ps1` scripted landing template, `curtain-ask.ps1` Y/N toast helper (need `yn-bisect-harness.patch`) |
| `pr-media.py` | Upload PR and issue screenshots to FrightWorks/pr-media (needs write access there): `--pr N` or `--issue N`, then `.png`/`.jpg` files; checks signature, 10 MB, name clashes and PNG text chunks / JPEG EXIF (refuses, never strips), pushes a plain commit from a temporary shallow clone, prints one Markdown image line per file. `--dry-run` checks only; `--repo URL` for tests; `--self-test` runs the checks against a local bare repo (Python 3.8+, standard library + git) |
| `holiday-shots.ps1` | Screenshot loop for `[game] holiday` dates per hub (needs PR #13) |
| `patches/` | Diagnostics kept out of PRs; table in `.agents\skills\manual-yn-bisect\strip-before-pr.md` |
| `patches/pad-script.patch` | Scripted pad 1: hold the stick and press buttons at set times, dev tools on or off (`NO100F_PAD_SCRIPT=<file>`). Format: top of `no100f_pad_script.h`, which the patch adds; usage in the `game-logic-debug` skill. Routes: `game/pad-walk-check.txt`, `audio/issue14-route.txt` |

### F8 progression diagnostics

Enable Developer tools in the launcher (or `NO100F_DEV_TOOLS=1`). These controls require the verified GIHE78 rev-1
executable layout; unavailable/modified state is reported, not treated as locked.

| Tab / command in `Play\devmenu.txt` | Use and limits |
| --- | --- |
| Player / `player`, `health 1`, `health 5`, `snacks 500` | Read health/balance or apply one-time edits. Health is 1..5, with no kill/resurrection or simulated hit. Spendable balance is 0..99999 (a diagnostic input limit, **not** the game's maximum); it does not collect snacks or change gallery progression. New Game normally restores health to 3; later gameplay can replace edits. |
| Player > Collected-snack progression / `snackareas`, `snackarea 1 118`, then `snackconfirm` or `snackcancel` | Advanced, separately confirmed **area-counter** edit. Select one named area 1..29; count must be 0..its live target. Shows current/target and an explicitly derived all-area sum. Can affect natural picture-gallery access and persist in a later ordinary save. Balance, other areas, physical pickup flags and scene-local counts are unchanged: this does **not** collect/remove/respawn snacks. No all-areas operation. |
| Warps / `warps`, `warp E004 on`, `warp E004 off` | Read all 26 discovery entries or change one. These are **one-time live discovery edits**, not continuously disabled physical gates. Approaching a gate can unlock it again; normal saves can retain changes. Map, gadgets and room discovery are separate. |
| Gallery / `gallery`, `gallery pictures locked`, `gallery movies unlocked`, `gallery movies natural` | In H003 only, test each picture/movie section with Game rules / Force locked / Force unlocked. Does not grant snacks, watched cinematics or story flags. Overrides clear when leaving the gallery/loading/returning to title. Modified or missing condition/link schemas disable the affected section. |
| Diagnostics / `receipt`, `readiness` | Read-only local progression receipt (build, position, resolved data/save paths, player stats, Map/gadgets, warps, natural gallery state and active overrides) or interaction blockers (including player-edit death/payment gates). Paths remain in the local log; useful for checking disposable-profile isolation. No import/restore. |
| Resources | Samples this game's process CPU and RAM approximately once per second while visible. CPU shows machine-normalized and one-core-equivalent percentages after warmup. RAM distinguishes resident working set from private committed memory. Process GPU utilization is explicitly unavailable: Aurora/WebGPU exposes no reliable process counter. Unsupported OS measurements have reasons, not zero placeholders. Nothing is uploaded. |

Edits require stable gameplay, player control and no cinematic/movie or portal/queued travel/spot transfer.
New diagnostic-only command files are consumed with explicit rejection reasons when unsafe, rather than replaying
edits after a transition; older command-file waiting behavior is unchanged. Results go to `Play\devmenu.log`.
Long command output scrolls within the F8 status area so diagnostic receipts cannot push the tabs off-screen.
Commands reject extra arguments and unknown identities; there is no arbitrary memory/event editor.
Player-stat and collected-counter edits additionally reject dead/dying players and pending snack-gate payments.
Progression review/confirmation is shared by the UI and command surface; scene/availability changes, another
selected area, or changed count/target invalidate it. `snackarea` only stages a change; `snackconfirm` applies it
once and requires unchanged state. Neither path invokes the ordinary snack-grant helper or its completion sound.
The former Power-ups tab is now Player; Map-first power-up identity/order and existing commands remain unchanged.

Gallery detail rows show game-rule availability separately from effective test access. Visibility refreshes only
through normal gallery entry/browsing; close/reopen the gallery after changes. Changing a rule does not dispatch
UI events or reveal gallery widgets over unrelated screens.
Clear gallery overrides restores game rules, not old progression. Normal movie playback can still update watched
state. None of these tools invokes save/load or edits saves, but normal gameplay/save actions retain their usual
side effects. Use an isolated disposable profile for diagnostics; never test against someone else's saves.

### `texhunt/` (HD texture packs, cut-outs, halos)

Data and output go to `autotest\texhunt\` (`suspects.csv`, `devmenu_textures.txt`, `disc.pkl`, packs, shots); the
session scripts need the dev menu build in `Play\` (`build-devmenu.ps1` or `play-patched.ps1`) and restore the user's
setup when the game closes.

| Tool | What |
| --- | --- |
| `look.ps1 -Boot e005 -Spot <spot> [-Hd \| -Pack <dir>] [-Extra <dir>] [-Interp N] [-KeepIntro] [-Sound]` | Open the game at a saved dev-menu spot (intro cutscene skipped) and leave it to the human |
| `hunt.ps1 [-Boot h001] [-Pack <dir>] [-Branch b] [-TexLog]` | Texture-hunt session: dev menu + HD pack, every powerup, no hub intro, sound at 5% (`play-patched.ps1`), audio dump checked for a stuck loop |
| `spots-run.ps1 -Boot h001 -Ids a,b -Mode orig\|hd\|clean [-Tag x] [-Branch b]` | Screenshot each saved spot |
| `cutout-shots.ps1`, `perf-cutout.ps1` | Shots / draw-call + frame-time numbers per cut-out mode (needs aurora's cut-out modes) |
| `build-devmenu.ps1 -Branch b` | `Play\` = that branch with the dev menu (the patch on older branches), source put back afterwards |
| `gxtex.py` | XXH64 + RWTX reader + GX decoders; names textures like the port (`tex1_WxH_<hash>_<fmt>`) |
| `where.py [IDs]` | Which assets use a suspect's texture; BSP geometry gets `tp X Y Z` positions |
| `tint-pack.py --out <dir> (--from <dir> \| --hip <HIP>) [--match s]` | Tinted copies of textures; link them with `look.ps1 -Extra` to see where a texture is drawn |
| `creeper-frames.py` | The HD Creeper window frames (issue #24) from the disc originals + ScoobyHD's window (needs SciPy) |
| `skipintro.py <mod dir>` | h001.HIP without the intro cutscene (written into a boot mod, never committed) |
| `ogexport.py`, `clean.py`, `onoff.py`, `verdicts.py` | Original-vs-HD sheets (local only), cleaned and on/off test packs, dev menu marks ↔ `suspects.csv` |

Patches are against upstream `main` `e600261` unless the table says otherwise. `gx-draw-probes.patch` was rebased onto
PR #8 `622b97a` and not rebuilt since.

Requirements: Python 3 with numpy + Pillow (+ `capstone` for `dol.py`/`overrides.py`, SciPy for `creeper-frames.py`). The
`capture\` tools use ffmpeg from PATH; without one, `capture\get-ffmpeg.ps1` downloads the BtbN win64 GPL build once into
`tools\debug\local\ffmpeg` (SHA-256 checked).
