#!/usr/bin/env python3
"""Compile C for the big-endian wasm guest: C -> MIPS IR -> big-endian memory -> wasm object.

N64 games read their assets straight out of the ROM: big-endian data with mixed field
sizes, used through C structs. Rather than byte-swapping every asset format, the game
code is made to see big-endian memory on any host:

  1. clang compiles the source for big-endian MIPS (o32), the N64's own ABI, to LLVM IR
     without optimisation. Struct layout, bitfield order, unions and the ABI are decided
     exactly as for the N64.
  2. Every load and store of a value wider than a byte gets an explicit byte swap
     (llvm.bswap; floats and pointers through integers), so memory holds big-endian data
     whatever the target's byte order. Global initialisers are emitted by the target in
     its own byte order; a per-file constructor swaps them once at start-up (table built
     here from each global's type, see port_be_fixup in runtime/wasm/be_fixup.c).
  3. `inbounds` is dropped from address arithmetic: decomp code reads past the end of
     arrays, which LLVM would otherwise treat as undefined behaviour and optimise away.
  4. The IR is retargeted to wasm32 (same type sizes and alignments as MIPS o32) and
     compiled with optimisation; LLVM folds the redundant swaps (locals in registers).

Varargs: the wasm backend stores variadic arguments in the target's byte order, so the
loads that fetch them (clang's MIPS va_arg sequence, pointer named %argp.cur*) stay native;
the va_list slot itself is converted to big-endian right after llvm.va_start.

Usage: wasm_be_cc.py [--native] <clang> <compile args...> -c <src> -o <obj>
       --native: no rewrite (support code that reads the fixup tables)
"""
import os
import re
import subprocess
import sys

WASM_LAYOUT = 'target datalayout = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20"'
WASM_TRIPLE = 'target triple = "wasm32-unknown-wasip1"'

SCALAR = {"i1": (1, 1), "i8": (1, 1), "i16": (2, 2), "i32": (4, 4), "i64": (8, 8), "half": (2, 2),
          "float": (4, 4), "double": (8, 8), "ptr": (4, 4)}


# ---- types and layout ----------------------------------------------------------------
class Types:
    """Layout of IR types (MIPS o32 = wasm32 sizes and alignments)."""

    def __init__(self, text):
        self.named = {}
        for m in re.finditer(r'^(%[\w.$"-]+) = type (.*)$', text, re.M):
            self.named[m.group(1)] = m.group(2).strip()
        self.cache = {}

    def parse(self, s, i=0):
        """-> (node, end). node: ('s', size) | ('a', count, node) | ('t', packed, [nodes])."""
        while s[i] == " ":
            i += 1
        if s.startswith("<{", i):
            fields, i = self.fields(s, i + 2, "}>")
            return ("t", True, fields), i
        if s[i] == "{":
            fields, i = self.fields(s, i + 1, "}")
            return ("t", False, fields), i
        if s[i] == "[":
            m = re.match(r"\[(\d+) x ", s[i:])
            elem, j = self.parse(s, i + m.end())
            assert s[j] == "]", s[i:j + 10]
            return ("a", int(m.group(1)), elem), j + 1
        if s[i] == "<":
            m = re.match(r"<(\d+) x ", s[i:])
            elem, j = self.parse(s, i + m.end())
            assert s[j] == ">"
            return ("a", int(m.group(1)), elem), j + 1
        if s[i] == "%":
            m = re.match(r'%("[^"]+"|[\w.$-]+)', s[i:])
            name = m.group(0)
            if name not in self.cache:
                body = self.named[name]
                self.cache[name] = ("s", 0, 1) if body == "opaque" else self.parse(body)[0]
            return self.cache[name], i + m.end()
        m = re.match(r"[\w]+", s[i:])
        word = m.group(0)
        if word in SCALAR:
            return ("s",) + SCALAR[word], i + m.end()
        mi = re.fullmatch(r"i(\d+)", word)
        if mi:
            n = (int(mi.group(1)) + 7) // 8
            return ("s", n, min(n, 8)), i + m.end()
        if word == "fp128":
            return ("s", 16, 16), i + m.end()
        raise ValueError("type " + s[i:i + 40])

    def fields(self, s, i, close):
        out = []
        while True:
            while s[i] == " ":
                i += 1
            if s.startswith(close, i):
                return out, i + len(close)
            node, i = self.parse(s, i)
            out.append(node)
            while s[i] == " ":
                i += 1
            if s[i] == ",":
                i += 1

    def size_align(self, node):
        kind = node[0]
        if kind == "s":
            return node[1], node[2]
        if kind == "a":
            size, align = self.size_align(node[2])
            return size * node[1], align
        packed, fields = node[1], node[2]
        off, align = 0, 1
        for f in fields:
            size, a = self.size_align(f)
            if packed:
                a = 1
            off = (off + a - 1) // a * a + size
            align = max(align, a)
        return (off + align - 1) // align * align, align

    def scalars(self, node, base, out):
        """Appends (offset, size) of every multi-byte scalar; arrays as (offset, size, count, stride)."""
        kind = node[0]
        if kind == "s":
            if node[1] > 1:
                out.append((base, node[1], 1, node[1]))
            return
        if kind == "a":
            elem = node[2]
            esize, _ = self.size_align(elem)
            if elem[0] == "s":
                if elem[1] > 1 and node[1] > 0:
                    out.append((base, elem[1], node[1], esize))
                return
            for k in range(node[1]):
                self.scalars(elem, base + k * esize, out)
            return
        packed, fields = node[1], node[2]
        off = 0
        for f in fields:
            size, a = self.size_align(f)
            if packed:
                a = 1
            off = (off + a - 1) // a * a
            self.scalars(f, base + off, out)
            off += size


