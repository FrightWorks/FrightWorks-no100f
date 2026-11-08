"""Checks the smoke jobs share (Python 3.8+, standard library; numpy only for the stuck-music check).

  png_is_blank(path)      a screenshot with (almost) no picture, decoded with zlib: no Pillow needed
  audio_peak(raw)         loudest sample of an AUDIO_RAW_DUMP file (big-endian 16-bit stereo, 32 kHz)
  loopcheck(raw, tool)    is the output stuck repeating a short loop? runs tools/debug/audio/loopcheck.py (needs numpy)
  hd_drawn(game)          how many of the HD pack's textures the game has drawn in this scene (the `textures` command)
  fps_samples / fps_summary / fps_too_slow   the frame rate the game logs every two seconds (NO100F_LOG_FPS=1) on stderr
"""
import array
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

AUDIO_RATE = 32000
AUDIO_BYTES_PER_SECOND = AUDIO_RATE * 4
FPS_LINE = re.compile(r"\[vi\] fps (\d+(?:\.\d+)?):")


class CheckNotRun(Exception):
    """A check that could not run (a tool or library is missing). Reported as not run, never as passed."""


# ---- pictures -------------------------------------------------------------------------------------------------------
def png_pixels_lit(path, threshold=16):
    """(lit, total): how many pixels have a luminance of at least `threshold`. 8-bit gray/RGB/RGBA, not interlaced."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, header = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        pos += 12 + length
    width, height, depth, ctype, _c, _f, interlace = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or channels is None or interlace:
        raise ValueError(f"{path}: unsupported PNG (depth {depth}, type {ctype}, interlace {interlace})")
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    prev = bytearray(stride)
    lit = 0
    for y in range(height):
        base = y * (stride + 1)
        ftype, line = raw[base], bytearray(raw[base + 1:base + 1 + stride])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 255
            elif ftype == 2:
                line[i] = (line[i] + b) & 255
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(width):
            px = line[x * channels:x * channels + channels]
            lum = px[0] if channels < 3 else (px[0] * 299 + px[1] * 587 + px[2] * 114) // 1000
            lit += lum >= threshold
    return lit, width * height


def png_is_blank(path, min_lit_fraction=0.02):
    """True when fewer than 2 % of the pixels are lit: the window shows no picture."""
    lit, total = png_pixels_lit(path)
    return lit < total * min_lit_fraction


# ---- audio ----------------------------------------------------------------------------------------------------------
def audio_seconds(raw):
    try:
        return Path(raw).stat().st_size / AUDIO_BYTES_PER_SECOND
    except OSError:
        return 0.0


def audio_peak(raw):
    samples = array.array("h")
    with open(raw, "rb") as f:
        data = f.read()
    samples.frombytes(data[:len(data) // 2 * 2])
    if sys.byteorder == "little":
        samples.byteswap()
    return max((abs(s) for s in samples), default=0)


def loopcheck(raw, repo_tools, skip=8, length=10):
    """Correlation of the dump's best repeat (0.9 and more = stuck). CheckNotRun when numpy is missing."""
    tool = Path(repo_tools) / "debug" / "audio" / "loopcheck.py"
    r = subprocess.run([sys.executable, str(tool), str(raw), "--skip", str(skip), "--len", str(length)],
                       capture_output=True, text=True)
    text = r.stdout + r.stderr
    if "No module named 'numpy'" in text or "ModuleNotFoundError" in text:
        raise CheckNotRun("loopcheck needs numpy (pip install numpy)")
    m = re.search(r"correlation ([\d.]+)", text)
    if not m:
        raise RuntimeError(f"loopcheck gave no result: {text.strip()[-300:]}")
    return float(m.group(1))


# ---- HD textures ----------------------------------------------------------------------------------------------------
def write_hd_probe(play_dir, pack_dir):
    """Register every file of the HD pack as a `textures` suspect (UserData/devmenu_textures.txt, id = file stem). The game
    then lists the ones it really drew in the current scene (aurora_texture_replacement_used), not "the picture differs"."""
    stems = sorted(f.stem for f in Path(pack_dir).glob("*.png"))
    if not stems:
        raise CheckNotRun(f"no HD pack files in {pack_dir}")
    lines = [f"{s}\t{s}\t1\t0\tboot\tprobe\n" for s in stems]
    (Path(play_dir) / "UserData" / "devmenu_textures.txt").write_text("".join(lines), encoding="utf-8")
    return len(stems)


def hd_drawn(devmenu, timeout=20):
    """Number of pack textures drawn in this scene so far (ask a second after the scene became playable: the game
    checks every 30 frames)."""
    text = devmenu.command("textures", timeout)
    return len(text.replace("drawn suspects:", "").split())


# ---- frame rate -----------------------------------------------------------------------------------------------------
def fps_samples(text):
    """The frame rates in a block of the game's stderr: one `[vi] fps 59.8: game ...` line per two seconds."""
    return [float(m) for m in FPS_LINE.findall(text)]


def fps_summary(samples, skip_first=True):
    """{'n', 'min', 'median', 'last'} of the samples, or None when there are none. The first sample is dropped when more
    than two came: its two-second window began before the level was playable (loading), so it is not the level's rate."""
    if skip_first and len(samples) > 2:
        samples = samples[1:]
    if not samples:
        return None
    ordered = sorted(samples)
    mid = len(ordered) // 2
    median = ordered[mid] if len(ordered) % 2 else (ordered[mid - 1] + ordered[mid]) / 2
    return {"n": len(samples), "min": ordered[0], "median": round(median, 1), "last": samples[-1]}


def fps_too_slow(summary, minimum):
    """True when the median frame rate is below `minimum`. No samples is not slow: the caller reports it as not run."""
    return summary is not None and summary["median"] < minimum
