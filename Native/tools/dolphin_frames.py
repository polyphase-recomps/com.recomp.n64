#!/usr/bin/env python3
"""Turn the "FRAME <frame> <row> <part> <hex>" lines of a Dolphin log (host/main_ogc.c with
sd:/n64port/dump.txt) into PPM images: dolphin_frames.py <dolphin.log> <out_dir>"""
import os
import re
import sys

frames = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"FRAME (\d+) (\d+) (\d+) ([0-9A-F]+)", line)
    if m:
        frame, row, part = int(m.group(1)), int(m.group(2)), int(m.group(3))
        frames.setdefault(frame, {})[(row, part)] = bytes.fromhex(m.group(4))
os.makedirs(sys.argv[2], exist_ok=True)
for frame, parts in sorted(frames.items()):
    data = b"".join(parts.get((row, part), b"\0" * (107 if part < 2 else 106) * 3)
                    for row in range(240) for part in range(3))
    with open(os.path.join(sys.argv[2], "frame_%05d.ppm" % frame), "wb") as f:
        f.write(b"P6\n320 240\n255\n" + data)
print(len(frames), "frames")