# ---- operand splitting ---------------------------------------------------------------
def split_top(s, sep=","):
    """Splits s at top-level separators (not inside (), [], {}, <>, quotes)."""
    parts, depth, cur, q = [], 0, [], False
    for ch in s:
        if q:
            cur.append(ch)
            if ch == '"':
                q = False
            continue
        if ch == '"':
            q = True
        elif ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        elif ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
            continue
        cur.append(ch)
    parts.append("".join(cur))
    return [p.strip() for p in parts]


PARAM_ATTRS = {"noundef", "signext", "zeroext", "inreg", "nonnull", "noalias", "nocapture", "readonly",
               "writeonly", "returned", "nofree", "noundef", "captures(none)", "immarg"}


def param_type(param):
    """'ptr noundef %0' / 'i16 noundef signext %x' -> ('ptr', 'signext'|'zeroext'|'')."""
    words = param.split()
    ty = words[0]
    ext = "signext" if "signext" in words else ("zeroext" if "zeroext" in words else "")
    return ty, ext


def zero_of(ty):
    if ty == "ptr":
        return "null"
    if ty in ("float", "double"):
        return "0.0"
    return "0"


class Signatures:
    """Parameter and return types of the functions defined or declared in a module."""

    def __init__(self, text):
        self.sigs = {}
        for m in re.finditer(r"^(?:define|declare)\b([^@\n]*?)\s(\S+) (@[\w.$\"-]+)\((.*)\)[^(\n]*(?:\{|$)", text, re.M):
            ret, name, params = m.group(2), m.group(3), m.group(4)
            parts = split_top(params) if params.strip() else []
            if parts and parts[-1] == "...":
                continue  # variadic: calls pass what they pass
            self.sigs[name] = (ret, [param_type(p) for p in parts])


INT_OF = {"float": "i32", "double": "i64", "ptr": "i32", "i16": "i16", "i32": "i32", "i64": "i64"}


