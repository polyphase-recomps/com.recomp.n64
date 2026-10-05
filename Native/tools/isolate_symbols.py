#!/usr/bin/env python3
"""Rename a game library's symbols that the platform's own libraries also define.

Decomps carry libultra's C library and gu maths (sprintf, memcpy, strlen, guPerspective, ...)
and the runtime provides a few more (bzero, bcopy). Linked statically into a console
executable they collide with newlib / libogc - or, worse, the engine's own calls bind to the
game's versions. Every global symbol the archive defines that one of the system libraries
also defines is renamed to n64_<name> throughout the archive (definitions and the game's own
references alike), so the game keeps its versions and everything else gets the platform's.

Usage: isolate_symbols.py <nm> <objcopy> <archive> <system library>...
"""
import os
import subprocess
import sys
import tempfile


def defined_globals(nm, path):
    out = subprocess.run([nm, "--defined-only", "-g", path], capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        # "<address> <type> <name>"; weak symbols (W/V) give way by themselves
        if len(parts) == 3 and parts[1] not in "wWvV":
            names.add(parts[2])
    return names


def main():
    nm, objcopy, archive = sys.argv[1:4]
    system = [p for p in sys.argv[4:] if os.path.isfile(p)]
    ours = defined_globals(nm, archive)
    clashes = set()
    for lib in system:
        clashes |= ours & defined_globals(nm, lib)
    clashes = sorted(n for n in clashes if not n.startswith("n64_"))
    if not clashes:
        return
    with tempfile.NamedTemporaryFile("w", suffix=".syms", delete=False) as f:
        for name in clashes:
            f.write(f"{name} n64_{name}\n")
        mapping = f.name
    try:
        subprocess.run([objcopy, f"--redefine-syms={mapping}", archive], check=True)
    finally:
        os.unlink(mapping)
    print(f"{os.path.basename(archive)}: renamed {len(clashes)} symbol(s) the platform libraries also define: "
          + " ".join(clashes))


if __name__ == "__main__":
    main()
