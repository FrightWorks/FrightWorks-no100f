"""v2 launcher art: mansion.png (glass cut out), mansion.json (window rects + sign outlines), background.png.

    python tools/launcher-art/make_mansion_v2.py <mansion concept.png> <background concept.png> <out_dir>

Run tools/launcher-art/draw_bars.py on the house concept first: it redraws the window bars as straight lines. Then run
tools/launcher-art/smooth_glass.py on the result: it antialiases the glass cut-outs.
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 1447, 1087

# (id, group, search box x0,y0,x1,y1, kind). "navy": painted dark glass. "lit": pale lantern panes.
WINDOWS = [
    ("tower", "tower", (243, 272, 360, 503), "navy"),
    ("diamond", "attic", (962, 298, 1088, 438), "navy"),
    ("right_tower", "right_tower", (1280, 340, 1343, 472), "navy"),
    ("parlor_left", "parlor", (86, 682, 147, 932), "navy"),
    ("parlor", "parlor", (143, 664, 265, 922), "navy"),
    ("parlor_side", "parlor", (275, 672, 328, 922), "navy"),
    ("fanlight", "entrance", (444, 632, 610, 722), "navy"),
    ("lantern_left", "lanterns", (384, 770, 432, 820), "navy"),
    ("lantern_right", "lanterns", (615, 770, 665, 820), "navy"),
    ("right_arch", "right_arch", (1222, 724, 1293, 903), "navy"),
]

# The front pane and the two narrow side panes of each porch lantern (art px); the thin bars between them stay painted.
LANTERN_PANES = {
    "lantern_left": [
        [(397, 776), (421, 776), (418, 815), (400, 815)],
        [(390, 778), (394, 778), (398, 813), (395.5, 813)],
        [(423, 777), (426, 777), (422.5, 813), (420.5, 813)],
    ],
    "lantern_right": [
        [(627.5, 776), (652, 776), (649, 815), (631, 815)],
        [(622, 778), (626, 778), (630, 813), (627, 813)],
        [(654, 778), (657, 778), (653.5, 813), (651.5, 813)],
    ],
}

# The bars between the panes, kept opaque over the glass: (x0, y0, x1, y1) in art px.
LANTERN_BARS = {
    "lantern_left": [(394.6, 777, 399.4, 815.5), (422.2, 776, 418.8, 815.5)],
    "lantern_right": [(626.6, 777, 630.8, 815.5), (652.8, 776, 650.3, 815.5)],
}

SIGNS = [
    ("play", (712, 466, 1170, 614)),
    ("settings", (772, 620, 1095, 708)),
    ("mods", (767, 711, 1095, 798)),
    ("extras", (765, 796, 1095, 880)),
    ("exit", (767, 876, 1098, 964)),
]


def filt(mask, f):
    return np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).filter(f)) > 127


def components(mask):
    from collections import deque
    h, w = mask.shape
    lab = np.zeros((h, w), np.int32)
    sizes = [0]
    for y, x in zip(*np.nonzero(mask)):
        if lab[y, x]:
            continue
        n = len(sizes)
        q = deque([(y, x)])
        lab[y, x] = n
        c = 0
        while q:
            cy, cx = q.popleft()
            c += 1
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = cy + dy, cx + dx
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not lab[ny, nx]:
                    lab[ny, nx] = n
                    q.append((ny, nx))
        sizes.append(c)
    return lab, sizes


def hull(points):
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


def simplify(poly, n):
    """Visvalingam: drop the point making the smallest triangle until n remain (a hull stays convex)."""
    poly = list(poly)
    while len(poly) > n:
        best, bi = None, 0
        for i in range(len(poly)):
            a, b, c = poly[i - 1], poly[i], poly[(i + 1) % len(poly)]
            area = abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]))
            if best is None or area < best:
                best, bi = area, i
        poly.pop(bi)
    return poly


def main():
    src, bg_src, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    im = Image.open(src).convert("RGBA").resize((W, H), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32)
    rgb, alpha = a[..., :3], a[..., 3]
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    # The concept's solid areas come out at alpha ~253; make the inside fully opaque, keep the soft outer edge.
    alpha = np.where(alpha >= 240, 255, alpha)

    navy = (b > 18) & (b < 140) & (g < 0.5 * b) & (r < 0.5 * b + 4)  # glass is far bluer than the teal wall
    lit = (r > 150) & (g > 110)  # warm or white-hot lantern glass
    hole = np.zeros((H, W), np.float32)
    windows_json = []
    for wid, group, (x0, y0, x1, y1), kind in WINDOWS:
        m = np.zeros((H, W), bool)
        m[y0:y1, x0:x1] = (navy if kind == "navy" else lit)[y0:y1, x0:x1]
        if not wid.startswith("lantern"):  # lanterns keep their thin bars
            m = filt(filt(m, ImageFilter.MaxFilter(3)), ImageFilter.MinFilter(3))
        m = m & np.pad(
            np.ones((y1 - y0, x1 - x0), bool), ((y0, H - y1), (x0, W - x1)))
        sub = m[y0:y1, x0:x1]
        lab, sizes = components(sub)
        biggest = max(sizes[1:])
        keep = [i for i, s in enumerate(sizes) if i and s >= (max(300, 0.12 * biggest) if kind == "navy" else 25)]
        sub = np.isin(lab, keep)
        # Grow into the near-black shadow pixels at the glass edges and in the corners (they show as specks when lit).
        dark = (rgb[y0:y1, x0:x1].max(axis=2) < 34)
        for _ in range(0 if wid.startswith("lantern") else 4):  # lantern frames are near-black: no growth there
            grown = np.asarray(Image.fromarray(sub.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(3))) > 127
            sub = sub | (grown & dark)
        # Fill small islands left inside the glass (not touching the box edge, so mullions and the frame stay).
        ilab, isizes = components(~sub)
        border = set(np.unique(np.concatenate([ilab[0], ilab[-1], ilab[:, 0], ilab[:, -1]]))) - {0}
        small = [i for i, sz in enumerate(isizes) if i and sz < 120 and i not in border]
        sub = sub | np.isin(ilab, small)
        if wid == "fanlight":
            # A clean half-circle up to the frame's inner edge (measured), minus the light spokes and hub, so the
            # uneven dark shadow band under the arch is cut evenly.
            fcx, fcy, fr, sill = 527.0, 709.0, 74.0, 716.5
            yy_, xx_ = np.mgrid[y0:y1, x0:x1]
            inside = ((xx_ - fcx) ** 2 + (yy_ - fcy) ** 2 <= fr * fr) & (yy_ <= sill)
            spokes = (rgb[y0:y1, x0:x1].max(axis=2) > 62) & ~((b[y0:y1, x0:x1] > 18) & (g[y0:y1, x0:x1] < 0.5 * b[y0:y1, x0:x1]))
            sub = inside & ~spokes
            lab_, sizes_ = components(sub)
            sub = np.isin(lab_, [i for i, sz in enumerate(sizes_) if i and sz >= 40])
        if wid in LANTERN_PANES:
            # Lantern glass is too small and dark to find by colour, so its panes are measured by hand.
            img = Image.new("L", ((x1 - x0) * 4, (y1 - y0) * 4), 0)
            dr = ImageDraw.Draw(img)
            for poly in LANTERN_PANES[wid]:
                dr.polygon([((px - x0) * 4, (py - y0) * 4) for px, py in poly], fill=255)
            for bx0, by0, bx1, by1 in LANTERN_BARS[wid]:
                dr.line([((bx0 - x0) * 4, (by0 - y0) * 4), ((bx1 - x0) * 4, (by1 - y0) * 4)], fill=0, width=7)
            sub = np.asarray(img.resize((x1 - x0, y1 - y0), Image.BOX)) > 127
        m[:] = False
        m[y0:y1, x0:x1] = sub
        # Grow 1 px over the antialiased glass edge (not on the lanterns: their bars are 1-2 px), then soften.
        if wid not in LANTERN_PANES:  # only into dark edge pixels, so the light bars keep their width and length
            m = m | (filt(m, ImageFilter.MaxFilter(3)) & (rgb.max(axis=2) < 70))
        soft = np.asarray(Image.fromarray(m.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) / 255
        soft = np.maximum(soft, m * 0.0)
        hole = np.maximum(hole, np.where(m, 1.0, soft))
        ys, xs = np.nonzero(m)
        bx0, by0, bx1, by1 = int(xs.min()) - 1, int(ys.min()) - 1, int(xs.max()) + 2, int(ys.max()) + 2
        windows_json.append(dict(id=wid, group=group, x=bx0, y=by0, w=bx1 - bx0, h=by1 - by0))
        print(f"{wid:14s} {group:12s} x={bx0} y={by0} w={bx1 - bx0} h={by1 - by0} px={int(m.sum())}")

    alpha = alpha * (1 - hole)
    out_img = np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8)
    Image.fromarray(out_img, "RGBA").save(os.path.join(out, "mansion.png"), optimize=True)

    wood = (r > 100) & (g > 0.66 * r) & (b < 170) & (r > b + 12) & (alpha > 200)  # tan planks, not the redder ropes
    signs_json = []
    for sid, (x0, y0, x1, y1) in SIGNS:
        m = np.zeros((H, W), bool)
        m[y0:y1, x0:x1] = wood[y0:y1, x0:x1]
        m = filt(filt(m, ImageFilter.MaxFilter(5)), ImageFilter.MinFilter(5))
        m = filt(filt(m, ImageFilter.MinFilter(7)), ImageFilter.MaxFilter(7))  # drop thin rope remnants
        sub = m[y0:y1, x0:x1]
        lab, sizes = components(sub)
        big = int(np.argmax(sizes))
        ys, xs = np.nonzero(lab == big)
        poly = hull(list(zip((xs + x0).tolist(), (ys + y0).tolist())))
        poly = simplify(poly, 24)
        cx, cy = float(np.mean([p[0] for p in poly])), float(np.mean([p[1] for p in poly]))
        grown = []
        for px, py in poly:
            dx, dy = px - cx, py - cy
            d = max(1e-3, (dx * dx + dy * dy) ** 0.5)
            grown.append([round(px + dx / d * 6, 1), round(py + dy / d * 6, 1)])
        xs_, ys_ = [p[0] for p in grown], [p[1] for p in grown]
        print(f"sign {sid:9s} {min(xs_):.0f}-{max(xs_):.0f} x {min(ys_):.0f}-{max(ys_):.0f} pts={len(grown)}")
        signs_json.append(dict(id=sid, points=grown))

    with open(os.path.join(out, "mansion.json"), "w", newline="\n") as fh:
        json.dump(dict(width=W, height=H, windows=windows_json, signs=signs_json), fh, indent=1)

    Image.open(bg_src).convert("RGB").save(os.path.join(out, "background.png"), optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