class Rewriter:
    def __init__(self, text):
        self.text = text
        self.n = 0
        self.need = set()
        self.sigs = Signatures(text)

    def tmp(self):
        self.n += 1
        return "%%be.%d" % self.n

    def swap_int(self, ity, val, lines, indent):
        """Emits code byte-reversing integer `val` of type ity; returns the new value name."""
        bits = int(ity[1:])
        if bits % 16 == 0:
            self.need.add(ity)
            t = self.tmp()
            lines.append(f"{indent}{t} = call {ity} @llvm.bswap.{ity}({ity} {val})")
            return t
        if bits == 24:
            # bytes b2 b1 b0 -> b0 b1 b2
            a, b, c, d, e = (self.tmp() for _ in range(5))
            lines.append(f"{indent}{a} = shl i24 {val}, 16")
            lines.append(f"{indent}{b} = and i24 {val}, 65280")
            lines.append(f"{indent}{c} = lshr i24 {val}, 16")
            lines.append(f"{indent}{d} = or i24 {a}, {b}")
            lines.append(f"{indent}{e} = or i24 {d}, {c}")
            return e
        raise ValueError("cannot byte-swap " + ity)

    def rewrite_load(self, m, lines):
        indent, res, vol, ty, rest = m.group(1), m.group(2), m.group(3) or "", m.group(4), m.group(5)
        ops = split_top(rest)
        ptr = ops[0]
        tail = (", " + ", ".join(ops[1:])) if len(ops) > 1 else ""
        pname = ptr.split()[-1]
        if ty in ("i8", "i1") or pname.startswith("%argp.cur"):
            lines.append(m.group(0))
            return
        if ty not in INT_OF and not re.fullmatch(r"i(16|24|32|64)", ty):
            raise ValueError("load of " + ty + ": " + m.group(0).strip())
        ity = INT_OF.get(ty, ty)
        raw = self.tmp()
        lines.append(f"{indent}{raw} = load {vol}{ity}, {ptr}{tail}")
        sw = self.swap_int(ity, raw, lines, indent)
        if ty == "ptr":
            lines.append(f"{indent}{res} = inttoptr i32 {sw} to ptr")
        elif ty in ("float", "double"):
            lines.append(f"{indent}{res} = bitcast {ity} {sw} to {ty}")
        else:
            lines.append(f"{indent}{res} = add {ity} {sw}, 0")

    def rewrite_store(self, m, lines):
        indent, vol, rest = m.group(1), m.group(2) or "", m.group(3)
        ops = split_top(rest)
        val, ptr = ops[0], ops[1]
        tail = (", " + ", ".join(ops[2:])) if len(ops) > 2 else ""
        ty, _, v = val.partition(" ")
        if ty in ("i8", "i1"):
            lines.append(m.group(0))
            return
        if ty not in INT_OF and not re.fullmatch(r"i(16|24|32|64)", ty):
            raise ValueError("store of " + ty + ": " + m.group(0).strip())
        ity = INT_OF.get(ty, ty)
        if ty == "ptr":
            iv = self.tmp()
            lines.append(f"{indent}{iv} = ptrtoint ptr {v} to i32")
        elif ty in ("float", "double"):
            iv = self.tmp()
            lines.append(f"{indent}{iv} = bitcast {ty} {v} to {ity}")
        else:
            iv = v
        sw = self.swap_int(ity, iv, lines, indent)
        lines.append(f"{indent}store {vol}{ity} {sw}, {ptr}{tail}")

    def rewrite_asm(self, line, lines):
        """Empty inline asm (scheduling barriers in matching decomps) does nothing natively."""
        m = re.match(r'(\s*)(?:(%[\w.]+) = )?(?:tail |musttail |notail )?call (\{[^}]*\}|\S+) asm (?:sideeffect )?(?:alignstack )?(?:inteldialect )?"([^"]*)", "([^"]*)"\((.*)\)(.*)$', line)
        if not m:
            raise ValueError("inline asm: " + line.strip())
        indent, res, rty, code, cons, args, _ = m.groups()
        if code.strip():
            raise ValueError("non-empty inline asm: " + line.strip())
        if res is None:
            return
        argv = split_top(args) if args.strip() else []
        outs = [c for c in cons.split(",") if c.startswith("=")]
        ins = [c for c in cons.split(",") if not c.startswith("=") and not c.startswith("~")]
        tied = []
        for k in range(len(outs)):
            idx = next((j for j, c in enumerate(ins) if c == str(k)), None)
            if idx is None:
                raise ValueError("asm output without tied input: " + line.strip())
            tied.append(argv[idx])
        if len(outs) == 1:
            t, _, v = tied[0].partition(" ")
            if t == "ptr":
                lines.append(f"{indent}{res} = getelementptr i8, ptr {v}, i32 0")
            elif t in ("float", "double"):
                lines.append(f"{indent}{res} = fadd {t} {v}, -0.0")
            else:
                lines.append(f"{indent}{res} = add {t} {v}, 0")
            return
        cur = "undef"
        for k, a in enumerate(tied):
            nxt = res if k == len(tied) - 1 else self.tmp()
            lines.append(f"{indent}{nxt} = insertvalue {rty} {cur}, {a}, {k}")
            cur = nxt

    def convert(self, val, src, dst, ext, lines, indent):
        """Emits code turning value `val` of type src into type dst (as a MIPS register would)."""
        if src == dst:
            return val
        def ibits(t):
            m = re.fullmatch(r"i(\d+)", t)
            return int(m.group(1)) if m else None
        sb, db = ibits(src), ibits(dst)
        t = self.tmp()
        if sb and db:
            op = "trunc" if sb > db else ("zext" if ext == "zeroext" else "sext")
            lines.append(f"{indent}{t} = {op} {src} {val} to {dst}")
        elif src == "ptr" and db:
            lines.append(f"{indent}{t} = ptrtoint ptr {val} to {dst}")
        elif sb and dst == "ptr":
            lines.append(f"{indent}{t} = inttoptr {src} {val} to ptr")
        elif (src, dst) in (("float", "i32"), ("i32", "float"), ("double", "i64"), ("i64", "double")):
            lines.append(f"{indent}{t} = bitcast {src} {val} to {dst}")
        else:
            return zero_of(dst)
        return t

    def rewrite_call(self, line, lines):
        """A direct call whose argument types differ from the callee's (the C code called it
        through a cast function pointer): pass what the callee takes, as on MIPS, where
        arguments are registers. Returns False if the call needs no change."""
        m = re.match(r"^(\s*)(?:(%[\w.\"-]+) = )?((?:tail |musttail |notail )?call )((?:\w+ )*?)(\S+) (@[\w.$\"-]+)\((.*)\)(.*)$", line)
        if not m:
            return False
        indent, res, call, cconv, rty, name, args, tail = m.groups()
        if name not in self.sigs.sigs or name.startswith("@llvm."):
            return False
        cret, cparams = self.sigs.sigs[name]
        argv = split_top(args) if args.strip() else []
        atypes = [a.split()[0] for a in argv]
        if atypes == [p[0] for p in cparams] and rty == cret:
            return False
        new_args = []
        for i, (pty, ext) in enumerate(cparams):
            if i < len(argv):
                aty, aval = argv[i].split()[0], argv[i].split()[-1]
                v = self.convert(aval, aty, pty, ext, lines, indent)
            else:
                v = zero_of(pty)
            attrs = " signext" if ext == "signext" else (" zeroext" if ext == "zeroext" else "")
            new_args.append(f"{pty}{attrs} {v}")
        call_line = f"{call}{cconv}{cret} {name}({', '.join(new_args)}){tail}"
        if res is None:
            lines.append(f"{indent}{call_line}")
        elif cret == "void":
            lines.append(f"{indent}{call_line}")
            lines.append(f"{indent}{res} = {'getelementptr i8, ptr null, i32 0' if rty == 'ptr' else ('fadd ' + rty + ' 0.0, 0.0' if rty in ('float', 'double') else 'add ' + rty + ' 0, 0')}")
        else:
            r = self.tmp()
            lines.append(f"{indent}{r} = {call_line}")
            v = self.convert(r, cret, rty, "", lines, indent)
            if v in ("0", "null", "0.0"):
                lines.append(f"{indent}{res} = add {rty} 0, 0")
            else:
                # name the converted value as the original result
                lines.append(f"{indent}{res} = {'getelementptr i8, ptr ' + v + ', i32 0' if rty == 'ptr' else ('fadd ' + rty + ' ' + v + ', -0.0' if rty in ('float', 'double') else 'add ' + rty + ' ' + v + ', 0')}")
        return True

    def run(self):
        out = []
        load_re = re.compile(r"^(\s*)(%[\w.\"-]+) = load (volatile )?([^,]+), (.*)$")
        store_re = re.compile(r"^(\s*)store (volatile )?(.*)$")
        for line in self.text.split("\n"):
            if line.startswith("target datalayout"):
                out.append(WASM_LAYOUT)
                continue
            if line.startswith("target triple"):
                out.append(WASM_TRIPLE)
                continue
            if line.startswith("@") and "getelementptr" in line:
                line = re.sub(r"getelementptr (?:inbounds |nuw |nusw )+", "getelementptr ", line)
            if line.startswith("attributes #"):
                line = re.sub(r' "target-(cpu|features)"="[^"]*"', "", line)
                out.append(line)
                continue
            if not line.startswith("  "):
                out.append(line)
                continue
            if "getelementptr" in line:
                # Decomps index past the end of arrays (a table of 8 read 16 times); without
                # `inbounds` LLVM cannot treat that as unreachable and drop the loop exit.
                line = re.sub(r"getelementptr (?:inbounds |nuw |nusw )+", "getelementptr ", line)
            if " load atomic " in line or line.lstrip().startswith(("atomicrmw", "cmpxchg")) or " = atomicrmw " in line or " = cmpxchg " in line:
                raise ValueError("atomic memory access: " + line.strip())
            m = load_re.match(line)
            if m:
                self.rewrite_load(m, out)
                continue
            m = store_re.match(line)
            if m:
                self.rewrite_store(m, out)
                continue
            if " asm " in line and re.search(r'call [^"]* asm ', line):
                self.rewrite_asm(line, out)
                continue
            if " call " in line or line.lstrip().startswith(("call ", "tail call ")):
                if self.rewrite_call(line, out):
                    continue
            m = re.match(r"^(\s*)call void @llvm\.va_(start)(?:\.p0)?\(ptr (%[\w.\"-]+)", line)
            out.append(line)
            if m:
                # va_start wrote the slot in the target's order: make it big-endian (va_copy
                # copies a slot that already is)
                indent, slot = m.group(1), m.group(3)
                a = self.tmp()
                out.append(f"{indent}{a} = load i32, ptr {slot}, align 4")
                b = self.swap_int("i32", a, out, indent)
                out.append(f"{indent}store i32 {b}, ptr {slot}, align 4")
        text = "\n".join(out)
        decls = []
        for ity in sorted(self.need):
            if f"declare {ity} @llvm.bswap.{ity}(" not in text:
                decls.append(f"declare {ity} @llvm.bswap.{ity}({ity})")
        return text + "\n" + "\n".join(decls) + "\n"


