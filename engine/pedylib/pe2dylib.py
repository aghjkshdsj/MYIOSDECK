#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows PE DLLs as signed iOS code: the no-JIT path for Wine's ARM64EC DLLs.

Without JIT, iOS executes only code that is signed and mapped from a file. Wine's PE
DLLs (ARM64EC) are native ARM64 code, so they can run at full speed without JIT if
each image lives inside a signed dylib with its sections at their PE offsets
(ARM64 code reaches its data PC-relatively, ADRP +-4 GB, so the layout must be kept).

  convert  DLL -> <name>.img (the image as mapped: every section at its RVA) and
           <name>.S: headers + code in __TEXT, data in __DATA right after. Linked as a dylib and signed,
           the image is executable in place; at load, App/Sources/Native/pe_dylib.c
           replaces the pages of every non-code section with writable memory (same
           address, same bytes), applies base relocations there, fills imports and
           the ARM64EC dispatch pointers. Code pages are never written.
  audit    what stands between a DLL and that scheme:
             conflict pages  16 KB iOS pages holding code AND something written at
                             load (writable section, relocation target, IAT, CHPE
                             pointers): needs a 16 KB section-aligned rebuild
             text relocs     base relocations inside code (e.g. literal pools of
                             syscall stubs): code would need writing, so must change
             x18 sites       instructions using x18 (Windows TEB; iOS clears x18):
                             Madeira patches them at load, here they must be
                             rewritten ahead of time
             tsd reads       hand-written `mrs TPIDRRO_EL0` TEB reads with a slot
                             offset patched at load (Madeira's FEX module)

Usage:
  pe2dylib.py convert DLL OUTDIR [--strict]
  pe2dylib.py audit DLL... [--md FILE] [--json FILE]
"""
import argparse
import json
import os
import struct
import sys

PAGE = 0x4000  # iOS arm64 VM page

SCN_CNT_CODE = 0x00000020
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_WRITE = 0x80000000

MACHINES = {0x8664: "x64/arm64ec", 0xAA64: "arm64", 0xA641: "arm64ec", 0xA64E: "arm64x"}


class PE:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, "rb") as f:
            self.raw = f.read()
        raw = self.raw
        if raw[:2] != b"MZ":
            raise ValueError("not a PE file (no MZ)")
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        if raw[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file (no PE signature)")
        (self.machine, nsec, _, _, _, opt_size, self.characteristics) = struct.unpack_from("<HHIIIHH", raw, pe + 4)
        opt = pe + 24
        magic = struct.unpack_from("<H", raw, opt)[0]
        if magic != 0x20B:
            raise ValueError("not PE32+")
        self.entry = struct.unpack_from("<I", raw, opt + 16)[0]
        self.image_base = struct.unpack_from("<Q", raw, opt + 24)[0]
        self.section_alignment, self.file_alignment = struct.unpack_from("<II", raw, opt + 32)
        self.size_of_image, self.size_of_headers = struct.unpack_from("<II", raw, opt + 56)
        ndirs = struct.unpack_from("<I", raw, opt + 108)[0]
        self.dirs = [struct.unpack_from("<II", raw, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        sh = opt + opt_size
        for i in range(nsec):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", raw, sh + 40 * i)
            ch = struct.unpack_from("<I", raw, sh + 40 * i + 36)[0]
            self.sections.append({
                "name": name.rstrip(b"\0").decode("latin-1"), "va": va, "vsize": vsize,
                "rsize": rsize, "rptr": rptr, "ch": ch,
                "exec": bool(ch & (SCN_MEM_EXECUTE | SCN_CNT_CODE)), "write": bool(ch & SCN_MEM_WRITE),
            })
        self.image = self._map()

    def _map(self):
        img = bytearray(self.size_of_image)
        img[:self.size_of_headers] = self.raw[:self.size_of_headers]
        for s in self.sections:
            n = min(s["rsize"], s["vsize"]) if s["vsize"] else s["rsize"]
            if n and s["rptr"]:
                img[s["va"]:s["va"] + n] = self.raw[s["rptr"]:s["rptr"] + n]
        return img

    def dir(self, i):
        return self.dirs[i] if i < len(self.dirs) else (0, 0)

    def u32(self, rva):
        return struct.unpack_from("<I", self.image, rva)[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.image, rva)[0]

    def section_at(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rsize"]):
                return s
        return None

    def relocs(self):
        """(rva, type) of every base relocation."""
        rva, size = self.dir(5)
        out, p, end = [], rva, rva + size
        while p + 8 <= end:
            page, block = struct.unpack_from("<II", self.image, p)
            if block < 8:
                break
            for i in range((block - 8) // 2):
                e = struct.unpack_from("<H", self.image, p + 8 + 2 * i)[0]
                if e >> 12:
                    out.append((page + (e & 0xFFF), e >> 12))
            p += block
        return out

    def chpe_metadata_rva(self):
        rva, size = self.dir(10)
        if not rva or size < 0xD0:
            return 0
        lc_size = self.u32(rva)
        if lc_size < 0xD0:
            return 0
        va = self.u64(rva + 0xC8)
        return va - self.image_base if va else 0

    def chpe_pointer_rvas(self):
        """RVAs the loader writes in IMAGE_ARM64EC_METADATA (the dispatch pointers)."""
        m = self.chpe_metadata_rva()
        if not m:
            return []
        f = struct.unpack_from("<29I", self.image, m)
        idx = [5, 6, 7, 8, 9, 14, 15, 18] + list(range(20, 29))  # dispatch_*, Get/SetX64Info, dispatch_fptr, helpers
        return [f[i] for i in idx if f[i]]


# --- x18 / TPIDRRO scan (ported from Madeira's ios_insn_x18_role, virtual_ios.c) ------------

def x18_role(insn):
    rn, rm, rt2 = (insn >> 5) & 31, (insn >> 16) & 31, (insn >> 10) & 31
    if 18 not in (rn, rm, rt2):
        return 0
    top8, top11 = insn >> 24, insn >> 21
    if (top8 & 0x3F) in (0x39, 0x3D, 0xB9 & 0x3F, 0xBD & 0x3F, 0xF9 & 0x3F, 0xFD & 0x3F, 0x79 & 0x3F, 0x7D & 0x3F):
        if rn == 18:
            return 1
    if (top11 & 0x1F9) == 0x1C1 and ((insn >> 10) & 3) == 2:
        if rn == 18:
            return 1
        if rm == 18:
            return 2
    if (top11 & 0x3E7) == 0x1C0 and rn == 18:
        return 1
    if (top8 & 0x3E) in (0x28, 0x2C, 0xA8 & 0x3E, 0xAC & 0x3E, 0x68, 0x6C):
        if rn == 18:
            return 1
        if rt2 == 18:
            return 3
    if (top11 & 0x7FF) in (0x150, 0x550) and rm == 18:
        return 2
    if (top8 & 0x5F) == 0x11 and rn == 18:
        return 1
    if (top8 & 0x5F) == 0x0B:
        if rn == 18:
            return 1
        if rm == 18:
            return 2
    return 0


def scan_code(pe):
    x18 = tsd = 0
    for s in pe.sections:
        if not s["exec"]:
            continue
        size = min(s["vsize"] or s["rsize"], s["rsize"]) & ~3
        words = struct.unpack_from("<%dI" % (size // 4), pe.image, s["va"])
        data = set()
        for i, w in enumerate(words):  # LDR (literal) targets are data, not code
            lit = {0x18: 1, 0x1C: 1, 0x98: 1, 0x58: 2, 0x5C: 2, 0xD8: 2, 0x9C: 4}.get(w >> 24)
            if lit:
                imm19 = ((w >> 5) & 0x7FFFF)
                if imm19 & 0x40000:
                    imm19 -= 0x80000
                t = i + imm19
                data.update(range(t, t + lit))
        for i, w in enumerate(words):
            if i in data:
                continue
            if x18_role(w):
                x18 += 1
            if (w & 0xFFFFFFE0) == 0xD53BD060 and i + 2 < len(words):
                r = w & 31
                if words[i + 1] == (0x927DF000 | (r << 5) | r) and (words[i + 2] & 0xFFC003FF) == (0xF9400000 | (r << 5) | r):
                    tsd += 1
    return x18, tsd


def audit(pe):
    pages_exec, pages_write = set(), set()
    for s in pe.sections:
        span = max(s["vsize"], s["rsize"])
        if not span:
            continue
        pages = set(range(s["va"] // PAGE, (s["va"] + span - 1) // PAGE + 1))
        (pages_exec if s["exec"] else pages_write if s["write"] else set()).update(pages)
    relocs = pe.relocs()
    text_relocs = 0
    for rva, _ in relocs:
        s = pe.section_at(rva)
        if s and s["exec"]:
            text_relocs += 1
        else:
            pages_write.add(rva // PAGE)
    iat_rva, iat_size = pe.dir(12)
    if iat_size:
        pages_write.update(range(iat_rva // PAGE, (iat_rva + iat_size - 1) // PAGE + 1))
    for rva in pe.chpe_pointer_rvas():
        pages_write.add(rva // PAGE)
    conflicts = sorted(pages_exec & pages_write)
    x18, tsd = scan_code(pe)
    misaligned = [s["name"] for s in pe.sections if s["va"] % PAGE]
    blockers = []
    if conflicts:
        blockers.append("%d code/data pages" % len(conflicts))
    if text_relocs:
        blockers.append("%d relocs in code" % text_relocs)
    if x18:
        blockers.append("%d x18 sites" % x18)
    if tsd:
        blockers.append("%d TSD reads" % tsd)
    return {
        "dll": pe.name,
        "machine": MACHINES.get(pe.machine, hex(pe.machine)),
        "arm64ec": bool(pe.chpe_metadata_rva()) or any(s["name"] == ".hexpthk" for s in pe.sections),
        "size_of_image": pe.size_of_image,
        "section_alignment": pe.section_alignment,
        "code_bytes": sum(max(s["vsize"], s["rsize"]) for s in pe.sections if s["exec"]),
        "sections": ["%s%s%s@%#x" % (s["name"], "X" if s["exec"] else "", "W" if s["write"] else "", s["va"])
                     for s in pe.sections],
        "misaligned_sections": misaligned,
        "conflict_pages": len(conflicts),
        "relocs": len(relocs),
        "text_relocs": text_relocs,
        "x18_sites": x18,
        "tsd_reads": tsd,
        "imports": len(imports(pe)),
        "ready": not blockers,
        "blockers": blockers,
    }


def imports(pe):
    rva, size = pe.dir(1)
    out, p = [], rva
    while size and p + 20 <= len(pe.image):
        ilt, _, _, name, iat = struct.unpack_from("<IIIII", pe.image, p)
        if not name:
            break
        dll = pe.image[name:pe.image.index(b"\0", name)].decode("latin-1")
        t = ilt or iat
        while True:
            v = pe.u64(t)
            if not v:
                break
            if v >> 63:
                out.append((dll, "#%d" % (v & 0xFFFF)))
            else:
                h = v & 0x7FFFFFFF
                out.append((dll, pe.image[h + 2:pe.image.index(b"\0", h + 2)].decode("latin-1")))
            t += 8
        p += 20
    return out


def cmd_convert(a):
    pe = PE(a.dll)
    r = audit(pe)
    print(json.dumps(r, indent=1))
    for d, n in imports(pe):
        print("import %s!%s" % (d, n))
    if a.strict and (r["conflict_pages"] or r["text_relocs"] or r["misaligned_sections"]):
        sys.exit("%s: not loadable from a signed dylib: %s" % (pe.name, ", ".join(r["blockers"]) or
                                                               "sections not 16 KB aligned"))
    # Split the image at the end of its last code page: headers + code go to __TEXT (r-x),
    # everything after to __DATA (rw, copy-on-write), which ld places right behind __TEXT,
    # so every section keeps its RVA. (iOS refuses to mmap over pages dyld mapped, so data
    # cannot be made writable later.)
    code_end = 0
    for s in pe.sections:
        if s["exec"] and max(s["vsize"], s["rsize"]):
            code_end = max(code_end, -(-(s["va"] + max(s["vsize"], s["rsize"])) // PAGE) * PAGE)
    for s in pe.sections:
        if not s["exec"] and max(s["vsize"], s["rsize"]) and s["va"] < code_end:
            sys.exit("%s: data section %s at %#x lies before the end of code (%#x)" % (pe.name, s["name"], s["va"], code_end))
    os.makedirs(a.outdir, exist_ok=True)
    stem = os.path.splitext(pe.name)[0]
    img = pe.image + bytes(-len(pe.image) % PAGE)
    with open(os.path.join(a.outdir, stem + ".text.img"), "wb") as f:
        f.write(img[:code_end])
    with open(os.path.join(a.outdir, stem + ".data.img"), "wb") as f:
        f.write(img[code_end:])
    with open(os.path.join(a.outdir, stem + ".S"), "w") as f:
        f.write("// Generated by engine/pedylib/pe2dylib.py from %s: the PE image as mapped.\n"
                "// Headers + code (RVA 0-%#x) in signed __TEXT, the rest in __DATA directly after.\n"
                "    .section __TEXT,__pe_image,regular,pure_instructions\n"
                "    .p2align 14\n"
                "    .globl _myiosdeck_pe_image\n"
                "_myiosdeck_pe_image:\n"
                "    .incbin \"%s.text.img\"\n"
                "    .section __DATA,__pe_data\n"
                "    .p2align 14\n"
                "    .globl _myiosdeck_pe_data\n"
                "_myiosdeck_pe_data:\n"
                "    .incbin \"%s.data.img\"\n"
                "    .globl _myiosdeck_pe_image_end\n"
                "_myiosdeck_pe_image_end:\n" % (pe.name, code_end, stem, stem))
    print("wrote %s.S: %d KB code in __TEXT, %d KB data in __DATA" % (stem, code_end >> 10, (len(img) - code_end) >> 10))


def cmd_audit(a):
    rows = []
    for p in a.dlls:
        try:
            rows.append(audit(PE(p)))
        except Exception as e:  # noqa: BLE001 - report and keep going
            rows.append({"dll": os.path.basename(p), "error": str(e), "ready": False, "blockers": [str(e)]})
    ok = [r for r in rows if "error" not in r]
    tot = lambda k: sum(r[k] for r in ok)  # noqa: E731
    lines = [
        "## Wine DLLs vs. the signed-dylib (no-JIT) scheme",
        "",
        "%d DLLs, %.1f MB of code. Ready as-is: **%d**. Section alignment: %s." % (
            len(rows), tot("code_bytes") / 1048576, sum(r["ready"] for r in rows),
            ", ".join(sorted({"%#x" % r["section_alignment"] for r in ok}))),
        "",
        "| total | conflict pages | relocs in code | x18 sites | TSD reads |",
        "|---|---|---|---|---|",
        "| all DLLs | %d | %d | %d | %d |" % (tot("conflict_pages"), tot("text_relocs"), tot("x18_sites"), tot("tsd_reads")),
        "",
        "| DLL | image | code/data pages | relocs in code | x18 sites | TSD reads | imports |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in sorted(ok, key=lambda r: -r["code_bytes"])[:40]:
        lines.append("| %s | %d KB | %d | %d | %d | %d | %d |" % (
            r["dll"], r["size_of_image"] >> 10, r["conflict_pages"], r["text_relocs"], r["x18_sites"],
            r["tsd_reads"], r["imports"]))
    for r in rows:
        if "error" in r:
            lines.append("| %s | error: %s | | | | | |" % (r["dll"], r["error"]))
    text = "\n".join(lines) + "\n"
    print(text)
    if a.md:
        with open(a.md, "a") as f:
            f.write(text)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(rows, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("dll")
    c.add_argument("outdir")
    c.add_argument("--strict", action="store_true")
    c.set_defaults(fn=cmd_convert)
    d = sub.add_parser("audit")
    d.add_argument("dlls", nargs="+")
    d.add_argument("--md")
    d.add_argument("--json")
    d.set_defaults(fn=cmd_audit)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
