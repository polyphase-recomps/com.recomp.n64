# Uncovered code inside .text input sections only (from the GNU ld map), per object.
#   python3 text_coverage.py dump.toml baserom.z64 game.map
import re
import sys
import tomllib

dump_path, rom_path, map_path = sys.argv[1:4]
rom = open(rom_path, "rb").read()
dump = tomllib.load(open(dump_path, "rb"))
secs = {s["name"]: s for s in dump["section"]}

texts, cur = [], None
for line in open(map_path, errors="replace"):
    m = re.match(r"^(\.\S+)\s", line)
    if m:
        cur = m.group(1)
        continue
    m = re.match(r"\s+\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$", line)
    if m and cur in secs:
        texts.append((cur, int(m.group(1), 16) & 0xFFFFFFFF, int(m.group(2), 16), m.group(3)))

total, rows = 0, []
for sec_name, start, size, obj in texts:
    s = secs[sec_name]
    covered = bytearray(size)
    for f in s.get("functions", []):
        a, b = max(f["vram"], start), min(f["vram"] + f["size"], start + size)
        for i in range(a - start, b - start):
            covered[i] = 1
    ro = s["rom"] + (start - s["vram"])
    code = rom[ro:ro + size]
    # uncovered, non-zero 32-bit words (zero words are alignment padding / nops between functions)
    missing = sum(4 for i in range(0, size - 3, 4) if not covered[i] and any(code[i:i + 4]))
    if missing:
        total += missing
        rows.append((missing, sec_name, obj))
rows.sort(reverse=True)
print(f"uncovered non-zero code in .text input sections: {total:#x} bytes in {len(rows)} objects")
for missing, sec_name, obj in rows[:25]:
    print(f"  {missing:#7x}  {sec_name:10s} {obj}")
