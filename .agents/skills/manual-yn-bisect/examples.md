# Worked examples

## Issues #6 (manor curtains) and #2 (solid map textures) — one root cause, fixed by PR #8

| | |
| --- | --- |
| Symptoms | #6: `i001` curtains with a thin neck at the sill and a long tail to the floor. #2: `e001` floors/walls go solid teal/beige after a deterministic trigger (walk-up / camera turn). |
| Root cause | `No100f_GXBeginDisplayList` (runtime override of `0x801022C8`) skipped the SDK's `__GXSetDirtyState` flush, so the game's `GXBegin` recorded 21 bytes of vertex state into every display list. Lists overran the builder's budget into the per-frame vertex arrays (float bytes became indices → #6), and load-time GX state was replayed on every draw (→ #2). |
| Found by | `hle-divergence-debug`: build-vs-call snapshot of every list (`dltail-diagnostic.patch`) → divergence exactly at the builder's budget, float bytes → `dol.py` on the builder `0x8015E944` and original `GXBeginDisplayList`. |
| Fix | 10 lines in `wiicompiled/runtime/src/hle/gx/gx_fifo.cpp`: call the game's `func_800FE204` when `__GXData->dirtyState` is set, before recording. |
| Verified | Integrity check 0 divergences; human visual on `i001` + #2 trigger on `e001`; smoke `h001`. |
| Superseded | PR #5 (`numTexGens` repair at draw time) patched the #2 symptom; closed. |

### What the Y/N bisects got wrong (do not repeat)

- Weeks of skip/flat-paint ownership chased static props (n=595/611/625 strips in pools `0xB94FA0`/`0xBA19C0`)
  that only *looked* like curtain panels. The real curtains were 38-vertex CPU-skinned meshes, strips n=76/86.
- Recomp "POS array size" was inferred from the corrupted max index, so the curtain buffers looked oversized and
  matched nothing in Dolphin.
- Skips, clamps and degenerate-triangle drops hid pieces of the symptom without explaining it.
- Lesson: when geometry is stretched, check that the indices/vertices the draw reads are what the game built
  **before** attributing pixels.

### Still useful from that work

- Locked landing (`tools/debug/yn-bisect/curtain-repro.ps1`, needs `yn-bisect-harness.patch` for `STICK_*`) for a stable before/after view.
- Toast Y/N (`curtain-ask.ps1`) for human confirmations without chat round-trips.
- One visible mesh can be several host draws (cull twins); merged host draws can sum several FIFO commands (`NO100F_NO_MERGE=1` for mapping).
- Dolphin capture `autotest/manual/curtains-i001.dff` + `tools/debug/dff/` for OG ground truth.
