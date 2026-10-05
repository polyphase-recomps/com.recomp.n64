#!/usr/bin/env python3
"""Turn the linked guest module (.wasm) into C for the host compilers (N64 runtime).

Adapted from com.recomp.ps1's wasm_to_c.py, without the PS1 module descriptor.

1. Trims the zero runs at the ends of every active data segment (linear memory
   starts zeroed anyway; the game's frame buffers and heaps are large BSS arrays).
2. Runs wasm2c, split into several files so they compile in parallel.
3. Makes the generated helpers follow the runtime's memory model by including
   runtime/wasm/n64w_mem_ops.h and n64w_ops.h at the right places of the generated
   -impl.h (masked addresses, no bounds checks, VR4300 division, no call_indirect
   type check).
4. Replaces the traps wasm-ld puts in place of direct calls whose signature does
   not match the callee (K&R declarations in decomps) with calls that pass the
   arguments the callee takes, as a MIPS call would.

Usage: wasm_to_c.py <wasm2c.exe> <in.wasm> <out_dir> <name> <num_outputs> [--runtime-include=<prefix>]
Writes <out_dir>/<name>_guest.h, _guest-impl.h, _guest_0.c ... and <name>_stubs.txt; the wasm2c
module is named <name>.
"""
import json
import os
import re
import subprocess
import sys


# ---- wasm binary: trim data segments ----------------------------------------------
def read_uleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, p


def read_sleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                result -= 1 << shift
            return result, p


def uleb(v):
    out = bytearray()
    while True:
        byte = v & 0x7F
        v >>= 7
        if v:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        byte = v & 0x7F
        v >>= 7
        if (v == 0 and not byte & 0x40) or (v == -1 and byte & 0x40):
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)


def to_i32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def trim_data(wasm):
    assert wasm[:8] == b"\0asm\1\0\0\0", "not a wasm module"
    out = bytearray(wasm[:8])
    p = 8
    saved = 0
    while p < len(wasm):
        sec_id = wasm[p]
        size, body = read_uleb(wasm, p + 1)
        end = body + size
        if sec_id != 11:
            out += wasm[p:end]
            p = end
            continue
        count, q = read_uleb(wasm, body)
        new = bytearray(uleb(count))
        for _ in range(count):
            flags, q = read_uleb(wasm, q)
            if flags not in (0, 2):
                # passive segment: copy as is
                n, r = read_uleb(wasm, q)
                new += uleb(flags) + wasm[q:r + n]
                q = r + n
                continue
            memidx = b""
            if flags == 2:
                mi, r = read_uleb(wasm, q)
                memidx = uleb(mi)
                q = r
            assert wasm[q] == 0x41, "data offset must be i32.const"
            offset, q = read_sleb(wasm, q + 1)
            assert wasm[q] == 0x0B
            q += 1
            n, q = read_uleb(wasm, q)
            data = wasm[q:q + n]
            q += n
            lead = len(data) - len(data.lstrip(b"\0"))
            data = data[lead:].rstrip(b"\0")
            saved += n - len(data)
            new += uleb(flags) + memidx + b"\x41" + sleb(to_i32(offset + lead)) + b"\x0B" + uleb(len(data)) + data
        out += bytes([11]) + uleb(len(new)) + new
        p = end
    return bytes(out), saved


# ---- generated C: overrides and signature-mismatch stubs -----------------------------
DECL_RE = re.compile(r"^(?:static )?(void|u32|u64|f32|f64) (w2c_\w+)\((w2c_\w+\*[^)]*)\);", re.M)
STUB_RE = re.compile(
    r"^(static )?(void|u32|u64|f32|f64) (w2c_\w+?)_signature_mismatch0x3A(\w+)\((w2c_\w+\* instance[^)]*)\) \{\n.*?^\}\n",
    re.M | re.S)


def param_types(params):
    # "w2c_ps1* instance, u32 var_p0" or "w2c_ps1*, u32" -> ["u32", ...] (instance dropped)
    parts = [x.strip() for x in params.split(",")][1:]
    return [x.split()[0] for x in parts if x]


def patch_impl(path, prefix):
    text = open(path, encoding="utf-8").read()
    anchor = "DEFINE_LOAD(i32_load,"
    assert anchor in text, "wasm2c output changed: no " + anchor
    text = text.replace(anchor, '#include "%sn64w_mem_ops.h"\n' % prefix + anchor, 1)
    m = re.search(r"^#define REM_U\(x, y\).*$", text, re.M)
    assert m, "wasm2c output changed: no REM_U"
    text = text[:m.end()] + '\n#include "%sn64w_ops.h"' % prefix + text[m.end():]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return text


def patch_header(path, prefix):
    text = open(path, encoding="utf-8").read()
    for h in ("wasm-rt.h", "wasm-rt-exceptions.h"):
        text = text.replace('#include "%s"' % h, '#include "%s%s"' % (prefix, h))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def demangle(text):
    """wasm2c name text -> the wasm name (0xHH sequences are escaped characters)."""
    return re.sub(r"0x([0-9A-F]{2})", lambda m: chr(int(m.group(1), 16)), text)


