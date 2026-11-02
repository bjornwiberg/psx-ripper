#!/usr/bin/env python3
"""Renders pkgfiles/ICON0.PNG (320x176): a CD with an arrow into a hard disk.

Usage: python3 tools/make_icon.py pkgfiles/ICON0.PNG
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H, S = 320, 176, 4          # draw at 4x and downsample for anti-aliasing
img = Image.new("RGB", (W * S, H * S))
d = ImageDraw.Draw(img)

# background: vertical gradient, deep navy to near black
for y in range(H * S):
    t = y / (H * S)
    d.line([(0, y), (W * S, y)], fill=(int(18 + 10 * (1 - t)), int(22 + 16 * (1 - t)), int(48 + 40 * (1 - t))))

# CD with a rainbow sheen
cx, cy, r = 92 * S, 80 * S, 58 * S
disc = Image.new("RGBA", img.size, (0, 0, 0, 0))
dd = ImageDraw.Draw(disc)
for a in range(0, 360, 2):
    hue = (a * 2) % 360
    c = tuple(int(200 + 55 * math.cos(math.radians(hue + k))) for k in (0, 120, 240))
    dd.pieslice([cx - r, cy - r, cx + r, cy + r], a, a + 2, fill=c + (255,))
dd.ellipse([cx - r * 0.93, cy - r * 0.93, cx + r * 0.93, cy + r * 0.93], outline=(255, 255, 255, 70), width=S)
dd.ellipse([cx - r * 0.36, cy - r * 0.36, cx + r * 0.36, cy + r * 0.36], fill=(225, 228, 235, 255))
dd.ellipse([cx - r * 0.13, cy - r * 0.13, cx + r * 0.13, cy + r * 0.13], fill=(0, 0, 0, 0))
img.paste(disc, (0, 0), disc)

# arrow
ax0, ax1, ay = 162 * S, 206 * S, 80 * S
d.rectangle([ax0, ay - 7 * S, ax1 - 14 * S, ay + 7 * S], fill=(255, 255, 255))
d.polygon([(ax1 - 16 * S, ay - 18 * S), (ax1 + 6 * S, ay), (ax1 - 16 * S, ay + 18 * S)], fill=(255, 255, 255))

# hard disk
hx, hy, hw, hh = 218 * S, 48 * S, 82 * S, 64 * S
d.rounded_rectangle([hx, hy, hx + hw, hy + hh], radius=8 * S, fill=(70, 78, 96), outline=(150, 160, 180), width=2 * S)
d.ellipse([hx + 14 * S, hy + 10 * S, hx + 58 * S, hy + 54 * S], fill=(120, 130, 150), outline=(190, 198, 214), width=2 * S)
d.ellipse([hx + 31 * S, hy + 27 * S, hx + 41 * S, hy + 37 * S], fill=(60, 66, 80))
d.line([(hx + 36 * S, hy + 32 * S), (hx + 70 * S, hy + 48 * S)], fill=(230, 232, 238), width=3 * S)
d.ellipse([hx + 68 * S, hy + hh - 14 * S, hx + 74 * S, hy + hh - 8 * S], fill=(80, 220, 120))

# title
try:
    font = ImageFont.truetype("/System/Library/Fonts/Avenir Next.ttc", 26 * S, index=2)
except OSError:
    font = ImageFont.load_default()
text = "PSX RiPPER"
tw = d.textlength(text, font=font)
d.text(((W * S - tw) / 2, 140 * S), text, font=font, fill=(255, 255, 255))

img = img.resize((W, H), Image.LANCZOS).filter(ImageFilter.UnsharpMask(1, 40, 2))
img.save(sys.argv[1], optimize=True)
print("wrote", sys.argv[1], img.size)
