"""Relighting experiment: measure how a relit screenshot differs from the game's own look, to steer the light file.

  compare BASE.png TEST.png [--light-file F] [--grid 4x3]
      Lightness, contrast, colour cast and saturation of TEST against BASE (same camera), per screen region and per
      tone band (BASE's darkest, middle and brightest thirds), with suggested light file changes and a picture of
      the differences (TEST's name + "-diff.png").

  shots [--views FILE] [--reload] [--light-file F]
      With the game running (manual-session start, NO100F_RELIGHT set): for each view, moves the free camera there,
      takes the game's look (dev menu "relight 0") and the relit look ("relight 1"), then compares each pair.
      --reload reloads the light file first ("relight reload"), so an edited file is measured without relaunching.

Colour science: sRGB is decoded to linear light and converted to CIELAB (D65). L* is lightness (0 black, 100 white;
about 10 L* is clearly visible), a* is green (-) to magenta (+), b* is blue (-) to yellow (+), C* is saturation.
"Stops" are doublings of linear light: +1 stop = twice as bright.

Suggestions assume what the light file does: the relit pixel is the texture times the light, so the game's look over
the relit look, in linear RGB, is the change of light that would match it. The dark third is mostly ambient, the
bright third mostly moon (and lamps near them). Matching the game is a reference point, not the goal: a darker or
bluer night may be wanted. The shots differ a little wherever something animates.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]

# h002 views: name, then the dev menu free camera (cam X Y Z YAW PITCH), or "-" for the start view (no free camera;
# must come first, as the free camera stays on).
DEFAULT_VIEWS = """
start -
scooby 23 5 9 110 35
top 20 14 2 -90 55
playground 12 5 20 -135 20
"""


# ----- colour ---------------------------------------------------------------------------------------------------------

def srgb_to_linear(c: np.ndarray) -> np.ndarray:
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_lab(rgb: np.ndarray) -> np.ndarray:
    m = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = rgb @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), (24389 / 27 * xyz + 16) / 116)
    lab = np.empty_like(xyz)
    lab[..., 0] = 116 * f[..., 1] - 16
    lab[..., 1] = 500 * (f[..., 0] - f[..., 1])
    lab[..., 2] = 200 * (f[..., 1] - f[..., 2])
    return lab


def load(path: Path) -> tuple[np.ndarray, np.ndarray]:
    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255.0
    lin = srgb_to_linear(rgb)
    return lin, linear_to_lab(lin)


def hud_mask(h: int, w: int) -> np.ndarray:
    """True where the picture counts: not the DEV build badge in the bottom-left corner."""
    mask = np.ones((h, w), bool)
    mask[int(h * 0.955):, : int(w * 0.21)] = False
    return mask


# ----- measuring ------------------------------------------------------------------------------------------------------

def stats(lin: np.ndarray, lab: np.ndarray, sel: np.ndarray) -> dict | None:
    if sel.sum() < 50:
        return None
    l = lab[..., 0][sel]
    return {
        "n": int(sel.sum()),
        "L": float(l.mean()),
        "a": float(lab[..., 1][sel].mean()),
        "b": float(lab[..., 2][sel].mean()),
        "C": float(np.hypot(lab[..., 1][sel], lab[..., 2][sel]).mean()),
        "rgb": lin[sel].mean(axis=0),
        "p10": float(np.percentile(l, 10)),
        "p90": float(np.percentile(l, 90)),
    }


def stops(base: dict, test: dict) -> float:
    y = lambda s: float(s["rgb"] @ np.array([0.2126, 0.7152, 0.0722]))
    return float(np.log2(max(y(test), 1e-6) / max(y(base), 1e-6)))


def cast_words(da: float, db: float) -> str:
    words = []
    if db < -2.5:
        words.append(f"bluer ({db:+.1f} b*)")
    elif db > 2.5:
        words.append(f"yellower ({db:+.1f} b*)")
    if da < -2.5:
        words.append(f"greener ({da:+.1f} a*)")
    elif da > 2.5:
        words.append(f"more magenta ({da:+.1f} a*)")
    return ", ".join(words) if words else "same hue"


def light_lines(path: Path | None) -> dict[str, list[float]]:
    lines: dict[str, list[float]] = {}
    if path is None or not path.exists():
        return lines
    for line in path.read_text().splitlines():
        parts = line.split("#")[0].split()
        if parts and parts[0] in ("ambient", "moon"):
            try:
                lines[parts[0]] = [float(v) for v in parts[1:]]
            except ValueError:
                pass
    return lines


def compare(base_path: Path, test_path: Path, light_file: Path | None, grid: tuple[int, int]) -> dict:
    base_lin, base_lab = load(base_path)
    test_lin, test_lab = load(test_path)
    if base_lin.shape != test_lin.shape:
        sys.exit(f"compare: {base_path.name} and {test_path.name} differ in size")
    h, w = base_lab.shape[:2]
    valid = hud_mask(h, w)
    # Pixels that changed hugely in hue and lightness at once are mostly animation (a character moved): left out.
    d = np.linalg.norm(test_lab - base_lab, axis=-1)
    valid &= d < np.percentile(d[valid], 99)

    print(f"\n=== {test_path.name}  vs  {base_path.name}")
    whole_b, whole_t = stats(base_lin, base_lab, valid), stats(test_lin, test_lab, valid)
    contrast_b = whole_b["p90"] - whole_b["p10"]
    contrast_t = whole_t["p90"] - whole_t["p10"]
    print(f"whole picture: L* {whole_b['L']:.1f} -> {whole_t['L']:.1f} ({stops(whole_b, whole_t):+.2f} stops), "
          f"contrast (L* p90-p10) {contrast_b:.1f} -> {contrast_t:.1f}, "
          f"saturation C* {whole_b['C']:.1f} -> {whole_t['C']:.1f}, "
          f"{cast_words(whole_t['a'] - whole_b['a'], whole_t['b'] - whole_b['b'])}")

    # Tone bands from the game's look: the same pixels in both.
    lo, hi = np.percentile(base_lab[..., 0][valid], [33.3, 66.7])
    bands = {
        "dark third": valid & (base_lab[..., 0] < lo),
        "middle third": valid & (base_lab[..., 0] >= lo) & (base_lab[..., 0] < hi),
        "bright third": valid & (base_lab[..., 0] >= hi),
    }
    print(f"\n{'tone band':<14}{'L* game':>9}{'L* relit':>10}{'stops':>8}{'dC*':>7}  cast")
    band_ratio = {}
    for name, sel in bands.items():
        b, t = stats(base_lin, base_lab, sel), stats(test_lin, test_lab, sel)
        if b is None or t is None:
            continue
        band_ratio[name] = b["rgb"] / np.maximum(t["rgb"], 1e-6)
        print(f"{name:<14}{b['L']:>9.1f}{t['L']:>10.1f}{stops(b, t):>+8.2f}{t['C'] - b['C']:>+7.1f}  "
              f"{cast_words(t['a'] - b['a'], t['b'] - b['b'])}")

    cols, rows = grid
    print(f"\nregions (stops relit vs game; + = brighter), {cols}x{rows}, top row first:")
    region_stops = np.full((rows, cols), np.nan)
    region_cast = [["" for _ in range(cols)] for _ in range(rows)]
    for r in range(rows):
        cells = []
        for c in range(cols):
            sel = np.zeros_like(valid)
            sel[r * h // rows:(r + 1) * h // rows, c * w // cols:(c + 1) * w // cols] = True
            b, t = stats(base_lin, base_lab, sel & valid), stats(test_lin, test_lab, sel & valid)
            if b is None or t is None:
                cells.append(f"{'-':>18}")
                continue
            region_stops[r, c] = stops(b, t)
            db = t["b"] - b["b"]
            region_cast[r][c] = "blue" if db < -2.5 else "yellow" if db > 2.5 else ""
            cells.append(f"{region_stops[r, c]:+6.2f} L*{t['L'] - b['L']:+5.1f} {region_cast[r][c]:<5}")
        print("  " + " |".join(cells))

    # Suggestions.
    print("\nsuggestions (to match the game's look; take part of the way, re-shoot):")
    lines = light_lines(light_file)
    damp = 0.7  # move 70% of the way in log terms: the bands mix ambient, moon and lamps
    for key, band in (("ambient", "dark third"), ("moon", "bright third")):
        if band not in band_ratio:
            continue
        mult = np.clip(band_ratio[band], 0.25, 4.0) ** damp
        text = f"  {key:<8} x ({mult[0]:.2f}, {mult[1]:.2f}, {mult[2]:.2f})  [from the {band}]"
        current = lines.get(key)
        if current is not None:
            colour = current[-3:]
            new = [round(v * m, 3) for v, m in zip(colour, mult)]
            prefix = " ".join(f"{v:g}" for v in current[:-3])
            text += f"\n           -> {key} {prefix + ' ' if prefix else ''}{new[0]:.3f} {new[1]:.3f} {new[2]:.3f}"
        print(text)
    if contrast_t < contrast_b - 5:
        print(f"  flatter than the game (contrast {contrast_t:.1f} vs {contrast_b:.1f}): lower ambient against moon")
    elif contrast_t > contrast_b + 5:
        print(f"  harsher than the game (contrast {contrast_t:.1f} vs {contrast_b:.1f}): raise ambient against moon")
    if whole_t["C"] < whole_b["C"] - 3:
        print("  less saturated: the light colours are greyer than the baked tint (raise their dominant channel)")

    out = test_path.with_name(test_path.stem + "-diff.png")
    diff_picture(base_path, test_path, base_lab, test_lab, valid, region_stops, region_cast, out)
    print(f"\npicture: {out}")
    return {"stops": stops(whole_b, whole_t), "regions": region_stops, "ratios": band_ratio,
            "contrast": (contrast_b, contrast_t)}


def diff_picture(base_path, test_path, base_lab, test_lab, valid, region_stops, region_cast, out: Path) -> None:
    """Game | relit | lightness difference (blue = relit darker, red = brighter, 20 L* = full), half size, with the
    regions' stops written on the difference."""
    h, w = base_lab.shape[:2]
    dl = np.clip((test_lab[..., 0] - base_lab[..., 0]) / 20.0, -1, 1)
    heat = np.zeros((h, w, 3))
    heat[..., 0] = np.where(dl > 0, dl, 0)
    heat[..., 2] = np.where(dl < 0, -dl, 0)
    grey = (base_lab[..., 0] / 100.0)[..., None] * 0.35
    heat = np.clip(grey + heat * 0.9, 0, 1)
    heat[~valid] = 0.15
    heat_img = Image.fromarray((heat * 255).astype(np.uint8))
    rows, cols = region_stops.shape
    draw = ImageDraw.Draw(heat_img)
    for r in range(rows):
        for c in range(cols):
            x0, y0 = c * w // cols, r * h // rows
            draw.rectangle([x0, y0, (c + 1) * w // cols, (r + 1) * h // rows], outline=(90, 90, 90))
            if not np.isnan(region_stops[r, c]):
                draw.text((x0 + 8, y0 + 6), f"{region_stops[r, c]:+.2f} {region_cast[r][c]}", fill=(255, 255, 255))
    tiles = [Image.open(base_path).convert("RGB"), Image.open(test_path).convert("RGB"), heat_img]
    tw, th = w // 2, h // 2
    sheet = Image.new("RGB", (tw * 3, th), (0, 0, 0))
    for i, tile in enumerate(tiles):
        sheet.paste(tile.resize((tw, th), Image.LANCZOS), (i * tw, 0))
    sheet.save(out)


# ----- driving the game -----------------------------------------------------------------------------------------------

def dev_command(command: str, settle: float) -> None:
    play = REPO / "Play"
    cmd_file = play / "devmenu.txt"
    cmd_file.write_text(command + "\n")
    deadline = time.time() + 15
    while cmd_file.exists():
        if time.time() > deadline:
            sys.exit(f"shots: the game did not take '{command}' (not in play?)")
        time.sleep(0.1)
    time.sleep(settle)


def shot(label: str) -> Path:
    script = REPO / "tools" / "debug" / "manual-session.ps1"
    result = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "shot", "-Label", label],
        capture_output=True, text=True, cwd=REPO)
    m = re.search(r"shot: (.+?\.png)", result.stdout)
    if not m:
        sys.exit(f"shots: no screenshot for {label}:\n{result.stdout}\n{result.stderr}")
    return Path(m.group(1))