GLOBAL_RE = re.compile(r'^(@[\w.$"-]+) = ((?:[a-z_]+(?:\([^)]*\))? )*)(global|constant) (.*)$')


def overlay_sections(text, tag):
    """Writable globals of an overlay's source go to the data segments ovd_<tag> / ovb_<tag>,
    which wasm-ld brackets with __start_/__stop_ symbols (see gen_overlays.py)."""
    out = []
    for line in text.split("\n"):
        m = GLOBAL_RE.match(line)
        if m and m.group(3) == "global" and not m.group(1).startswith("@llvm.") and \
                not re.search(r"\b(external|extern_weak)\b", m.group(2)):
            sect = "ovb_" + tag if " zeroinitializer" in line else "ovd_" + tag
            if ', section "' in line:
                line = re.sub(r', section "[^"]*"', f', section "{sect}"', line, count=1)
            elif ", align " in line:
                line = line.replace(", align ", f', section "{sect}", align ', 1)
            else:
                line += f', section "{sect}"'
        out.append(line)
    return "\n".join(out)


def fixup_globals(text, stem):
    """Byte-swap table for every initialised global; constant globals become writable."""
    types = Types(text)
    out, entries = [], []
    for line in text.split("\n"):
        m = GLOBAL_RE.match(line)
        if not m or m.group(1).startswith("@llvm.") or re.search(r"\b(external|extern_weak)\b", m.group(2)):
            out.append(line)
            continue
        name, rest = m.group(1), m.group(4)
        try:
            node, end = types.parse(rest, 0)
        except (ValueError, KeyError, AttributeError) as e:
            raise ValueError(f"{name}: cannot lay out the type ({e})")
        init = rest[end:].strip()
        if init.startswith(("zeroinitializer", "undef", "poison")):
            out.append(line)
            continue
        runs = []
        types.scalars(node, 0, runs)
        if not runs:
            out.append(line)
            continue
        if m.group(3) == "constant":
            line = line[:m.start(3)] + "global" + line[m.end(3):]
        out.append(line)
        for off, size, count, stride in runs:
            entries.append((name, off, size, count, stride))
    if not entries:
        return "\n".join(out)
    rows = []
    for name, off, size, count, stride in entries:
        addr = f"getelementptr (i8, ptr {name}, i32 {off})" if off else f"ptr {name}"
        if off:
            addr = "ptr " + addr
        rows.append(f"{{ ptr, i32, i32 }} {{ {addr}, i32 {count}, i32 {(size << 16) | stride} }}")
    tab = f"@__befix.tab.{stem}"
    ctor = f"@__befix.ctor.{stem}"
    out.append(f"{tab} = private constant [{len(rows)} x {{ ptr, i32, i32 }}] [" + ", ".join(rows) + "]")
    out.append(f"define internal void {ctor}() {{\n  call void @port_be_fixup(ptr {tab}, i32 {len(rows)})\n  ret void\n}}")
    out.append("declare void @port_be_fixup(ptr, i32)")
    text = "\n".join(out)
    ctor_entry = f"{{ i32, ptr, ptr }} {{ i32 101, ptr {ctor}, ptr null }}"
    m = re.search(r"^@llvm\.global_ctors = appending global \[(\d+) x \{ i32, ptr, ptr \}\] \[(.*)\]$", text, re.M)
    if m:
        n = int(m.group(1)) + 1
        text = text[:m.start()] + f"@llvm.global_ctors = appending global [{n} x {{ i32, ptr, ptr }}] [{m.group(2)}, {ctor_entry}]" + text[m.end():]
    else:
        text += f"\n@llvm.global_ctors = appending global [1 x {{ i32, ptr, ptr }}] [{ctor_entry}]\n"
    return text


