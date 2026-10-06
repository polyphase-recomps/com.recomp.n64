"""Check com.recomp.n64's own GBI / ABI headers (runtime/recomp/gbi) against a decomp's.

Recomp mode builds the renderer and the audio interpreters with headers written for the runtime
(no N64 SDK headers). This compiles one small program twice, once with those headers and once
with a decomp's include/ folder, printing every constant they define and the layouts the
runtime reads, and reports any difference.

    python check_gbi.py <decomp>/include [--cc clang] [-D F3DEX_GBI_2 ...]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.normpath(os.path.join(HERE, "..", ".."))
OURS = os.path.join(NATIVE, "runtime", "recomp", "gbi")
HOST_INCLUDE = os.path.join(NATIVE, "runtime", "host", "include")  # exact-width ultratypes.h

LAYOUTS = [
    # (the whole Gfx union is not compared: an SDK-style one also holds bit-field views, which a
    # Windows compiler lays out wider; the runtime only reads the two words)
    ("sizeof(Gwords)", None), ("offsetof(Gfx, words.w0)", None), ("offsetof(Gfx, words.w1)", None),
    ("sizeof(Vtx)", None), ("offsetof(Vtx, v.ob)", None), ("offsetof(Vtx, v.flag)", None),
    ("offsetof(Vtx, v.tc)", None), ("offsetof(Vtx, v.cn)", None), ("offsetof(Vtx, n.n)", None),
    ("offsetof(Vtx, n.a)", None),
    ("sizeof(Mtx)", None),
    ("sizeof(Vp)", None), ("offsetof(Vp, vp.vscale)", None), ("offsetof(Vp, vp.vtrans)", None),
    ("sizeof(Light)", None), ("offsetof(Light, l.col)", None), ("offsetof(Light, l.colc)", None),
    ("offsetof(Light, l.dir)", None),
    ("sizeof(Acmd)", None),
    ("sizeof(OSTask)", "OSTASK"), ("offsetof(OSTask, t.data_ptr)", "OSTASK"),
    ("offsetof(OSTask, t.data_size)", "OSTASK"),
]


def our_constants():
    names = []
    for header in ("gbi.h", "abi.h", "sptask.h"):
        for line in open(os.path.join(OURS, "PR", header), encoding="utf-8"):
            m = re.match(r"\s*#define\s+([A-Z][A-Z0-9_]+)\s+\S", line)
            if m and not m.group(1).startswith("_"):
                names.append(m.group(1))
    return names


def program(names, task_layout_fixed):
    out = ["#include <stdio.h>", "#include <stddef.h>", "#include <PR/ultratypes.h>",
           "#include <PR/mbi.h>", "#include <PR/gbi.h>", "#include <PR/abi.h>", "#include <PR/sptask.h>", "int main(void) {"]
    for name in names:
        out.append(f'#ifdef {name}\n printf("{name} %lld\\n", (long long)({name}));\n'
                   f'#else\n printf("{name} (not defined)\\n");\n#endif')
    for expr, tag in LAYOUTS:
        if tag == "OSTASK" and not task_layout_fixed:
            # an SDK OSTask holds pointers: only comparable on a 32-bit build
            continue
        out.append(f' printf("{expr} %d\\n", (int)({expr}));')
    out.append(" return 0; }")
    return "\n".join(out) + "\n"


def run(cc, includes, defines, source, work, tag):
    src = os.path.join(work, f"gbi_{tag}.c")
    exe = os.path.join(work, f"gbi_{tag}.exe")
    open(src, "w").write(source)
    cmd = [cc, "-w", "-o", exe, src] + [f"-I{i}" for i in includes] + [f"-D{d}" for d in defines]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"compiling with the {tag} headers failed:\n{result.stderr[:3000]}")
    return subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("decomp_include")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("-D", dest="defines", action="append", default=["F3DEX_GBI_2", "_LANGUAGE_C", "_MIPS_SZLONG=32"])
    args = parser.parse_args()

    names = our_constants()
    with tempfile.TemporaryDirectory() as work:
        ours = run(args.cc, [OURS, HOST_INCLUDE], args.defines, program(names, False), work, "ours")
        theirs = run(args.cc, [HOST_INCLUDE, args.decomp_include], args.defines, program(names, False), work, "decomp")
    differ = [(a, b) for a, b in zip(ours, theirs) if a != b]
    for a, b in differ:
        print(f"DIFFERS  ours: {a:40s} decomp: {b}")
    print(f"{len(ours)} values compared, {len(differ)} differ")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
