#!/usr/bin/env python3
"""Cut the asset data out of an N64 ROM into a pack file the runtime loads instead of the ROM.

The pack holds only the ROM ranges named on the command line (asset files, particle banks,
audio banks, ...), addressed by their original ROM offsets, so the game's own loaders work
unchanged. It contains none of the game's code and is not a ROM.

Layout (all integers big-endian):
    0x00  "N64PAK1\\0"
    0x08  u32 size of the original ROM
    0x0C  u32 number of ranges
    0x10  per range: u32 ROM offset, u32 size, u32 offset of the data in this file
    ...   data

Usage:
    make_rom_pack.py <rom.z64> <out.n64pak> --range START:END [--range START:END ...]
    make_rom_pack.py <rom.z64> <out.n64pak> --yaml <splat.yaml> --from SEGMENT --to SEGMENT
        (everything from the start of segment FROM up to the start of segment TO)
"""
import argparse
import os
import re
import struct


def yaml_starts(path):
    """{segment name: ROM start} for a splat yaml, both entry spellings."""
    starts, name = {}, None
    with open(path) as f:
        for line in f:
            m = re.match(r"  - \[\s*(0x[0-9A-Fa-f]+)\s*,\s*[\w.]+\s*,\s*([\w.]+)", line)
            if m:
                starts[m.group(2)] = int(m.group(1), 16)
                continue
            m = re.match(r"  - name:\s*([\w.]+)", line)
            if m:
                name = m.group(1)
                continue
            m = re.match(r"    start:\s*(0x[0-9A-Fa-f]+)", line)
            if m and name is not None:
                starts[name] = int(m.group(1), 16)
                name = None
    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--range", action="append", default=[], help="START:END (hex ROM offsets)")
    ap.add_argument("--yaml")
    ap.add_argument("--from", dest="first")
    ap.add_argument("--to", dest="last")
    opts = ap.parse_args()

    with open(opts.rom, "rb") as f:
        rom = f.read()
    if rom[:2] != b"\x80\x37":
        raise SystemExit(f"{opts.rom} is not a big-endian (.z64) N64 ROM")

    ranges = []
    for text in opts.range:
        start, end = (int(x, 16) for x in text.split(":"))
        ranges.append((start, end))
    if opts.yaml:
        starts = yaml_starts(opts.yaml)
        for key in (opts.first, opts.last):
            if key not in starts:
                raise SystemExit(f"segment '{key}' not found in {opts.yaml}")
        ranges.append((starts[opts.first], starts[opts.last]))
    if not ranges:
        raise SystemExit("no ranges given")
    ranges.sort()
    for start, end in ranges:
        if not (0 <= start < end <= len(rom)):
            raise SystemExit(f"range 0x{start:X}:0x{end:X} is outside the ROM")

    header_size = 16 + 12 * len(ranges)
    table, data, offset = b"", b"", header_size
    for start, end in ranges:
        table += struct.pack(">III", start, end - start, offset)
        data += rom[start:end]
        offset += end - start
    blob = b"N64PAK1\0" + struct.pack(">II", len(rom), len(ranges)) + table + data

    try:
        with open(opts.out, "rb") as f:
            if f.read() == blob:
                return
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(opts.out)), exist_ok=True)
    with open(opts.out, "wb") as f:
        f.write(blob)
    print(f"{opts.out}: {len(ranges)} range(s), {len(blob) / 1048576:.1f} MB")


if __name__ == "__main__":
    main()
