#!/usr/bin/env python3
"""Renders pkgfiles/PIC1.PNG (1920x1080), the background the XMB shows behind the app's icon.

Own artwork only: a dark navy gradient with a large CD whose rainbow sheen fades into the
background on the right, and the tracks of a disc as thin rings. The XMB draws the title and the
icon on top, so there is no text here.

Usage: python3 tools/make_background.py pkgfiles/PIC1.PNG [variant]
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter

W, H = 1920, 1080
variant = sys.argv[2] if len(sys.argv) > 2 else "a"

img = Image.new("RGB", (W, H))
d = ImageDraw.Draw(img)
for y in range(H):   # the app's own backdrop colours: deep navy at the top to near black
    t = y / H
    d.line([(0, y), (W, y)], fill=(int(29 - 17 * t), int(33 - 20 * t), int(72 - 46 * t)))

# the disc: centre off to the right, partly outside the picture
cx, cy, r = (1500, 560, 640) if variant == "a" else (1650, 1150, 900)
disc = Image.new("RGBA", (W, H), (0, 0, 0, 0))
dd = ImageDraw.Draw(disc)
for a in range(0, 360, 1):   # rainbow sheen, faint
    hue = (a * 2) % 360
    c = tuple(int(150 + 70 * math.cos(math.radians(hue + k))) for k in (0, 120, 240))
    dd.pieslice([cx - r, cy - r, cx + r, cy + r], a, a + 1.2, fill=c + (46,))
for k in range(28):   # tracks
    rr = r * (0.40 + 0.58 * k / 27)
    dd.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=(255, 255, 255, 10 + (k % 3) * 4), width=2)
hub = r * 0.36
dd.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=(12, 14, 32, 230))
dd.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], outline=(255, 255, 255, 40), width=3)
hole = r * 0.12
dd.ellipse([cx - hole, cy - hole, cx + hole, cy + hole], fill=(8, 9, 20, 255))
disc = disc.filter(ImageFilter.GaussianBlur(1.2))

# fade the disc out towards the left, where the XMB lists the items
mask = Image.new("L", (W, H))
md = ImageDraw.Draw(mask)
for x in range(W):
    md.line([(x, 0), (x, H)], fill=int(255 * max(0.0, min(1.0, (x - 500) / 900))))
alpha = Image.composite(disc.split()[3], Image.new("L", (W, H)), mask)
disc.putalpha(alpha)
img.paste(disc, (0, 0), disc)

# a soft glow behind the disc
glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([cx - r * 1.1, cy - r * 1.1, cx + r * 1.1, cy + r * 1.1], fill=(90, 110, 220, 26))
img.paste(glow.filter(ImageFilter.GaussianBlur(120)), (0, 0), glow.filter(ImageFilter.GaussianBlur(120)))

img.save(sys.argv[1], optimize=True)
print("wrote", sys.argv[1], img.size)