def main():
    args = sys.argv[1:]
    native = False
    if args and args[0] == "--native":
        native, args = True, args[1:]
    cc, args = args[0], args[1:]
    src = args[args.index("-c") + 1]
    obj = args[args.index("-o") + 1]
    ll = obj + ".ll"
    stem = re.sub(r"\W", "_", os.path.basename(obj))

    # 1. C -> IR for big-endian MIPS, unoptimised
    front = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-c",):
            continue
        if a == "-o":
            skip = True
            continue
        if a.startswith("--target=") or a.startswith("-O") or a.startswith("-m"):
            continue
        front.append(a)
    target = ["--target=wasm32-unknown-wasip1"] if native else ["--target=mips-unknown-linux-gnu", "-mabi=32"]
    cmd = [cc] + target + front + ["-O2", "-Xclang", "-disable-llvm-passes", "-fno-discard-value-names",
                                   "-S", "-emit-llvm", "-o", ll]
    r = subprocess.call(cmd)
    if r != 0:
        return r
    if not native:
        with open(ll, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        try:
            text = Rewriter(text).run()
            tag = next((m.group(1) for m in (re.search(r"ovl_(\d+)\.h", a) for a in args) if m), None)
            if tag is not None:
                text = overlay_sections(text, tag)
            text = fixup_globals(text, stem)
        except ValueError as e:
            print(f"{src}: wasm_be_cc: {e}", file=sys.stderr)
            return 1
        with open(ll, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
            f.write(text)

    # 2. IR -> optimised IR. Optimisation turns calls through cast function pointers into
    #    direct calls with the wrong signature, which the wasm backend would make traps:
    #    adapt those calls (as on MIPS, where arguments are registers) before code generation.
    opt = [a for a in args if a.startswith("-O")] or ["-O2"]
    target = ["--target=wasm32-unknown-wasip1"]
    if native:
        r = subprocess.call([cc] + target + opt[-1:] + ["-Wno-everything", "-c", ll, "-o", obj])
    else:
        ll2 = obj + ".opt.ll"
        r = subprocess.call([cc] + target + opt[-1:] + ["-Wno-everything", "-S", "-emit-llvm", ll, "-o", ll2])
        if r != 0:
            return r
        with open(ll2, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        try:
            text = adapt_calls(text)
        except ValueError as e:
            print(f"{src}: wasm_be_cc: {e}", file=sys.stderr)
            return 1
        with open(ll2, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
            f.write(text)
        # 3. -> wasm object
        r = subprocess.call([cc] + target + opt[-1:] + ["-Wno-everything", "-c", ll2, "-o", obj])
        if r == 0 and not os.environ.get("WASM_BE_KEEP_IR"):
            os.remove(ll2)
    if r == 0 and not os.environ.get("WASM_BE_KEEP_IR"):
        os.remove(ll)
    return r


def adapt_calls(text):
    """Only the call adaptation of Rewriter, on already big-endian (optimised) IR."""
    rw = Rewriter(text)
    out = []
    for line in text.split("\n"):
        if line.startswith("  ") and (" call " in line or line.lstrip().startswith(("call ", "tail call "))):
            if rw.rewrite_call(line, out):
                continue
        out.append(line)
    return "\n".join(out)


if __name__ == "__main__":
    sys.exit(main())
