#!/usr/bin/env python3
"""Compiler launcher for ELF targets: give an overlay's variables their own sections.

Usage (set as CMAKE_<LANG>_COMPILER_LAUNCHER):
    cc_overlay.py <objcopy> [--bss-as-data] -- <compiler> <args...>

--bss-as-data turns the overlay's zero-initialised variables into ordinary (zero-filled) data.
Needed with linker scripts that place unknown no-load sections after the end of .bss, where
the C runtime neither clears them nor keeps them out of the heap (devkitPPC / libogc: the
malloc arena starts right at the end of .bss).

Runs the compiler. If the command force-includes an overlay header (`-include .../ovl_<n>.h`,
see gen_overlays.py) the object's writable data sections are renamed to ovd_<n> / ovb_<n>, for
which the linker provides __start_ / __stop_ symbols. The runtime snapshots those ranges at
boot and restores them whenever the game loads the overlay.
"""
import re
import subprocess
import sys

DATA = (".data", ".data.rel", ".data.rel.local", ".sdata")
BSS = (".bss", ".sbss")


def main():
    split = sys.argv.index("--")
    objcopy, command = sys.argv[1], sys.argv[split + 1:]
    bss_as_data = "--bss-as-data" in sys.argv[2:split]
    result = subprocess.call(command)
    if result != 0:
        return result
    tag = None
    for arg in command:
        m = re.search(r"ovl_(\d+)\.h", arg)
        if m:
            tag = m.group(1)
    if tag is None or "-o" not in command or "-c" not in command:
        return 0
    obj = command[command.index("-o") + 1]
    renames = []
    for name in DATA:
        renames += ["--rename-section", f"{name}=ovd_{tag}"]
    for name in BSS:
        flags = ",alloc,load,contents,data" if bss_as_data else ""
        renames += ["--rename-section", f"{name}=ovb_{tag}{flags}"]
    return subprocess.call([objcopy] + renames + [obj])


if __name__ == "__main__":
    sys.exit(main())
