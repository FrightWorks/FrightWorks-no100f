"""Antialiases the window cut-outs of the launcher's house art, in place.

    python tools/launcher-art/smooth_glass.py [launcher/art/mansion.png] [launcher/art/mansion.json]

make_mansion_v2.py cuts the glass out with a near-hard mask, so arches, slanted panes and the bars between panes
step by whole pixels, and the launcher shows the steps. For each window box in mansion.json (a little larger), this
redraws the alpha with smooth edges: the cut-out is scaled up 4x, evened out a little (a 1 art px blur, which turns
the steps into the line or curve they approximate), cut at half, and scaled back down by averaging, so each edge
pixel gets the share of it that is frame. The lanterns are left alone: their bars are a pixel wide. The pixels that
become partly opaque take the colour of the frame next to them, so the edges don't show the painted glass as a dark
fringe. Run it after make_mansion_v2.py; running it twice changes little.

Needs numpy and Pillow.
"""
import json
import sys

import numpy as np
from PIL import Image

UP = 4       # supersampling for the smooth edge
SOFTEN = 1.0  # art px: how much the stepped edges are evened out before the cut (straightens slopes and curves)
MARGIN = 4   # art px around each window box


def fill_from_frame(rgb, solid, steps=8):
    """Gives every non-solid pixel the average colour of the solid pixels around it, spreading outward step by step."""
    rgb = rgb.copy()
    have = solid.copy()
    for _ in range(steps):
        acc = np.zeros_like(rgb)
        n = np.zeros(have.shape, np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            m = np.roll(np.roll(have, dy, 0), dx, 1)
            acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * m[..., None]
            n += m
        new = (~have) & (n > 0)
        rgb[new] = acc[new] / n[new][:, None]
        have |= new
    return rgb


def blur(a, sigma):
    """A Gaussian blur of a float image (edges extended)."""
    r = int(3 * sigma)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    a = np.pad(a, r, mode="edge")
    a = np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 0, a)
    return np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 1, a)


def smooth(img, box):
    x, y, w, h = box
    h_img, w_img = img.shape[:2]
    x0, y0 = max(0, x - MARGIN), max(0, y - MARGIN)
    x1, y1 = min(w_img, x + w + MARGIN), min(h_img, y + h + MARGIN)
    region = img[y0:y1, x0:x1]
    alpha = region[..., 3] / 255.0
    rh, rw = alpha.shape
    big = np.asarray(Image.fromarray(alpha.astype(np.float32), "F").resize((rw * UP, rh * UP), Image.BICUBIC))
    big = blur(big, SOFTEN * UP)
    cover = (big > 0.5).astype(np.float32).reshape(rh, UP, rw, UP).mean(axis=(1, 3))
    # Keep the outer two pixels as they were, so the region joins the rest of the house without a seam.
    keep = np.zeros_like(cover, bool)
    keep[:2, :] = keep[-2:, :] = keep[:, :2] = keep[:, -2:] = True
    cover = np.where(keep, alpha, cover)
    rgb = fill_from_frame(region[..., :3], alpha >= 0.97)
    region[..., :3] = np.where((alpha < 0.97)[..., None], rgb, region[..., :3])
    region[..., 3] = cover * 255


def main():
    png = sys.argv[1] if len(sys.argv) > 1 else "launcher/art/mansion.png"
    layout = sys.argv[2] if len(sys.argv) > 2 else "launcher/art/mansion.json"
    img = np.asarray(Image.open(png).convert("RGBA")).astype(np.float32)
    for win in json.load(open(layout, encoding="utf-8"))["windows"]:
        if win["id"].startswith("lantern"):
            continue
        smooth(img, (win["x"], win["y"], win["w"], win["h"]))
    Image.fromarray(img.round().clip(0, 255).astype(np.uint8), "RGBA").save(png, optimize=True)
    print("smoothed", png)


if __name__ == "__main__":
    main()
