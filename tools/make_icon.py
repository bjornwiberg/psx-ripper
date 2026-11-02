#!/usr/bin/env python3
"""Renders pkgfiles/ICON0.PNG (320x176), the app's icon in the XMB: a black PS1 disc, an arrow,
and the .bin and .cue files in the PSXISO folder, with the name under them. Own artwork; the
text is DejaVu Sans Bold from fonts/.

Usage: python3 tools/make_icon.py pkgfiles/ICON0.PNG
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H, S = 320, 176, 4          # drawn at 4x and downsampled for anti-aliasing
FB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts", "DejaVuSans-Bold.ttf")


def background():
    img = Image.new("RGB", (W * S, H * S))
    d = ImageDraw.Draw(img)
    for y in range(H * S):   # deep navy to near black, as the app's screens
        t = y / (H * S)
        d.line([(0, y), (W * S, y)], fill=(int(18 + 10 * (1 - t)), int(22 + 16 * (1 - t)), int(48 + 40 * (1 - t))))
    return img, d


def black_disc(img, cx, cy, r):
    """The black data side of a PS1 disc: smooth black with a soft blue-violet reflection."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    dd = ImageDraw.Draw(layer)
    dd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(12, 12, 18, 255))
    light = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(light)
    ld.ellipse([cx - r * 0.95, cy - r * 1.05, cx + r * 0.15, cy - r * 0.05], fill=(110, 120, 190, 120))
    ld.ellipse([cx + r * 0.05, cy + r * 0.10, cx + r * 0.90, cy + r * 0.90], fill=(150, 90, 170, 70))
    light = light.filter(ImageFilter.GaussianBlur(r * 0.28))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    light.putalpha(Image.composite(light.split()[3], Image.new("L", img.size, 0), mask))   # only on the disc
    layer.alpha_composite(light)
    dd = ImageDraw.Draw(layer)
    # the hub is black too: a darker edge shows where it is, and the hole
    dd.ellipse([cx - r * 0.36, cy - r * 0.36, cx + r * 0.36, cy + r * 0.36], fill=(14, 14, 20, 255), outline=(4, 4, 6, 255), width=2 * S)
    dd.ellipse([cx - r * 0.13, cy - r * 0.13, cx + r * 0.13, cy + r * 0.13], outline=(4, 4, 6, 255), width=2 * S)
    dd.ellipse([cx - r * 0.13, cy - r * 0.13, cx + r * 0.13, cy + r * 0.13], fill=(0, 0, 0, 0))
    img.paste(layer, (0, 0), layer)


def arrow(d, x0, x1, y):
    d.rectangle([x0, y - 7 * S, x1 - 14 * S, y + 7 * S], fill=(255, 255, 255))
    d.polygon([(x1 - 16 * S, y - 18 * S), (x1 + 6 * S, y), (x1 - 16 * S, y + 18 * S)], fill=(255, 255, 255))


def file_page(d, x, y, w, h, label, col):
    fold = 12 * S
    d.polygon([(x, y), (x + w - fold, y), (x + w, y + fold), (x + w, y + h), (x, y + h)], fill=(236, 240, 248), outline=(150, 158, 178))
    d.polygon([(x + w - fold, y), (x + w - fold, y + fold), (x + w, y + fold)], fill=(180, 188, 206))
    d.rounded_rectangle([x + 7 * S, y + 9 * S, x + w - 16 * S, y + 13 * S], radius=2 * S, fill=(170, 178, 196))
    f = ImageFont.truetype(FB, 12 * S)
    tw = d.textlength(label, font=f)
    d.rounded_rectangle([x + 5 * S, y + 22 * S, x + w - 5 * S, y + 38 * S], radius=3 * S, fill=col)
    d.text((x + (w - tw) / 2, y + 22 * S), label, font=f, fill=(255, 255, 255))


def folder(d, fx, fy, fw, fh):
    d.rounded_rectangle([fx, fy + 6 * S, fx + 40 * S, fy + 24 * S], radius=5 * S, fill=(214, 160, 58))   # tab
    d.rounded_rectangle([fx, fy + 16 * S, fx + fw, fy + fh], radius=7 * S, fill=(232, 178, 72))          # back
    file_page(d, fx + 12 * S, fy, 40 * S, 56 * S, "BIN", (61, 111, 217))                                 # the rip's files
    file_page(d, fx + 50 * S, fy + 6 * S, 40 * S, 56 * S, "CUE", (70, 150, 96))
    d.rounded_rectangle([fx, fy + 44 * S, fx + fw, fy + fh], radius=7 * S, fill=(250, 208, 110))       # front
    f = ImageFont.truetype(FB, 14 * S)
    tw = d.textlength("PSXISO", font=f)
    d.text((fx + (fw - tw) / 2, fy + 60 * S), "PSXISO", font=f, fill=(120, 80, 20))


def render(path):
    img, d = background()
    black_disc(img, 88 * S, 74 * S, 56 * S)
    arrow(d, 154 * S, 196 * S, 74 * S)
    folder(d, 210 * S, 24 * S, 100 * S, 92 * S)
    f = ImageFont.truetype(FB, 25 * S)
    name = "PSX RiPPER"
    d.text(((W * S - d.textlength(name, font=f)) / 2, 138 * S), name, font=f, fill=(255, 255, 255))
    img = img.resize((W, H), Image.LANCZOS).filter(ImageFilter.UnsharpMask(1, 40, 2))
    img.save(path, optimize=True)


if __name__ == "__main__":
    render(sys.argv[1])
    print("wrote", sys.argv[1])
