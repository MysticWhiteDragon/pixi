#!/usr/bin/env python3
"""Draws the boot, shutdown and restart screens of the Plymouth theme in assets/plymouth/pspi.

usage: tools/make-splash.py        (needs NumPy and Pillow)
Run it after changing the look; the theme's script and .plymouth file are written by hand.
"""
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
OUT = os.path.join(ROOT, "plymouth", "pspi")
W, H = 1600, 960  # drawn large; the theme scales it to the screen


def font(name, size):
    return ImageFont.truetype(os.path.join(ROOT, "fonts", name), size)


def background():
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    t = y / H
    top, bot = np.array([33, 11, 70], np.float32), np.array([13, 6, 33], np.float32)
    img = top[None, None, :] * (1 - t[..., None]) + bot[None, None, :] * t[..., None]
    # a soft glow behind the middle, a little left of centre, like the home screen's
    d2 = ((x - W * 0.42) / (W * 0.55)) ** 2 + ((y - H * 0.42) / (H * 0.65)) ** 2
    glow = np.exp(-d2 * 1.6)[..., None] * np.array([58, 28, 118], np.float32)[None, None, :] * 0.85
    img = img + glow
    # three slow ribbons crossing the lower half
    for k, (amp, ph, base, a) in enumerate([(70, 0.0, 0.64, 0.10), (50, 1.7, 0.72, 0.07), (90, 3.1, 0.58, 0.05)]):
        wave = base * H + amp * np.sin(x / W * 2 * np.pi * 1.1 + ph)
        band = np.exp(-(((y - wave) / (22 + 6 * k)) ** 2))
        img = img + band[..., None] * np.array([150, 120, 230], np.float32)[None, None, :] * a
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def spaced(draw, xy, text, fnt, fill, gap):
    """text centred on xy, with extra space between the letters"""
    widths = [draw.textlength(c, font=fnt) for c in text]
    total = sum(widths) + gap * (len(text) - 1)
    x, y = xy[0] - total / 2, xy[1]
    for c, w in zip(text, widths):
        draw.text((x, y), c, font=fnt, fill=fill)
        x += w + gap


def wordmark(draw, cx, top, fill, h=230, t=26, w=210, gap=64):
    """An original squared "PSPi": closed, even-stroke letters built from rectangles, with a small i."""
    iw = t
    total = 3 * w + iw + 3 * gap
    x = cx - total / 2
    r = draw.rectangle

    def P(x):
        mid = top + h * 0.56
        r((x, top, x + t, top + h), fill=fill)
        r((x, top, x + w, top + t), fill=fill)
        r((x + w - t, top, x + w, mid), fill=fill)
        r((x, mid - t, x + w, mid), fill=fill)

    def S(x):
        mid = top + (h - t) / 2
        r((x, top, x + w, top + t), fill=fill)
        r((x, top, x + t, mid + t), fill=fill)
        r((x, mid, x + w, mid + t), fill=fill)
        r((x + w - t, mid, x + w, top + h), fill=fill)
        r((x, top + h - t, x + w, top + h), fill=fill)

    P(x)
    S(x + w + gap)
    P(x + 2 * (w + gap))
    xi = x + 3 * (w + gap)
    r((xi, top + h * 0.38, xi + iw, top + h), fill=fill)   # stem
    r((xi, top, xi + iw, top + iw), fill=fill)              # dot


def screen(path, line):
    img = background().convert("RGBA")
    # the wordmark, with a soft shadow under it
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    wordmark(ImageDraw.Draw(shadow), W / 2, H * 0.17 + 12, (0, 0, 0, 150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(14))
    img = Image.alpha_composite(img, shadow)
    d = ImageDraw.Draw(img)
    wordmark(d, W / 2, H * 0.17, (255, 255, 255, 255))
    spaced(d, (W / 2, H * 0.17 + 330), "RASPBERRY PI PORTABLE", font("Roboto-Medium.ttf", 42), (205, 190, 245, 235), 12)
    if line:
        spaced(d, (W / 2, H * 0.78), line, font("Roboto-Medium.ttf", 56), (255, 255, 255, 235), 3)
    img.convert("RGB").save(path, optimize=True)


def bar():
    tw, th = 640, 8
    track = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    ImageDraw.Draw(track).rounded_rectangle((0, 0, tw - 1, th - 1), th // 2, fill=(255, 255, 255, 70))
    track.save(os.path.join(OUT, "bar-track.png"))
    sw = 200
    seg = Image.new("RGBA", (sw, th), (0, 0, 0, 0))
    ImageDraw.Draw(seg).rounded_rectangle((0, 0, sw - 1, th - 1), th // 2, fill=(255, 255, 255, 235))
    seg.save(os.path.join(OUT, "bar-seg.png"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    screen(os.path.join(OUT, "boot.png"), "")
    screen(os.path.join(OUT, "shutdown.png"), "Shutting down…")
    screen(os.path.join(OUT, "reboot.png"), "Restarting…")
    bar()
    print("wrote", OUT)
