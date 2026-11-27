"""Cut drifting fog / cloud sprites out of a sheet.

    python tools/launcher-art/sprites.py fog   <sheet on black.png> <out_dir> <prefix>
    python tools/launcher-art/sprites.py cloud <sheet on green.png> <out_dir> <prefix>

fog:   brightness becomes alpha (soft mist), coloured a pale cool grey-blue.
cloud: the flat green is keyed out (with spill removed), the painted colours stay.
The sheet is split into horizontal bands at empty rows; each band is trimmed to its content with a small margin.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

mode, src, out, prefix = sys.argv[1:5]
os.makedirs(out, exist_ok=True)
a = np.asarray(Image.open(src).convert("RGB")).astype(np.float32)
r, g, b = a[..., 0], a[..., 1], a[..., 2]

if mode == "fog":
    lum = r * 0.299 + g * 0.587 + b * 0.114
    peak = np.percentile(lum, 99.7)
    alpha = np.clip((lum - 8) / max(1.0, peak - 8), 0, 1) ** 1.15 * 0.9
    colour = np.array([206.0, 216.0, 232.0])
    rgb = np.broadcast_to(colour, a.shape).copy()
elif mode == "wisp":  # painted wisps on black: brightness becomes alpha, the colour is un-premultiplied
    lum = np.maximum(np.maximum(r, g), b)
    peak = np.percentile(lum, 99.7)
    alpha = np.clip((lum - 10) / max(1.0, peak - 10), 0, 1) ** 1.1 * 0.92
    rgb = np.clip(a / np.maximum(lum, 1)[..., None] * peak, 0, 255)  # the hue at full strength
else:
    excess = g - np.maximum(r, b)
    alpha = 1 - np.clip((excess - 4) / 70.0, 0, 1)
    rgb = a.copy()
    rgb[..., 1] = np.minimum(g, np.minimum(r, b) + 4)  # the clouds have no green: clamp all spill out
    alpha = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(5)).filter(
        ImageFilter.GaussianBlur(1.1))).astype(np.float32) / 255  # pull the edge in a pixel, soften

rows = (alpha > 0.04).sum(axis=1) > 3
bands, y = [], 0
while y < len(rows):
    if rows[y]:
        y0 = y
        while y < len(rows) and rows[y]:
            y += 1
        if y - y0 > 12:
            bands.append((y0, y))
    else:
        y += 1
# merge bands separated by tiny gaps
merged = []
for bnd in bands:
    if merged and bnd[0] - merged[-1][1] < 6:
        merged[-1] = (merged[-1][0], bnd[1])
    else:
        merged.append(bnd)

for i, (y0, y1) in enumerate(merged, 1):
    sub = alpha[y0:y1]
    cols = np.nonzero((sub > 0.04).sum(axis=0) > 2)[0]
    x0, x1 = max(0, cols[0] - 6), min(a.shape[1], cols[-1] + 7)
    y0p, y1p = max(0, y0 - 6), min(a.shape[0], y1 + 6)
    sprite = np.dstack([rgb[y0p:y1p, x0:x1], alpha[y0p:y1p, x0:x1] * 255]).clip(0, 255).astype(np.uint8)
    # nothing may touch the sprite edge: fade the outermost 4 px
    h, w = sprite.shape[:2]
    ramp = np.ones((h, w), np.float32)
    for k in range(4):
        f = (k + 1) / 5
        ramp[k, :] = np.minimum(ramp[k, :], f)
        ramp[h - 1 - k, :] = np.minimum(ramp[h - 1 - k, :], f)
        ramp[:, k] = np.minimum(ramp[:, k], f)
        ramp[:, w - 1 - k] = np.minimum(ramp[:, w - 1 - k], f)
    sprite[..., 3] = (sprite[..., 3] * ramp).astype(np.uint8)
    path = os.path.join(out, f"{prefix}{i}.png")
    Image.fromarray(sprite, "RGBA").save(path, optimize=True)
    print(path, f"{w}x{h}")
