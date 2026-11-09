---
name: audio-debug
description: >-
  Capture and analyse the Scooby-Doo recomp's audio (AX HLE mixer, ADPCM streams, output limiter, SDL backend)
  to root-cause crackle, clicks, fuzz, dropouts or wrong sounds. Use for any audio bug, before guessing at
  Windows settings, resamplers or buffer sizes.
---

# Audio debugging

The game's audio goes: DSP-ADPCM voices in ARAM → `AcceleratorT` decode (`hle/audio/ax_internal.h`) → per-voice
SRC (`ResampleAudio`, `ax_mix.cpp`) → mix at 32 kHz → `GcOutputSamples` (tanh soft limiter, knee 24576) →
`audio_backend.cpp` (SDL3 stream, drops whole blocks past a 120 ms queue). The HLE is ported from Dolphin, so a
bug there is often also in Dolphin HLE. Goal is a clean remaster, not matching Dolphin.

## Order of work

1. **Capture what the game mixed** (no SDL, no Windows): `AUDIO_RAW_DUMP=<file>` (built in) before
   `manual-session start -Sound` → BE16 stereo R/L, 32 kHz. Audio work runs with `-Sound` (on at 5%); without it test
   runs are muted. Mute/volume are SDL output gain only, so the dump is the same either way.
2. **Measure, don't just listen:** `python tools\debug\audio\crackle.py <dump> [--wav out.wav] [--from S --to S]`
   → 2nd-difference clicks, flat tops (clipping), zero gaps (drops), limiter-knee counts, per-second table, strongest
   clicks with frame numbers. Look for a **grid** in click times (a period = a buffer/loop size).
3. **Split by stage** with `tools\debug\patches\audio-diagnostic.patch` (`git apply`; env `AX_DIAG_DIR=<dir>`):
   - `bus.raw` pre-limiter MAIN L/R int32 (`--fmt bus`), `stream.raw` stream voice post-SRC, `stream_src.raw`
     stream voice as decoded (pre-SRC, source rate), `stream_pos.txt` per-frame stream address + `WRAP` lines
     (loop pred/scale vs ARAM header), `events.txt` (`B` voice start, `K` stopped, `E` ended, `S` depop, `C` stream
     coefs + ARAM bytes). Flushes about once a second, so killing the game keeps the data.
   - `AX_HEADROOM_DB=<n>` scales the bus before the limiter (overload test). Backend logs every `[drop]`.
4. **Check against the disc** (ground truth for "is it in the recording?"):
   `python tools\debug\audio\adpcm_ref.py Disc\DATA\files\boot.HIP --list` then
   `adpcm_ref.py boot.HIP <asset id> --compare <AX_DIAG_DIR>\stream_src.raw` → bit-exact or first differing sample.
5. Strip the patch (`git apply -R`) before a PR; re-capture with the fix-only build.

For music-only listening in the hubs, make sure no holiday fires: builds with PR #13 take `[game] holiday = "off"`; older
builds read the date 2000-01-01 (fireworks every load) unless they have PR #12 or `tools\debug\patches\real-date.patch`
(`NO100F_REAL_DATE=1`).

**Stuck loop** (music repeating ~1 s): `python tools\debug\audio\loopcheck.py <dump> --scan 10` (stuck ring ≈ 0.99, normal
music ≈ 0.1). `tools\debug\audio\loop-capture.ps1 -Label x [-Boot h001] [-Hd] [-Cmd "scene H001" -CmdAt 20]` boots a scene
with the dump on (sound at 5%) and checks it; `texhunt\hunt.ps1` checks every session.

Issue 14's cause: the game's end-of-track check kept a stale value between streams (`hle/no100f_stream_read.h`). To see it
come back, take a save in `g001`, walk left to the warp gate and warp to the Mystery Machine (`h001`) with the map open
within about 1.2 s of the music starting: the stream restarts every 1.02 s. Dev tools don't matter. Dev menu `scene` warps
and slow play miss the window, so a clean run from them says nothing about this bug. Other stuck-sounding bugs may still
exist: check the stream entry's flags before assuming this one. `tools\debug\audio\issue14-route.txt` is that route as a
pad script (`tools\debug\patches\pad-script.patch`, usage in `game-logic-debug`); it needs your own save.

## Game facts

- Music = one ADPCM **stream** voice (`is_stream 1`, e.g. PB `8025EAF4` in `h002`), 48 kHz source (SNDI may say 47999),
  SRC ratio `0x17FFD`, `src_type 1` (linear). Ring buffer in ARAM refilled by the game; `h002` ring = 48,000 nibbles =
  0.875 s. The game never sets `adpcm_loop_info` (old AX lib, always 0).
- Stream table: 6 entries at `0x8024C258`, `0x104` apart: +0 voice, +4 flags, +0x88 disc position, +0x8C size, +0x90 bytes
  read, +0x94 bytes read at the last crossing into half 1. Each chunk is `0x2EE0` bytes (0.4375 s). The refill runs in
  `fn_800D9EF8` (per AX frame); stream start is `fn_800D8CA8`. Flags: 0x20 end, 0x40 final, 0x800 chunk ready,
  0x1000/0x2000 half 1/2 full, 0x4000/0x8000 playing half 1/2. The warp map pauses the voice (flag 0x2).
- Tracks live in `boot.HIP` as `SNDS` assets (raw ADPCM frames, no header; names end `-L`). Coefficients etc. in the
  `SNDI` asset: u32 count, then per entry **[0x60-byte DSP header][u32 asset id]** — the id comes *after* its header
  (reading it as id-then-header gives the neighbour's coefs). `h002` music = `9974599F` "action loop 06-L".
- Level `SNDS` assets in level HIPs are voice lines, not music.
- The AX partial-versus-full address update is handled in `hle/audio/ax_address_sync.cpp`; the old clock cap is gone (`audio_poll_clock.h`).
- Volume sliders (F10) add a per-category offset in a native override of `MIXAdjustFader` (`0x800DD2A0`, the engine's only per-voice volume call, every frame; `music_attenuation.cpp`).
  The game only knows music (record flag `0x400`) versus SFX; ours: Music = `0x400` from the boot/mnu4 banks, Voices = `0x400` from a level bank (NPC dialog, `cin_XX_m`), Menus = the mnu4.HIP bank, SFX = the rest.
  Limits: resident dialog (Scooby barks, `holly_51a`) counts as SFX, Bink movie audio is master only, and the `w028` dock ambience loops count as Voices.
- `h002` = Mystic Playground (the reported crackle level).

## Traps

- Click detectors also flag drum hits; compare a before/after pair sample by sample (decode is deterministic,
  differences = the change) instead of trusting counts.
- Spectrograms hide few-ms cracks in busy music; zoom the waveform (30–40 ms) at a known event.
- Fireworks/SFX push the bus past the limiter knee (peaks ~41k pre-limiter) → fuzz that is not a decode bug.
- GitHub won't attach `.wav`/`.mp3`; post clips as `.mp4` (title card + audio). `gh` can't upload attachments —
  insert a placeholder section and have the human drag files in.

Case history: `crackle-pr11.md`.