def shots(views_text: str, reload: bool, light_file: Path | None, grid: tuple[int, int]) -> None:
    views = []
    for line in views_text.splitlines():
        parts = line.split("#")[0].split()
        if parts:
            views.append((parts[0], parts[1:]))
    if reload:
        dev_command("relight reload", 1.0)
    log = REPO / "Play" / "devmenu.log"
    summary = []
    for name, cam in views:
        if cam and cam != ["-"]:
            dev_command("cam " + " ".join(cam), 1.5)
        else:
            dev_command("freecam off", 1.5)  # the game's own camera
        dev_command("relight 0", 1.5)
        base = shot(f"rl-{name}-game")
        dev_command("relight 1", 1.5)
        test = shot(f"rl-{name}-relit")
        if log.exists() and "relight on" not in log.read_text(errors="replace").splitlines()[-1]:
            sys.exit("shots: relighting did not turn on (is NO100F_RELIGHT set for this run?)")
        summary.append((name, compare(base, test, light_file, grid)))
    print("\n=== all views (relit vs game)")
    for name, result in summary:
        cb, ct = result["contrast"]
        print(f"  {name:<12}{result['stops']:+.2f} stops, contrast {cb:.1f} -> {ct:.1f}")
    # One suggestion for all views: the geometric mean of their multipliers, damped as in compare.
    lines = light_lines(light_file)
    print("suggestions for all views together:")
    for key, band in (("ambient", "dark third"), ("moon", "bright third")):
        ratios = [r["ratios"][band] for _, r in summary if band in r["ratios"]]
        if not ratios:
            continue
        mult = np.exp(np.mean([np.log(np.clip(r, 0.25, 4.0)) for r in ratios], axis=0) * 0.7)
        text = f"  {key:<8} x ({mult[0]:.2f}, {mult[1]:.2f}, {mult[2]:.2f})"
        if key in lines:
            current = lines[key]
            new = [v * m for v, m in zip(current[-3:], mult)]
            prefix = " ".join(f"{v:g}" for v in current[:-3])
            text += f"  ->  {key} {prefix + ' ' if prefix else ''}{new[0]:.3f} {new[1]:.3f} {new[2]:.3f}"
        print(text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("compare")
    c.add_argument("base", type=Path)
    c.add_argument("test", type=Path)
    s = sub.add_parser("shots")
    s.add_argument("--views", type=Path, help="file of 'name X Y Z YAW PITCH' lines ('name -' = the start view)")
    s.add_argument("--reload", action="store_true")
    for p in (c, s):
        p.add_argument("--light-file", type=Path, default=REPO / "autotest" / "relight" / "h002.txt")
        p.add_argument("--grid", default="4x3")
    args = parser.parse_args()
    grid = tuple(int(v) for v in args.grid.lower().split("x"))
    if args.mode == "compare":
        compare(args.base, args.test, args.light_file, grid)
    else:
        shots(args.views.read_text() if args.views else DEFAULT_VIEWS, args.reload, args.light_file, grid)


if __name__ == "__main__":
    main()
