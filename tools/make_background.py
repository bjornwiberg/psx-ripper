#!/usr/bin/env python3
"""Renders pkgfiles/PIC1.PNG (1920x1080), the background the XMB shows behind the app's icon: the
same deep navy gradient as the app's own screens (ui_init() in source/ui.cpp), so the XMB and the
app look alike.

Usage: python3 tools/make_background.py pkgfiles/PIC1.PNG
"""
import sys

from PIL import Image, ImageDraw

W, H = 1920, 1080


def render(path):
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=(28 - int(14 * t), 34 - int(18 * t), 72 - int(40 * t)))
    img.save(path, optimize=True)


if __name__ == "__main__":
    render(sys.argv[1])
    print("wrote", sys.argv[1])
