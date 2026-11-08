---
name: manual-yn-bisect
description: >-
  Locked level landing plus in-game Y/N toast bisects for visual or GPU bugs
  (manual-session, repro scripts, curtain-ask style waits). Use when attributing
  pixels, skipping draws, flat-painting draws, A/B env flags, or any bug that
  needs a stable camera and human yes/no without chat back-and-forth.
---

# Manual Y/N bisect (locked landing)

Project-local method for Scooby recomp visual/GPU bugs. Not tied to one issue —
reuse for textures, curtains, future glitches. Diagnosis harness is temporary;
strip before PR (see [strip-before-pr.md](strip-before-pr.md)).

Worked examples: [examples.md](examples.md) (issue #2 textures, i001 curtains).
Live bug status: root **`AGENTS.md`** (single source of truth).

**Run the `hle-divergence-debug` skill first** for stretched/spiked geometry or state that goes wrong.
Its build-vs-use data check found the cause of #6 and #2 in about an hour, after days of Y/N ownership bisects
that only narrowed down *which* draws looked wrong. Use this skill for human visual confirmation,
and for pixel ownership once the data is known to be clean.

**Harness is a patch now.** The toast, Y/N keys, `STICK_*` script input and skip/flat files are not in `main`:

```powershell
git apply tools\debug\patches\yn-bisect-harness.patch     # before building for a bisect session
git apply -R tools\debug\patches\yn-bisect-harness.patch  # before any PR
```

Without it, `NO100F_INPUT_SCRIPT` still works for buttons (`0:A:1` keeps pad 1 connected) but `STICK_*` entries do nothing,
so scripted landings drift.

## When to use

- Bug is **visual** and needs a human eye (wrong color, spike, missing mesh, solid texture).
- You need a **repeatable viewpoint** so draw/event IDs or pixels stay stable.
- You want the agent to **wait** on Y/N in-game instead of asking in chat.

## Core loop

1. **Land** — boot one level; script or walk to a fixed position/camera.
2. **Freeze** — do not move after landing (IDs and pixels are view-locked).
3. **Try one thing** — one skip, flat-paint, env flag, or code experiment.
4. **Ask** — toast with a clear yes/no question; wait for answer keys.
5. **Record** — take `y`/`n` (optional shot), clear experiment + toast, next bisect.

Never stack untested changes in one ask. Never leave a non-question toast that looks like it needs Y/N. Prefer labeled shots under `autotest/manual/`.

When scoring, label outcomes **separately** if multiple phenomena exist (e.g. fabric vs purple spikes vs teal vs other scene damage). “Mesh gone” ≠ proof every artifact came from those draws.

## Hard rules

1. Manual-session only for this class of bug. No `level-sweep.py` / `_verify_*.py` / multi-window automation unless the human asks.
2. Pick the **level where the bug actually shows** (document it; wrong hub/room wastes the session).
3. Keyboard focus on the **game** window for Y/N.
4. Clear idle toasts: empty/delete `Play/curtain-test-toast.txt`.
5. Clear leftover diag envs before a clean boot: `Remove-Item Env:NO100F_*` (preserve any env a repro script must keep, e.g. flat-size lists — see curtain-repro).
6. Re-dump owners after any camera move or new boot — host event IDs drift.

---

## Step 1 — Locked landing

### Minimal (any bug)

```powershell
Remove-Item Env:NO100F_* -ErrorAction SilentlyContinue
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1 -SkipTools
powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 start -Boot <LEVEL> -AllPowerups
# Human or script walks to the glitch; then freeze.
```

### Scripted landing (preferred when stable)

Add a small `tools/debug/yn-bisect/<bug>-repro.ps1` that:

| Piece | What to set |
| --- | --- |
| Level | `-Boot` / `BOOT=` for the glitch room (scene IDs and names: `tools/debug/game/levels.md`) |
| Settle | Delay until world is ready |
| Move | `NO100F_INPUT_SCRIPT` with `STICK_*` (keep pad “connected” so unplug pause does not freeze) |
| Optional dump | Env for owner/trace logs if needed |
| Toast | Informational “landed — do not move” only (not a Y/N question) |

Template: `tools/debug/yn-bisect/curtain-repro.ps1` (i001 LEFT then BACK). Copy and change level/stick timings for a new bug.

After landing, clear or replace any info toast before the first real question.

Live stick nudge file (if enabled): `Play/curtain-stick.txt` with `LEFT`/`RIGHT`/`UP`/`DOWN` — avoid if IDs must stay fixed.

---

## Step 2 — Toast Y/N

### Answer keys (in game today)

While `Play/curtain-test-toast.txt` is non-empty, the overlay shows the toast and:

| Key | Meaning | File |
| --- | --- | --- |
| **Y** | yes | `Play/curtain-answer.txt` → `y` |
| **N** | no | `Play/curtain-answer.txt` → `n` |

Code: `wiicompiled/runtime/src/settings_overlay.cpp` (SDL `Y` / `N`). Overlay prints `[Y] = yes  [N] = no`.

Filenames say `curtain-*` for historical reasons; **use them for any bug** until renamed.

### Agent helper

```powershell
. .\tools\debug\yn-bisect\curtain-ask.ps1   # name is historical; works for any toast bisect

$a = Wait-CurtainAnswer `
  -Toast 'After this change: is the glitch GONE? Y=yes N=no' `
  -TimeoutSec 180 `
  -ShotLabel bug-yn-01

Set-CurtainToast ''
```

`Wait-CurtainAnswer` **clears all** skip/flat control files before applying that ask’s levers (avoids stale Events under a Sig-only ask).

| Helper | File | Role |
| --- | --- | --- |
| `Set-CurtainToast` | `Play/curtain-test-toast.txt` | Question (or `''` to clear) |
| `Set-CurtainEvents` | `Play/curtain-skip-events.txt` | GPU event skip (`a-b`; WIP comma lists / multi-ranges — rebuild first) |
| `Set-CurtainSizes` | `Play/curtain-skip-sizes.txt` | POS size skip (often unreliable) |
| `Set-CurtainSig` | `Play/curtain-skip-sig.txt` | `prim:n` or `prim:n:size` (`0`=wildcard; often unreliable) |
| `Set-CurtainFlat` | `Play/curtain-flat-events.txt` | Flat-white paint by event — **prefer for ownership** |
| `Wait-CurtainAnswer` | polls answer | Returns `'y'` or `'n'` |

Toast wording: hypothesis + what Y means, e.g. `Skip 100-120: is the solid floor GONE? Y=yes N=no`.

One question → wait → clear skips/toast → next. Do not ask Y/N in chat when this path is live.

---

## Step 3 — What to try (pick per bug)

| Lever | How | When it helps |
| --- | --- | --- |
| Flat-paint event | `curtain-flat-events.txt` / `-Flat` | Pixel ownership without removing the draw (silhouette takes white) |
| GPU event skip | `curtain-skip-events.txt` / `-Events` | Kill switch; may need **both cull passes** for one visible mesh |
| POS size / prim:n skip | sizes / sig files | Only if fingerprint is unique (often wobbles / wrong population) |
| Env A/B | `NO100F_*` / feature flags | Known code paths |
| Code experiment | one focused patch | After ownership narrows the draw |
| Dolphin / FIFO compare | capture + parse | Same draw in OG vs recomp |
| Shots | `tools\debug\manual-session.ps1 shot -Label …` | Evidence pack |

**Ownership lessons (curtains, reusable):**

- One visible mesh can be **multiple host events** (e.g. cull=0 and cull=1 twins). Skipping one may leave the mesh; flat-painting one can still prove ownership.
- Wide event bands can clear the bug **and** nuke unrelated scene bits — narrow with flat-paint or multi-event lists.
- Verify skips actually fire (`[curtain.skippush]` / `[curtain.skipparse]`) before trusting a “still broken” Y/N.

**Phases (generic):**

0. Data integrity first (`hle-divergence-debug`): is the guest data the draw uses what the game built?
1. Own the pixels (flat-paint/skip/flag until the glitch clears or paints on Y).
2. Compare that draw to Dolphin (or known-good) at the first divergent stage (POS/idx → matrix → topology → TEV → projection). Beware host **merge**: one host event may sum several FIFO commands (`NO100F_NO_MERGE=1` for mapping only).
3. One experiment at that stage — do not pile heuristics.

**Divergent-stage ladder:**

| Stage | If divergent | One experiment |
| --- | --- | --- |
| A. POS / idx | mismatch | decode / endian / stride (never force global LE) |
| B. XF / pnMtx | mismatch | matrix load / staged 12 floats (slot 0 after compact ≠ identity bug) |
| C. Topology | strip vs list | restart / winding / expand |
| D. TEV / blend / tex | mismatch | sampler / blend |
| E. Projection / viewport | mismatch | widen / fog |

---

## New bug checklist

- [ ] Level ID where bug shows (and rooms that do **not**)
- [ ] Landing script or written walk steps
- [ ] Success question for Y (“GONE?” / “still broken?” — keep Y meaning consistent)
- [ ] Separate score columns if multiple visual phenomena
- [ ] Optional: owner dump env + output path
- [ ] Handoff note in `AGENTS.md`: what was tried, what Y/N proved, what not to revive
