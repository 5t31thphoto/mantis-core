#!/usr/bin/env python3
"""Check .mg files against the Mantis spec and turn them into PNGs plus one contact sheet.

    python3 tools/strider_preview/mg_sheet.py <dir> [scale]
"""
import os
import sys

import numpy as np
from PIL import Image

HDR = bytes([0x4D, 0x47, 0x30, 0x31, 0x40, 0x01, 0xF0, 0x00])


def load(path):
    data = open(path, "rb").read()
    assert len(data) == 153608, f"{path}: {len(data)} bytes"
    assert data[:8] == HDR, f"{path}: bad header {data[:8].hex()}"
    px = np.frombuffer(data[8:], dtype="<u2").reshape(240, 320)
    r = ((px >> 11) & 31) * 255 // 31
    g = ((px >> 5) & 63) * 255 // 63
    b = (px & 31) * 255 // 31
    return Image.fromarray(np.dstack([r, g, b]).astype(np.uint8))


def main():
    d = sys.argv[1]
    scale = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    files = sorted(f for f in os.listdir(d) if f.endswith(".mg"))
    imgs = []
    for f in files:
        im = load(os.path.join(d, f))
        if scale > 1:
            im = im.resize((320 * scale, 240 * scale), Image.NEAREST)
        im.save(os.path.join(d, f[:-3] + ".png"))
        imgs.append(im)
    cols = 4
    rows = (len(imgs) + cols - 1) // cols
    w, h = imgs[0].size
    sheet = Image.new("RGB", (cols * (w + 6) + 6, rows * (h + 6) + 6), (20, 20, 20))
    for i, im in enumerate(imgs):
        sheet.paste(im, (6 + (i % cols) * (w + 6), 6 + (i // cols) * (h + 6)))
    sheet.save(os.path.join(d, "sheet.png"))
    print(f"{len(files)} files OK -> {d}/sheet.png")


if __name__ == "__main__":
    main()
