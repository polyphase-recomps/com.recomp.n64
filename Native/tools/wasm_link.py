#!/usr/bin/env python3
"""wasm-ld wrapper for the guest module.

wasm-ld replaces a direct call whose signature differs from the callee's (common
in decomps: K&R declarations, prototypes that disagree between files) with a trap
stub and may then drop the callee as unused. wasm_to_c.py turns those stubs back
into calls, so every such callee is kept: the link is repeated with each of them
exported when the first link reports mismatches.

Usage: wasm_link.py <wasm-ld> <wasm-ld args...>
"""
import re
import subprocess
import sys


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    cmd = sys.argv[1:]
    code, out = run(cmd)
    names = sorted(set(re.findall(r"function signature mismatch: (\S+)", out)))
    if code == 0 and names:
        code, out = run(cmd + [f"--export-if-defined={n}" for n in names])
    # keep the output short: one line per mismatch instead of wasm-ld's three
    lines = [l for l in out.splitlines() if not l.startswith(">>>") and "signature mismatch" not in l]
    if names:
        lines.append(f"wasm_link: {len(names)} signature mismatches (adapted by wasm_to_c.py): " + " ".join(names))
    if lines:
        print("\n".join(lines))
    return code


if __name__ == "__main__":
    sys.exit(main())
