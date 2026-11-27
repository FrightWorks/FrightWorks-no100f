---
name: hle-divergence-debug
description: >-
  Root-cause rendering, geometry or state bugs in the Scooby-Doo recomp by checking guest data integrity
  and comparing native HLE overrides (PPC_NATIVE_OVERRIDE) against the original game code in main.dol.
  Use when geometry is stretched/spiked, textures or state go wrong, a display list or buffer looks
  corrupted, behaviour differs from Dolphin, or before starting a long pixel-ownership bisect.
---

# HLE divergence debugging

The recomp runs the game's own translated code except where the runtime replaces a function natively
(`PPC_NATIVE_OVERRIDE` / `PPC_NATIVE_OVERRIDE_VOID`, list in `wiicompiled/projects/no100f/hook_remap.txt`
and `grep PPC_NATIVE_OVERRIDE wiicompiled/runtime/src`). An override that skips a side effect of the
original (a state flush, padding, a saved/restored struct) corrupts data far away from the override.
Issues #6 (curtains) and #2 (solid map textures) were both one missing `__GXSetDirtyState` call in
`No100f_GXBeginDisplayList` (PR #8). Days of pixel-ownership bisects never found it; this method did in about an hour.

## Order of work

1. **Data before pixels.** Before skipping/flat-painting draws, check whether the guest data the draw uses
   is what the game built. Snapshot at build time, compare at use time.
2. **Find the guest code** that builds/uses the data (callers, sizes, layout) with `tools/debug/dol.py`.
3. **Diff the override against the original.** Disassemble the original function at the override address
   and list every side effect. Anything the native version drops is a suspect.
4. **One fix, measured by the same check** (step 1 shows 0 divergences), then human visual confirm.

Use Dolphin only as an oracle for "what OG does" (step 5 below), not as the first tool.

## Step 1 — build-vs-use integrity check (display lists)

Patch: `tools/debug/patches/dltail-diagnostic.patch` (env `NO100F_DLTAIL`). Snapshots every list at
`GXEndDisplayList`, compares at `GXCallDisplayList`, logs the first differing byte.

```powershell
git apply tools\debug\patches\dltail-diagnostic.patch
powershell -ExecutionPolicy Bypass -File tools\build-no100f.ps1 -SkipTools -SkipTranslate
# RT_LOGF goes to stderr; manual-session does not capture it, so launch directly after manual-session prepare (writes
# Config/boot; manual-session stop afterwards puts the user's config back):
$env:NO100F_DLTAIL = '1'
Start-Process 'Play\FrightWorks-No100F-Game.exe' -WorkingDirectory Play -RedirectStandardError autotest\manual\dltail.log -RedirectStandardOutput autotest\manual\dltail.out.log
Select-String autotest\manual\dltail.log -Pattern '\[dltail\]' -Context 0,2
git apply -R tools\debug\patches\dltail-diagnostic.patch   # remove before any PR
```

Read the result: `first diff +N` where N is the same budget boundary across many lists, with float-looking
bytes (`3e..`, `3f..`, `40..`, `bf..`, `c0..`) = something else's buffer overlaps the list. 0 diffs = lists are clean.
The same pattern (snapshot at producer, compare at consumer) works for any guest buffer; copy the helper.

### Step 1b — which draw is it, and what did aurora decode? (`gx-draw-probes.patch`)

For "object invisible / wrong in some places" with clean list data. Made against PR #8's `622b97a` (`git apply -3` elsewhere).
**It pads `GXEndDisplayList` to 32 bytes by default**; set `NO100F_DL_NOPAD=1` to keep the shipping behaviour.

| Probe | How | Use |
| --- | --- | --- |
| IDVIEW | create `Play/ghost-idview.txt` | every draw painted in a colour hashed from its display list address (find a mesh's lists) |
| Paint | `Play/ghost-paint.txt`, lines `ADDR RRGGBB` (`0` = raw FIFO draws) | paint only those lists: proves a list is the object you think |
| `[livevtx]` | always on | layout aurora decoded a CP-write-free list with vs the HLE layout it should have got (VCD + VAT) |
| `[shadowvcd]` | always on | aurora's API VCD shadow vs its decoder at each `__GXSetVCD` |
| `[statecheck]` | `NO100F_STATECHECK` | game's own `__GXData` vs aurora's decoded state at draw time |
| A/B switches | `NO100F_DL_LEGACY`(+`_RANGE`), `NO100F_DL_STRIP=cp\|xf\|all`, `NO100F_NO_CP_CACHE`, `NO100F_FORCE_FLATTEN`, `NO100F_NO_WG_DL`, `NO100F_NO_WG_AURORA`, `NO100F_LIST_START`, `NO100F_ZFRZ` | rule out caches / list prefix / flatten paths |

The `[ghost]`/`[frame]` dumps key on the i004 ghost model (first dirty list of 1504 bytes); retarget before reusing.

## Step 2 — guest code with dol.py

```powershell
python tools\debug\dol.py callers 801022C8     # who calls GXBeginDisplayList
python tools\debug\dol.py func 8015E944        # disassemble the caller (bl targets named from MAP.txt)
python tools\debug\dol.py name 8015EA94        # address -> MAP name+offset
```

Needs `pip install capstone`. MAP names are partial (`fn_XXXXXXXX` for most of the game). Useful anchors:
`__GXData` pointer at `r13-0x7DB8`; `dirtyState` at `+0x4F4`; gather pipe stores go to `0xCC008000` (`lis r3,-0x33ff` / `-0x8000(r3)`).

## Step 3 — override vs original checklist

Start with `tools/debug/overrides.py` (active overrides only; `#if 0` and parked 0x80001xxx ones excluded):

```powershell
python tools\debug\overrides.py hle\gx                 # address, MAP name, native function, file:line
python tools\debug\overrides.py --effects SetPixelFmt  # original's calls, __GXData writes/reads, FIFO stores
python tools\debug\overrides.py --xref hle\gx          # fields the original writes that translated code reads
```

Native overrides never write the game's real `__GXData` (their `kGXDataPtrAddr` is a scratch shadow), so
every `__GXData` store in the original is skipped. `--xref` lists where a translated function reads such a
field. Static and linear: confirm with `dol.py func` before acting. The full GX audit (2026-10-22) and GPU comparison traps are in `gx-audit.md`.

For the override's address, disassemble the original and check each item against the native body:

- Dirty-state flushes (`__GXSetDirtyState` 0x800FE204) before/after FIFO redirection
- Return values: rounding/padding (hardware DL sizes are 32-byte multiples), overflow flags
- Saved/restored state (e.g. `GXBeginDisplayList` copies `__GXData` when `+0x4F1` is set)
- Flags other code reads (`+0x4F0` in-display-list)
- Cache ops are no-ops in the recomp (`DCInvalidateRange`, `dcbi`); `dcbz` zeroes the aligned 32-byte block

Also check Aurora's BP write dedup (`bpRegCache`, `command_processor.cpp`): Aurora API calls made by
natives do not update it, so a register written by both a native setter and translated FIFO code can
have a later FIFO write wrongly dropped. Translated writers of a shadow field: `overrides.gx_field_users()`.
The reverse also bites: Aurora's own API shadows (e.g. `__gx->vcdLo/Hi`) are not updated by FIFO-decoded CP writes,
so a later dirty flush can re-send stale API state over what the game's lists set (PR #8 ghost regression, `cases.md`).

Known remaining divergences (not needed so far): native `GXEndDisplayList` (0x80102390) skips the end
dirty flush and the save-context restore, and returns the exact byte count instead of the padded size.

### Calling original guest code from an override

An override's address is excluded from translation, but its callees are not. Call them by symbol:

```cpp
extern "C" void func_800FE204(CpuContext* ctx);  // the game's __GXSetDirtyState
if (CpuContext* ctx = TryGetCpuContext()) func_800FE204(ctx);
```

Read guest globals through `ctx->gpr[13]` (SDA) with `Memory::TryRead32`. Pattern in tree: `hle/no100f_camera.cpp`, `hle/gx/gx_fifo.cpp`.

## Step 4 — fix and verify

- Fix the override to match the original; no heuristics in Aurora to hide the symptom.
- Re-run step 1: divergences must be 0.
- Build without the diagnostic. Human visual check when the bug is visible (manual-session; see `manual-yn-bisect` skill for
  locked landings), and the smoke test in `.agents/REPO.md` (Testing: `python tools/smoke-test.py --scenes <changed level>`).

## Step 5 — Dolphin oracle (when you need OG ground truth)

Dolphin FIFO log (`.dff`) of the OG scene, e.g. `autotest/manual/curtains-i001.dff`. Decoder and helpers in `tools/debug/dff/`:

| Script | Use |
| --- | --- |
| `dff_decode.py <in.dff> <out.pkl> [frames]` | Replays CP/XF/BP + memory updates; pickles every indexed draw (prim, n, VCD/VAT, POS array, matrix, indices, verts) |
| `render_dff.py <pkl> <frame> <out.png> [hl ids] [only ids]` | Painter's render of a frame; highlight draws |
| `world_views.py <pkl> <out.png> <ids> [title]` | Orthographic X-Y / Z-Y wireframes of chosen draws |
| `list_pool.py <pkl> [frame] [lo] [hi]` | Draws whose POS base is in an address range |
| `list_dyn.py <pkl> <dff> [frame]` | Draws using arrays rewritten every frame (CPU-skinned meshes) |

DFF format notes: header register blocks are little-endian u32 (bp @448, cp @1472, xf @2496, xfregs @18880);
64-byte frame records; 24-byte memory updates `<IIQIB` (fifo_pos, addr, data_off, size, type; 4=vertex, 1=texture, 8=TMEM).
PNMTXIDX byte is the XF row directly (matrix address = row*4).

Trap: recomp "array size" in owner dumps is inferred from max index (`(maxIdx+1)*stride`), not game data. A corrupted index makes the array look bigger.

Case histories (PR #8 display lists, PR #10 MSAA centroid) and parked follow-ups: `cases.md`.
