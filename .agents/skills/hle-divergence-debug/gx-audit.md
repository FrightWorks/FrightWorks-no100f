# GX override audit (2026-10-22, read-only; `tools/debug/overrides.py --xref hle\gx`)

184 active overrides total, 59 in `hle/gx` (`GXInit`, `__GXPEInit` and the parked 0x80001xxx ones are `#if 0` / inactive).
Natives write a scratch `__GXData` shadow (`kShadowGXDataAddr`), never the game's real one (`r13-0x7DB8`), so every
`__GXData` store in an original is skipped. Result: **no new likely bug**; nothing to fix without a symptom.

| Divergence | Why it is harmless today |
| --- | --- |
| BP setters (blend/z/dither/dst-alpha/pixel fmt/scissor/copies/fog/chan colors) skip their `__GXData` shadows (`cmode0` +0x1D0, `cmode1` +0x1D4, `zmode` +0x1D8, `peCtrl` +0x1DC, `suScis` +0xF8/+0xFC, copy regs +0x1E0..+0x200, amb/mat +0xA8..+0xB4) | Only overridden functions (and boot `GXInit`) read or emit them. Aurora's BP dedup cache is also safe: after `GXInit` no translated code writes those BP regs |
| `GXLoadTexObjPreLoaded` skips dirty bit 1 + per-map `tImage0/tMode0` shadows | Bit 1 only triggers `__GXSetSUTexRegs`, itself native (Aurora computes tex scale) |
| `GXSetPixelFmt` skips GEN_MODE multisample bit + dirty bit 4. Game does call `GXSetPixelFmt(2=RGB565_Z16)` in `fn_800DB114` (AA flag) | Aurora's GEN_MODE decode ignores bit 9 |
| BP setters don't clear `bpSentNot` (+0x2). Game calls `GXSetMisc(1, 8)` (`fn_800FDCC8`) at boot, so translated `GXBegin` sends `__GXSendFlushPrim` (8 dummy verts) after BP writes | Natives just cause fewer dummy degenerate strips than OG |
| `GXFlush` / `GXDrawDone` skip `__GXSetDirtyState` | State is still flushed by the next translated `GXBegin` / `GXCallDisplayList` before any draw |
| `GXEnableBreakPt` skips `cpEnable` (+0x8) | Native delivers the breakpoint callback at once (FIFO is consumed as written) |
| `GXEndDisplayList` skips end dirty flush, padded return, and **save-context restore** (`GXInit` sets `dlSaveContext` +0x4F1 = 1; game never clears it) | Only list builder is `0x8015E944`: between Begin/End it does `GXBegin` + indices only, and the fixed Begin pre-flushes. Revisit if another builder appears |

Ordering: gather-pipe bytes are decoded as written (`gx_fifo.cpp` parse loop), so native Aurora calls and FIFO commands stay in program order.
Not covered: non-GX overrides (OS/DVD/VI/pad) sharing other guest globals; `--xref` only tracks `__GXData`.

## GPU comparison traps

1. **Merge before host event numbering.** `handle_draw` can merge compatible primitives into one host draw (`NO100F_NO_MERGE=1` disables).
2. **DL tail trimming** (when `g_replayingDisplayList`) can clip `vtxCount`; log declared vs effective count before trusting a comparison.
3. **Matrix layout:** without per-vertex `PNMTXIDX`, Aurora compacts the current position matrix into uniform slot 0. `currentPnMtx=0` is not identity corruption.
4. **Inferred array sizes:** recomp POS "size" = `(maxIdx+1)*stride`, so corrupted indices fake big arrays.
5. **FIFO Player Analyze:** commands after a draw may configure the next object; EFB-copy dependencies can span frames.
6. AABB / index range agreement is not bytewise vertex, matrix or clip-space equivalence.
