"""Add IDO static functions to an N64Recomp symbol dump.

IDO leaves `static` functions out of an object's ELF symbol table; they are only in the
object's ECOFF .mdebug section. GNU ld can't merge IDO .mdebug sections (it crashes), so this
reads each linked object's .mdebug directly, places its static procedures with the GNU ld map
(object .text address and output section), and inserts them into the dump.

    python3 ido_static_funcs.py dump.toml <decomp>/build/game.map <decomp> out.toml

(<decomp>: the folder the map's object paths are relative to, i.e. the decomp root)

Sizes: up to the next function of the section (global or static), or the object's .text end.
"""
import re
import struct
import sys
import tomllib

ST_PROC, ST_STATIC_PROC = 6, 14
SC_TEXT = 1


def elf_sections(data):
    """name -> (offset, size) of an ELF32 big-endian file's sections."""
    shoff, = struct.unpack_from(">I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x2E)
    heads = [struct.unpack_from(">IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    stroff = heads[shstrndx][4]
    out = {}
    for h in heads:
        name = data[stroff + h[0]:data.index(b"\0", stroff + h[0])].decode()
        out[name] = (h[4], h[5])
    return out


def mdebug_static_procs(path):
    """[(name, offset in .text, size or None)] of an IDO object's static procedures."""
    data = open(path, "rb").read()
    secs = elf_sections(data)
    if ".mdebug" not in secs:
        return []
    base, _ = secs[".mdebug"]
    # HDRR: magic, vstamp, then (count, offset) pairs; offsets are file offsets
    hdr = struct.unpack_from(">hh" + "I" * 23, data, base)
    (_magic, _vstamp, iline_max, cb_line, cb_line_off, idn_max, cb_dn_off, ipd_max, cb_pd_off,
     isym_max, cb_sym_off, iopt_max, cb_opt_off, iaux_max, cb_aux_off, iss_max, cb_ss_off,
     iss_ext_max, cb_ss_ext_off, ifd_max, cb_fd_off, crfd, cb_rfd_off) = hdr[:23]
    # IDO writes file offsets for where .mdebug sat in IDO's own output; post-processing
    # (asm-processor) moves the section without updating them. The first table follows the
    # 0x60-byte header, so the smallest offset tells the shift.
    offsets = [o for o in (cb_line_off, cb_dn_off, cb_pd_off, cb_sym_off, cb_opt_off, cb_aux_off, cb_ss_off,
                           cb_ss_ext_off, cb_fd_off, cb_rfd_off) if o]
    shift = (base + 0x60) - min(offsets) if offsets else 0
    cb_sym_off, cb_ss_off, cb_fd_off = cb_sym_off + shift, cb_ss_off + shift, cb_fd_off + shift
    if cb_fd_off + ifd_max * 72 > len(data) or cb_sym_off + isym_max * 12 > len(data):
        print(f"  skipped (unreadable .mdebug): {path}")
        return []
    procs = []
    for f in range(ifd_max):
        # FDR (72 bytes): adr, rss, issBase, cbSs, isymBase, csym, ...
        fdr = struct.unpack_from(">IiiIiI", data, cb_fd_off + f * 72)
        _adr, _rss, iss_base, _cb_ss, isym_base, csym = fdr
        if isym_base < 0 or csym < 0 or isym_base + csym > isym_max or iss_base < 0 or iss_base > iss_max:
            continue  # not a table this parser understands
        syms = []
        for i in range(isym_base, isym_base + csym):
            iss, value, bits = struct.unpack_from(">iiI", data, cb_sym_off + i * 12)
            st, sc = (bits >> 26) & 0x3F, (bits >> 21) & 0x1F
            syms.append((iss, value, st, sc))
        for k, (iss, value, st, sc) in enumerate(syms):
            if st == ST_STATIC_PROC and sc == SC_TEXT:
                at = cb_ss_off + iss_base + iss
                name = data[at:data.index(b"\0", at)].decode()
                procs.append((name, value))
    return procs


def map_text_objects(map_path):
    """[(output section, vma, size, object path)] of every .text input section."""
    out, cur = [], None
    for line in open(map_path, errors="replace"):
        m = re.match(r"^(\.\S+)\s", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"\s+\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$", line)
        if m and cur:
            out.append((cur, int(m.group(1), 16) & 0xFFFFFFFF, int(m.group(2), 16), m.group(3)))
    return out


def main():
    dump_path, map_path, root, out_path = sys.argv[1:5]
    dump = tomllib.load(open(dump_path, "rb"))
    sections = {s["name"]: s for s in dump["section"]}
    names = {f["name"] for s in dump["section"] for f in s.get("functions", [])}
    added, skipped = 0, 0
    for out_sec, vma, size, obj in map_text_objects(map_path):
        if out_sec not in sections or size == 0:
            continue
        sec = sections[out_sec]
        funcs = sec.setdefault("functions", [])
        starts = {f["vram"] for f in funcs}
        try:
            procs = mdebug_static_procs(f"{root}/{obj}" if not obj.startswith("/") else obj)
        except (struct.error, ValueError, UnicodeDecodeError) as e:
            print(f"  skipped {obj}: {e}")
            continue
        for name, offset in procs:
            vram = vma + offset
            if vram in starts:
                skipped += 1  # already known (a global at the same address)
                continue
            if name in names:
                name = f"{name}_{vram:08X}"
            funcs.append({"name": name, "vram": vram, "size": 0, "_obj_end": vma + size})
            starts.add(vram)
            names.add(name)
            added += 1
    # sizes: up to the next function start in the section, capped at the object's .text end
    for sec in dump["section"]:
        funcs = sorted(sec.get("functions", []), key=lambda f: f["vram"])
        for i, f in enumerate(funcs):
            if "_obj_end" in f:
                nxt = funcs[i + 1]["vram"] if i + 1 < len(funcs) else sec["vram"] + sec["size"]
                f["size"] = min(nxt, f.pop("_obj_end")) - f["vram"]
        sec["functions"] = funcs
    with open(out_path, "w", newline="\n") as o:
        o.write("# Autogenerated from an ELF via N64Recomp, plus IDO static functions from each object's .mdebug\n")
        for sec in dump["section"]:
            o.write(f'[[section]]\nname = "{sec["name"]}"\nrom = 0x{sec["rom"]:08X}\nvram = 0x{sec["vram"]:08X}\n'
                    f'size = 0x{sec["size"]:X}\n')
            if "relocs" in sec:
                raise SystemExit("relocs in dump: extend the writer")
            o.write("\nfunctions = [\n")
            for f in sec["functions"]:
                o.write(f'    {{ name = "{f["name"]}", vram = 0x{f["vram"]:08X}, size = 0x{f["size"]:X} }},\n')
            o.write("]\n\n")
    print(f"static functions added: {added} (already known: {skipped})")


main()
