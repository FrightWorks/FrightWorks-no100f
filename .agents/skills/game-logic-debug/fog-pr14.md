# i004 floor fog flicker at 60 fps — PR #14 `fix/fog-fps60` `1864646` (2026-10-24)

- Symptom: "Mind your Manors Part 1" (`i004`), floor fog puffs flash brighter for single frames, worst at the grate near the
  start. On `main` too; only with `[video] fps60` (6 s at spawn: 64/276 frames flash with fps60, 3/171 without); Dolphin smooth.
- Repro: `mkhip.py i004 --spot spot1` (grate view, monsters moved off-map), record 6 s, `flashcount.py`.
- How it was found:
  1. `fogstat-probes.patch` (per-frame blended draw stats): the fog batch kept the same triangle count, but its screen coverage
     spiked for single frames, always on the first two quads in the list.
  2. postool `trace` of the fog system's list: those two particles were replaced every frame; their memory then held torch-flame
     particles.
  3. `parsys.py` on dumps: pool 2000/2000 at 60 fps (torch system 1442 particles) vs ~1560 at 30 fps (torches 954).
- Cause: torches/candles use emitters with interval 0 → emit every call of the per-frame update `0x800C4AAC`. At 60 fps that is
  twice the particles; the pool fills; the allocator steals the newest particles of the lowest-priority system (the fog,
  priority 0), which respawn at random spots → a puff near the camera flashes for one frame.
- Fix: native `0x800C4AAC` (`hle/no100f_particles.cpp`, 41 lines): with fps60, per emitter accumulate `dt`, call the game's emit
  once the sum reaches 0.75/30 s with the summed `dt`. Particles still move every frame. Without fps60 unchanged.
- Result: 49 → 0 flash frames (PR clip); pool ~1560 at 60 fps like 30 fps. `births` per emitter, path 1 (this update): ~1.0x of
  30 fps in `i004`, `i001`, `e001`, `h002` (torches 419.5/s both). Smoke `i001`, `h001`, `i004`. Human visual: fixed. PR body has
  before/after `.mp4`.

**PR #14 bug found later (2026-10-24, path-2 work):** passing the *summed* dt doubles particle speed. Emit sets
velocity = asset velocity x dt and MOVE (`0x800C38F0`, PARS command 3) adds velocity to position every frame, no dt.
`births` speed column: every p1 emitter 2.00x at 60 fps (torch flames twice as tall, seen by the human in i004). Also the
gate skipped timer-driven emitters (interval > 0) too. Fix `132da75` on `fix/fog-fps60`: gate only when the emitter timer
+0x1C <= 0 (emits every call), at most once per 30 Hz step, and pass this frame's dt -> all p1 1.00x count and speed in
i004/i001/e001/h002; grate flash frames 1/219 (30 fps baseline 3/171). Smoke i001/h001/h002/i004.

## Path 2 — `fix/particles-fps60-path2` `212643d` (off `main`)

- `0x800C5C0C` "emit with overrides": applies overrides (flags word at ov+0), optional save/restore (flag 0x4000, save
  area `0x802486A0`), emit (`bl 0x800C4B04` at `0x800C5EB8`, caller's f1 passed through), restore. Was native in full;
  since `refactor/particles-entity-wrapper` (2026-11-22) `hle/no100f_particles_entity.cpp` wraps emit `0x800C4B04`
  instead and acts only when lr == `0x800C5EBC` (emit's only other caller is the update, `0x800C4AE8`), so the game's
  own code applies and restores the overrides. Callers: 89 sites; 36 pass a fixed
  1/30 (sda2 `0x3D088889`) instead of frame dt -> 2x speed at 60 fps (`PAREMIT_CHAND_CANDLE`), plus 2x count for timer-0
  emitters (`SMALL_VAN_PUFF`, `FIREWORKS_TRAIL`, chandelier candles).
- Fix (fps60 only): dt == 1/30 -> frame time `[0x80270D84]` (r13-0x74FC, clamped 1/60..0.1); if timer <= 0, per emitter skip
  the emit when it emitted in the previous frame (`[0x80270890]` frame counter, r13-0x79F0) unless that frame was a full
  30 Hz step. Same-frame calls all emit (chandeliers: 16 calls/frame on one emitter). Bursts never dropped (probe: every
  firework burst a 1-frame streak, skipped 0).
- Result: VAN_PUFF 0.96-1.01x, FIREWORKS_TRAIL 0.75-0.91x (random launch timing), CHAND_CANDLE speed 2.00x -> 1.00x.
  Tried a shared global 30 Hz phase for path 1+2: no measurable difference, dropped (keeps the two files independent).
- Probes: `patches/particles-path2-probes.patch` (written against the full port: no longer applies to the wrapper; p2 tag, per-asset call streaks / skips / caller lr+flags / avg system
  population -> `postool/streaks.txt`; `births-run.ps1` copies extra postool txt files).
- `births` is wrong for `PAREMIT_CHAND_CANDLE` (13 "births"/s but ~0.6 alive with 0.75 s life): use the population probe.
  i004 CHAND_CANDLE at 60 fps ~0 on every build including `main` (pre-existing).
- Not measured: rain/snow (`PAREMIT_RAIN`/`SNOW`, holiday/code-driven); `PAREMIT_SD_FLEASCRATCH` (path 1, interval 0.01 < 1/60)
  is timer-driven so not gated by #14's fix (expected ~2x at 60 fps, tiny; random idle anim, not measured reliably).
