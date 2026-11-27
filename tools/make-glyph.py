#!/usr/bin/env python3
"""Turns one glyph SVG (assets/glyphs-src/NAME.svg) into its distance field (assets/glyphs/NAME.png).

The field is 128x128, one byte per pixel: 128 on the outline, lower inside the shape and higher
outside, falling or rising 7.6 per pixel of distance. The shape is rendered at 4x first, so the
edge is placed to a fraction of a pixel.

usage: tools/make-glyph.py NAME [NAME...]      (needs rsvg-convert, NumPy, SciPy and Pillow)
"""
import io
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
SIZE, SS, SLOPE = 128, 4, 7.6


def field(svg_path):
    png = subprocess.run(["rsvg-convert", "-w", str(SIZE * SS), "-h", str(SIZE * SS), svg_path],
                         capture_output=True, check=True).stdout
    alpha = np.array(Image.open(io.BytesIO(png)).convert("RGBA"))[:, :, 3] / 255.0
    inside = alpha > 0.5
    sd = np.where(inside, ndi.distance_transform_edt(inside), -ndi.distance_transform_edt(~inside)) / SS
    sd = sd.reshape(SIZE, SS, SIZE, SS).mean(axis=(1, 3))  # back to 128 px
    return np.clip(np.rint(128 - SLOPE * sd), 0, 255).astype(np.uint8)


if __name__ == "__main__":
    for name in sys.argv[1:]:
        src = os.path.join(ROOT, "glyphs-src", name + ".svg")
        dst = os.path.join(ROOT, "glyphs", name + ".png")
        Image.fromarray(field(src), "L").save(dst)
        print("wrote", dst)
