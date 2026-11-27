"""Builds the launcher's mansion art from the concept picture.

    python tools/launcher-art/make_launcher_art.py <concept.png> [out_dir]
    python tools/launcher-art/make_launcher_art.py --icon <icon.png> <file.ico> [...]   (the app icon)

The app icon's source is the FrightWorks organisation's square picture for this game (not in git). --icon
writes it as an .ico with every size Windows asks for; the repo keeps two copies, launcher/frightworks.ico (the
launcher, the installer and the Linux package) and wiicompiled/runtime/assets/icon/frightworks.ico (the game).

The concept is a 1447x1087 picture of a haunted mansion on black with lit windows and five menu signs
(not in git). Output, by default in launcher/art:

  mansion.png  the house on a transparent background. Every window's glass is a hole, so the launcher draws
               the lights, curtains and silhouettes behind the house and the frames sit on top.
  dog.png      a plain Great Dane silhouette (the concept's, without its collar tag), for the big left window.
  ghost.png    the ghost silhouette from the tower window (its face stays a hole the light shows through).
  mansion.json window boxes (where the launcher draws), sign outlines (where it draws its own signs, covering
               the painted ones) and the picture size.

Needs numpy and Pillow.
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# Window glass. "warm": the lit pixels in the box become the hole and the painted mullions stay. "pane": a
# silhouette covers the glass, so the whole pane (the hull of its lit pixels) becomes the hole and the mullions
# listed are drawn back as clean lines: (x0, y0, x1, y1, width).
WINDOWS = [
    dict(id="tower", box=(512, 150, 628, 368), mode="pane", group="tower",
         mullions=[(563, 156, 561, 366, 3), (522, 204, 608, 193, 3), (545, 199, 533, 180, 3), (585, 195, 592, 176, 3)]),
    dict(id="tower_side", box=(650, 215, 664, 310), mode="warm", group="tower"),
    dict(id="diamond", box=(888, 250, 970, 352), mode="warm", group="attic"),
    dict(id="right_tower", box=(1100, 280, 1180, 380), mode="warm", group="right_tower"),
    dict(id="dormer", box=(300, 410, 352, 532), mode="warm", group="dormer"),
    dict(id="gable", box=(520, 484, 606, 556), mode="warm", group="gable"),
    dict(id="parlor", box=(128, 582, 266, 834), mode="pane", group="parlor",
         mullions=[(134, 641, 264, 638, 4), (170, 588, 168, 640, 4), (202, 584, 202, 640, 4), (235, 598, 235, 640, 4),
                   (168, 640, 162, 834, 4), (228, 639, 232, 834, 4), (130, 743, 264, 741, 4)]),
    dict(id="parlor_side", box=(288, 672, 322, 840), mode="warm", group="parlor_side"),
    dict(id="fanlight", box=(440, 636, 610, 695), mode="warm", group="entrance"),
    dict(id="door_left", box=(444, 708, 465, 840), mode="warm", group="entrance"),
    dict(id="door_right", box=(587, 700, 609, 834), mode="warm", group="entrance"),
    dict(id="right_arch", box=(1252, 700, 1310, 868), mode="warm", group="right_arch"),
]

# The painted signs, top to bottom: (id, box, colour test).
SIGNS = [
    ("play", (728, 388, 1205, 548), "lime"),
    ("settings", (752, 545, 1198, 648), "purple"),
    ("mods", (758, 648, 1192, 752), "purple"),
    ("extras", (765, 752, 1190, 860), "purple"),
    ("exit", (775, 858, 1190, 965), "purple"),
]

SS = 4  # supersampling for drawn shapes


def max_filter(mask, size):
    return np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(size))) > 127


def min_filter(mask, size):
    return np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(size))) > 127


def components(mask):
    """4-connected components of a boolean mask: a label array and the sizes (index = label)."""
    img = Image.fromarray(mask.astype(np.uint8) * 255).copy()
    labels = np.zeros(mask.shape, np.int32)
    sizes = [0]
    ys, xs = np.nonzero(mask)
    work = img.copy()
    for y, x in zip(ys, xs):
        if work.getpixel((int(x), int(y))) != 255:
            continue
        n = len(sizes)
        value = 1 + (n % 250)  # floodfill marks with a grey; read back which pixels changed
        ImageDraw.floodfill(work, (int(x), int(y)), value)
        region = (np.asarray(work) == value) & (labels == 0) & mask
        labels[region] = n
        sizes.append(int(region.sum()))
    return labels, sizes


def largest(mask):
    ys, xs = np.nonzero(mask)
    if len(ys) == 0:
        return mask
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    labels, sizes = components(mask[y0:y1, x0:x1])
    out = np.zeros_like(mask)
    if len(sizes) >= 2:
        out[y0:y1, x0:x1] = labels == int(np.argmax(sizes))
    return out


def big_parts(mask, min_size):
    ys, xs = np.nonzero(mask)
    out = np.zeros_like(mask)
    if len(ys) == 0:
        return out
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    labels, sizes = components(mask[y0:y1, x0:x1])
    keep = [i for i, s in enumerate(sizes) if i > 0 and s >= min_size]
    out[y0:y1, x0:x1] = np.isin(labels, keep)
    return out


def fill_holes(mask):
    """Fills regions of ~mask not connected to the border."""
    outside = Image.fromarray((~mask).astype(np.uint8) * 255).copy()
    padded = Image.new("L", (outside.width + 2, outside.height + 2), 255)
    padded.paste(outside, (1, 1))
    ImageDraw.floodfill(padded, (0, 0), 128)
    reach = np.asarray(padded)[1:-1, 1:-1] == 128
    return ~reach


def hull(points):
    """Convex hull (monotone chain) of (x, y) points."""
    pts = sorted(set(points))
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def draw_coverage(size, draw_fn):
    """Antialiased coverage (0..1) of shapes drawn by draw_fn(draw, scale) at SS times the size."""
    w, h = size
    big = Image.new("L", (w * SS, h * SS), 0)
    draw_fn(ImageDraw.Draw(big), SS)
    return np.asarray(big.resize((w, h), Image.BOX)).astype(np.float32) / 255


def make_icon(src, path):
    """The app icon from its square source picture (cropped to the middle square if it isn't one), as an .ico
    with all the sizes Windows asks for."""
    im = Image.open(src).convert("RGBA")
    side = min(im.size)
    left, top = (im.width - side) // 2, (im.height - side) // 2
    im = im.crop((left, top, left + side, top + side))
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    im.resize((256, 256), Image.LANCZOS).save(path, sizes=[(s, s) for s in sizes])


def main():
    if sys.argv[1:2] == ["--icon"]:
        if len(sys.argv) < 4:
            sys.exit("usage: make_launcher_art.py --icon <icon.png> <file.ico> [...]")
        for p in sys.argv[3:]:
            make_icon(sys.argv[2], p)
        return
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "..", "launcher", "art")
    os.makedirs(out, exist_ok=True)
    img = Image.open(src).convert("RGB")
    rgb = np.asarray(img).astype(np.int32)
    H, W = rgb.shape[:2]
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    peak = rgb.max(axis=2)

    # The house: everything not connected to the black border, plus a 4 px ring of the black outline around it.
    dark = Image.fromarray((peak <= 3).astype(np.uint8) * 255).copy()  # fromarray images are read-only: floodfill would do nothing
    for x, y in [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1)] + [(x, 0) for x in range(0, W, 16)] + \
            [(x, H - 1) for x in range(0, W, 16)] + [(0, y) for y in range(0, H, 16)] + [(W - 1, y) for y in range(0, H, 16)]:
        if dark.getpixel((x, y)) == 255:
            ImageDraw.floodfill(dark, (x, y), 128)
    background = np.asarray(dark) == 128
    house = max_filter(~background, 9)
    alpha = np.asarray(Image.fromarray(house.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.8))).astype(np.float32) / 255

    out_rgb = rgb.astype(np.float32).copy()
    hole = np.zeros((H, W), np.float32)  # 1 = glass (fully see-through)
    glass = np.zeros((H, W), bool)  # pixels that become frame colour (glass edges, redrawn mullions)
    lit = (r > 150) & (r - b > 90)
    windows_json = []
    silhouettes = {}
    for win in WINDOWS:
        x0, y0, x1, y1 = win["box"]
        sub = (slice(y0, y1), slice(x0, x1))
        core = np.zeros((H, W), bool)
        core[sub] = lit[sub]
        if win["mode"] == "warm":
            zone = max_filter(core, 5)
            f = np.clip((r - 30) / 180.0, 0, 1) * (b < r + 10) * zone
        else:
            ys, xs = np.nonzero(core)
            poly = hull(list(zip(xs.tolist(), ys.tolist())))
            pane = draw_coverage((W, H), lambda d, s: d.polygon([(px * s + s / 2, py * s + s / 2) for px, py in poly], fill=255))
            pane = np.maximum(pane, max_filter(core, 3) * np.clip((r - 30) / 180.0, 0, 1))
            bars = draw_coverage((W, H), lambda d, s: [d.line([(m[0] * s, m[1] * s), (m[2] * s, m[3] * s)], fill=255, width=m[4] * s)
                                                        for m in win["mullions"]])
            # The silhouette: the dark shape on the glass, without the painted mullions and the window's frame.
            # Opening by 5 px drops the 3-4 px mullions and frame edges; the pieces left over 150 px are the figure.
            inside = pane > 0.5
            shape = inside & (peak < 80)
            shape = max_filter(min_filter(shape, 5), 5) & inside
            shape = big_parts(shape, 150)
            shape = min_filter(max_filter(shape, 5), 5) & inside  # seal the cracks where light lines crossed the figure
            silhouettes[win["id"]] = (shape, (x0, y0, x1, y1))
            f = pane * (1 - bars)
            glass |= pane > 0
        hole = np.maximum(hole, f)
        windows_json.append(dict(id=win["id"], group=win["group"], x=x0, y=y0, w=x1 - x0, h=y1 - y0))

    # Glass edges keep the frame's dark colour, fading out over the hole.
    edge = glass | (hole > 0)
    out_rgb[edge] = np.array([10, 9, 16], np.float32)
    alpha = alpha * (1 - hole)
    base = np.dstack([out_rgb, alpha * 255]).clip(0, 255).astype(np.uint8)
    Image.fromarray(base, "RGBA").save(os.path.join(out, "mansion.png"), optimize=True)

    # Silhouettes, cropped to their window box, black with soft edges.
    for name, key, fill in (("dog.png", "parlor", True), ("ghost.png", "tower", False)):
        shape, (x0, y0, x1, y1) = silhouettes[key]
        if fill:
            shape = fill_holes(shape)  # the collar tag is a lit hole in the neck
        crop = shape[y0:y1, x0:x1]
        a = np.asarray(Image.fromarray(crop.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.7)))
        sil = np.zeros(crop.shape + (4,), np.uint8)
        sil[..., :3] = (14, 10, 22)
        sil[..., 3] = a
        Image.fromarray(sil, "RGBA").save(os.path.join(out, name), optimize=True)

    # Sign outlines: the hull of each sign's coloured wood, grown by 8 px so a drawn sign covers the painted one.
    signs_json = []
    lime = (g > 150) & (r > 120) & (b < 110) & (g >= r - 10)
    purple = (b > 85) & (r > 40) & (g < 45) & (b > r + 20)
    for sid, (x0, y0, x1, y1), test in SIGNS:
        m = np.zeros((H, W), bool)
        m[y0:y1, x0:x1] = (lime if test == "lime" else purple)[y0:y1, x0:x1]
        m = largest(m)
        ys, xs = np.nonzero(m)
        poly = hull(list(zip(xs.tolist(), ys.tolist())))
        cx, cy = float(np.mean([p[0] for p in poly])), float(np.mean([p[1] for p in poly]))
        grown = []
        for px, py in poly:
            dx, dy = px - cx, py - cy
            d = max(1e-3, (dx * dx + dy * dy) ** 0.5)
            grown.append((round(px + dx / d * 8, 1), round(py + dy / d * 8, 1)))
        signs_json.append(dict(id=sid, points=grown))

    with open(os.path.join(out, "mansion.json"), "w", newline="\n") as fh:
        json.dump(dict(width=W, height=H, windows=windows_json, signs=signs_json), fh, indent=1)
    print("wrote", out)


if __name__ == "__main__":
    main()
