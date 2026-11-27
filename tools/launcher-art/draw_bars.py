"""Draw the window bars on the lanterns-off house as straight lines that run frame to frame.

    python tools/launcher-art/draw_bars.py <house concept (bars painted over)> <out.png>

The bars were measured on an earlier concept picture (art px, 1447x1087). Each line is drawn long and then clipped to the
window glass, so it ends exactly at the frame.
"""
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# (points..., width). Lines are extended a little past the glass and clipped to it.
BARS = [
    # tower: vertical and horizontal
    ([(311.5, 272), (298.8, 505)], 4.0),
    ([(244, 390.8), (358, 371.6)], 4.0),
    # parlor (centre bay): vertical, and the gently curved horizontal
    ([(217.3, 664), (205.6, 922)], 4.0),
    ([(140, 779.5), (180, 773), (213, 769.3), (240, 767.6), (268, 767)], 4.0),
    # parlor_left: sloping bar
    ([(82, 807), (149, 768.7)], 3.5),
    # parlor_side: sloping bar
    ([(274, 768.2), (329, 798.8)], 3.5),
    # diamond: vertical
    ([(1031, 316), (1031, 422)], 3.5),
    # right_tower: horizontal
    ([(1280, 402.5), (1342, 402.5)], 3.5),
    # right_arch: vertical and horizontal
    ([(1259.6, 722), (1262.2, 906)], 4.0),
    ([(1220, 808.6), (1296, 808.6)], 4.0),
    # fanlight spokes, from the hub out past the arch (clipped at the frame)
    ([(527.8, 623.5), (527.8, 701.3)], 4.0),
    ([(482.3, 637.3), (515.3, 701.3)], 4.0),
    ([(571.6, 638.1), (539.5, 701.3)], 4.0),
    ([(451.7, 671.8), (508.7, 706.3)], 4.0),
    ([(603.1, 673.5), (548.7, 706.3)], 4.0),
]
COLOUR = (108, 118, 124, 255)  # this house's cool grey-cream trim in shade

src, out = sys.argv[1], sys.argv[2]
img = Image.open(src).convert("RGBA")
W0, H0 = img.size
sx, sy = W0 / 1447, H0 / 1087
a = np.asarray(img).astype(np.float32)
r, g, b = a[..., 0], a[..., 1], a[..., 2]
glass = ((b > 18) & (b < 140) & (g < 0.5 * b) & (r < 0.5 * b + 4)) | (np.maximum(np.maximum(r, g), b) < 34)
glass = np.asarray(Image.fromarray(glass.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(5))) > 127  # reach the frame

S = 4
lines = Image.new("L", (W0 * S, H0 * S), 0)
d = ImageDraw.Draw(lines)
for pts, w in BARS:
    d.line([(x * sx * S, y * sy * S) for x, y in pts], fill=255, width=int(round(w * S)), joint="curve")
cover = np.asarray(lines.resize((W0, H0), Image.LANCZOS)).astype(np.float32) / 255
cover *= glass
col = np.array(COLOUR[:3], np.float32)
a[..., :3] = a[..., :3] * (1 - cover[..., None]) + col * cover[..., None]
Image.fromarray(a.clip(0, 255).astype(np.uint8), "RGBA").save(out)
print(out)
