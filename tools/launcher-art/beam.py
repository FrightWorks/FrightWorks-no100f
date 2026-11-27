"""Lighthouse light sprites, made procedurally.

    python tools/launcher-art/beam.py <out_dir>

beam.png  : 1400x220. A soft cone of light with its tip at the left middle (0,110), widening and fading out to the
            right. The code stretches it horizontally by cos(angle) (flipped for the left side) as the light turns.
flare.png : 256x256 radial glow, centred, for the lamp itself (pulses, and flares when the beam faces the viewer).
"""
import os
import sys

import numpy as np
from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
col = np.array([255.0, 236.0, 178.0])

W, H = 1400, 220
x = np.arange(W, dtype=np.float32)[None, :]
y = np.arange(H, dtype=np.float32)[:, None] - H / 2
half = 7 + (H / 2 - 14) * (x / W) ** 0.6  # the cone's half-width at each x
across = np.exp(-((y / half) ** 2) * 2.2)  # soft edges across the beam
along = np.exp(-x / 700) * np.clip(x / 18, 0, 1) ** 0.5  # fades with distance; no hard tip
# The far end fades out over the outer 65% of the length (smoothstep), so there's no visible end even when the code
# squeezes the beam short as it turns.
k = np.clip((x - 0.35 * W) / (0.65 * W), 0, 1)
alpha = across * along * (1 - k * k * (3 - 2 * k))
rgb = np.broadcast_to(col, (H, W, 3))
Image.fromarray(np.dstack([rgb, alpha * 255]).clip(0, 255).astype(np.uint8), "RGBA").save(os.path.join(out, "beam.png"), optimize=True)

S = 256
yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) - (S - 1) / 2
r = np.hypot(xx, yy) / (S / 2)
a = np.clip(np.exp(-(r ** 2) * 5) * 1.0 + np.exp(-(r ** 2) * 40) * 0.8, 0, 1) * np.clip((1 - r) * 4, 0, 1)
fl = np.dstack([np.broadcast_to(col, (S, S, 3)), a * 255]).clip(0, 255).astype(np.uint8)
Image.fromarray(fl, "RGBA").save(os.path.join(out, "flare.png"), optimize=True)
print("beam.png 1400x220, flare.png 256x256")
