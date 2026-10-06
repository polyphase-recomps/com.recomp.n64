#!/usr/bin/env python3
"""Turn the 3DS runner's frame dumps (host/main_ctr.c, every=N) into images.

Usage: ctr_frames.py <dir with frame_*.bin> <out dir> [--sheet cols rows]

A dump is the top screen's render target as the GPU leaves it: 240 x 400 RGBA8 (bytes A B G R),
the screen turned a quarter. Images come out as the screen shows them, 400 x 240; with --sheet
the N64 picture (the middle 320 columns) of each is put on one contact sheet.
"""
import glob
import os
import sys

from PIL import Image


def convert(path):
    raw = open(path, "rb").read()
    img = Image.frombytes("RGBA", (240, 400), raw, "raw", "ABGR")
    # framebuffer column u = 240 - y, row v = 400 - x (see port_gpu_c3d.c)
    return img.transpose(Image.Transpose.ROTATE_90).convert("RGB")


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    frames = sorted(glob.glob(os.path.join(src, "frame_*.bin")))
    images = []
    for path in frames:
        img = convert(path)
        name = os.path.splitext(os.path.basename(path))[0]
        img.save(os.path.join(dst, name + ".png"))
        images.append(img)
    if "--sheet" in sys.argv and images:
        i = sys.argv.index("--sheet")
        cols, rows = int(sys.argv[i + 1]), int(sys.argv[i + 2])
        sheet = Image.new("RGB", (320 * cols, 240 * rows))
        for n, img in enumerate(images[:cols * rows]):
            sheet.paste(img.crop((40, 0, 360, 240)), ((n % cols) * 320, (n // cols) * 240))
        sheet.save(os.path.join(dst, "sheet.png"))
    print(len(images), "frames")


if __name__ == "__main__":
    main()
