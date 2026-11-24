#!/usr/bin/env python3
"""Turn LineXinBar-style glyph SVGs into signed distance field PNGs for pixi-shell.

The same measurement LineXinBar's icons::builtin_distance_field takes: the
drawing is rasterised at SUPERSAMPLE times the cell, split into inside/outside
at half coverage, measured with an exact Euclidean distance transform in both
directions, averaged down to the cell, and stored as
    stored = 0.5 + distance_in_cell_fractions / (2 * SDF_RANGE)
in an 8-bit greyscale PNG (negative distances, inside the mark, are below 0.5).
The shell's glyph shader undoes this with the same SDF_RANGE.

usage: make-sdf.py SRC_DIR OUT_DIR     (needs rsvg-convert, numpy, scipy, Pillow)
"""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt

CELL = 128
SUPERSAMPLE = 4
SDF_RANGE = 0.125

def sdf(svg_path):
    fine = CELL * SUPERSAMPLE
    with tempfile.NamedTemporaryFile(suffix=".png") as tmp:
        subprocess.run(["rsvg-convert", "-w", str(fine), "-h", str(fine), svg_path, "-o", tmp.name], check=True)
        alpha = np.asarray(Image.open(tmp.name).convert("RGBA"))[:, :, 3]
    inside = alpha >= 128
    out = distance_transform_edt(~inside)      # outside pixels: distance to the mark
    within = distance_transform_edt(inside)    # inside pixels: distance to getting out
    field = (out - within).reshape(CELL, SUPERSAMPLE, CELL, SUPERSAMPLE).mean(axis=(1, 3))
    stored = 0.5 + (field / fine) / (2.0 * SDF_RANGE)
    return Image.fromarray(np.clip(np.round(stored * 255.0), 0, 255).astype(np.uint8), "L")

def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    names = sorted(n for n in os.listdir(src) if n.endswith(".svg"))
    for n in names:
        sdf(os.path.join(src, n)).save(os.path.join(dst, n[:-4] + ".png"), optimize=True)
    print(f"{len(names)} glyphs -> {dst}")

if __name__ == "__main__":
    main()