def mangle_after(name):
    """The wasm2c spelling of `name` when it follows "w2c_<module>_": an underscore is escaped
    where it would make a double underscore."""
    out = "_"
    for ch in name:
        if ch.isalnum() or (ch == "_" and out[-1] != "_"):
            out += ch
        else:
            out += "0x%02X" % ord(ch)
    return out[1:]


# Stubs the wasm backend itself makes for calls through a function pointer cast to another
# type within one object file (clang's FixFunctionBitcasts): ".L<callee>_bitcast_invalid".
BITCAST_RE = re.compile(
    r"^(static )?(void|u32|u64|f32|f64) (w2c_\w+?)_0x2EL(\w+?)_bitcast_invalid(\w*)\((w2c_\w+\* instance[^)]*)\) \{\n.*?^\}\n",
    re.M | re.S)


def adapt(prefix, wasm_name, ret, params, decls, report, label):
    """Body of a stub that calls `wasm_name` with the arguments it takes, or None."""
    names = [p.split()[1] for p in params.split(",")[1:] if p.strip()]
    types = param_types(params)
    callee = prefix + "_" + mangle_after(wasm_name)
    # wasm_link.py exports every callee; wasm2c then names the export wrapper (which
    # runs the module constructors) <callee> and the function itself <callee>_0
    if callee + "_0" in decls:
        callee += "_0"
    if callee not in decls:
        report.append(f"{label}: callee {wasm_name} not found, left as a trap")
        return None
    cret, ctypes = decls[callee]
    args = ["instance"] + [f"({t}){names[i]}" if i < len(names) else f"({t})0" for i, t in enumerate(ctypes)]
    call = f"{callee}({', '.join(args)})"
    if ret == "void":
        body = f"  {call};\n"
    elif cret == "void":
        body = f"  {call};\n  return 0;\n"
    else:
        body = f"  return ({ret}){call};\n"
    report.append(f"{label}: caller ({', '.join(types) or 'void'}) -> {ret}, callee ({', '.join(ctypes) or 'void'}) -> {cret}")
    return body


def fix_stubs(c_files, impl_text):
    decls = {name: (ret, param_types(params)) for ret, name, params in DECL_RE.findall(impl_text)}
    report = []
    for path in c_files:
        text = open(path, encoding="utf-8").read()

        def repl(m):
            static, ret, prefix, target, params = m.groups()
            # a second stub for the same callee is named "<callee>.1" (0x2E = '.')
            wasm_name = demangle(re.sub(r"0x2E\d+$", "", target))
            body = adapt(prefix, wasm_name, ret, params, decls, report, wasm_name)
            if body is None:
                return m.group(0)
            return f"{static or ''}{ret} {prefix}_signature_mismatch0x3A{target}({params}) {{\n{body}}}\n"

        def repl_bitcast(m):
            static, ret, prefix, target, suffix, params = m.groups()
            wasm_name = demangle(target)
            body = adapt(prefix, wasm_name, ret, params, decls, report, wasm_name + " (cast)")
            if body is None:
                return m.group(0)
            return f"{static or ''}{ret} {prefix}_0x2EL{target}_bitcast_invalid{suffix}({params}) {{\n{body}}}\n"

        new = STUB_RE.sub(repl, text)
        new = BITCAST_RE.sub(repl_bitcast, new)
        if new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
    return report


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
    wasm2c, src, out_dir, name, outputs = args[:5]
    prefix = opts.get("runtime-include", "")
    stem = name + "_guest"
    os.makedirs(out_dir, exist_ok=True)
    wasm, saved = trim_data(open(src, "rb").read())
    trimmed = os.path.splitext(src)[0] + ".trimmed.wasm"
    with open(trimmed, "wb") as f:
        f.write(wasm)
    for old in os.listdir(out_dir):
        if old.startswith(stem) and (old.endswith(".c") or old.endswith(".h")):
            os.remove(os.path.join(out_dir, old))
    out_c = os.path.join(out_dir, stem + ".c")
    cmd = [wasm2c, trimmed, "-n", name, "--num-outputs=" + outputs, "--disable-tail-call", "-o", out_c]
    if subprocess.call(cmd) != 0:
        sys.exit("wasm2c failed")
    patch_header(os.path.join(out_dir, stem + ".h"), prefix)
    impl_text = patch_impl(os.path.join(out_dir, stem + "-impl.h"), prefix)
    c_files = [os.path.join(out_dir, f"{stem}_{i}.c") for i in range(int(outputs))]
    report = fix_stubs(c_files, impl_text)
    with open(os.path.join(out_dir, name + "_stubs.txt"), "w", newline="\n") as f:
        f.write("\n".join(report) + "\n")
    print(f"wasm_to_c: {saved // 1024} KB of zero data trimmed, {len(report)} signature-mismatch calls adapted")


if __name__ == "__main__":
    main()
