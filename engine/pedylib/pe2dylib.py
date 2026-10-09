#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows PE DLLs as signed iOS code: the no-JIT path for Wine's ARM64EC DLLs.

Without JIT, iOS executes only code that is signed and mapped from a file. Wine's PE
DLLs (ARM64EC) are native ARM64 code, so they run at full speed without JIT if each
image lives inside a signed dylib with its sections at their PE offsets (ARM64 code
reaches its data PC-relatively, so the layout must be kept). Proven on device in
build 43 (docs/NO_JIT_WINDOWS.md).

  convert  DLL -> <name>.S + <name>.{tramp,text,data}.img for engine/pedylib/link-dylibs.sh:
             __TEXT  [x18 trampolines][PE headers + code, up to the last code page]
             __DATA  [the rest of the image][TEB slot offset word]
           ld puts __DATA right behind __TEXT, so every section keeps its RVA. iOS
           refuses to mmap over pages dyld mapped (build 42), so anything written at
           load (writable sections, relocation targets, IAT, CHPE pointers) must be in
           __DATA. Instructions using x18 are rewritten here (see rewrite_x18).
  audit    what stands between a DLL and that scheme:
             conflict pages  16 KB pages holding code AND something written at load
             text relocs     base relocations inside code: code would need writing (literal
                             pools of absolute addresses, as in Wine's aarch64 syscall thunks,
                             are made position-independent first: rewrite_code_literals)
             x18 sites       instructions using x18 (the Windows TEB register, which iOS
                             clears) in ARM64 code; convert rewrites them
             x18 unpatchable sites the rewrite cannot handle (sp-based, no free register)
             tsd reads       hand-written `mrs TPIDRRO_EL0` TEB reads with a slot offset
                             patched at load (Madeira's FEX module only)

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

# Byte offset of the TEB pointer from TPIDRRO_EL0 & ~7 (the thread's TSD array): the
# loader overwrites the default with the slot of its own pthread key.
TEB_TSD_OFFSET_DEFAULT = 0x898


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
        if struct.unpack_from("<H", raw, opt)[0] != 0x20B:
            raise ValueError("not PE32+")
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
                "name": name.rstrip(b"\0").decode("latin-1"), "va": va, "span": max(vsize, rsize),
                "vsize": vsize, "rsize": rsize, "rptr": rptr, "ch": ch,
                "exec": bool(ch & (SCN_MEM_EXECUTE | SCN_CNT_CODE)), "write": bool(ch & SCN_MEM_WRITE),
            })
        self.image = bytearray(self.size_of_image)
        self.dead_literals = set()   # code words that are data no instruction reads any more
        self.image[:self.size_of_headers] = raw[:self.size_of_headers]
        for s in self.sections:
            n = min(s["rsize"], s["vsize"]) if s["vsize"] else s["rsize"]
            if n and s["rptr"]:
                self.image[s["va"]:s["va"] + n] = raw[s["rptr"]:s["rptr"] + n]

    def dir(self, i):
        return self.dirs[i] if i < len(self.dirs) else (0, 0)

    def u32(self, rva):
        return struct.unpack_from("<I", self.image, rva)[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.image, rva)[0]

    def section_at(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + s["span"]:
                return s
        return None

    def relocs(self):
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
        if not rva or size < 0xD0 or self.u32(rva) < 0xD0:
            return 0
        va = self.u64(rva + 0xC8)
        return va - self.image_base if va else 0

    def chpe_fields(self):
        m = self.chpe_metadata_rva()
        return struct.unpack_from("<29I", self.image, m) if m else None

    def chpe_pointer_rvas(self):
        """RVAs the loader writes in IMAGE_ARM64EC_METADATA (the dispatch pointers)."""
        f = self.chpe_fields()
        if not f:
            return []
        return [f[i] for i in [5, 6, 7, 8, 9, 14, 15, 18] + list(range(23, 29)) if f[i]]

    def arm64_ranges(self):
        """[start, end) RVAs of ARM64/ARM64EC code: the CHPE code map when there is one
        (x64 ranges, like the .hexpthk thunks, excluded), else every code section."""
        f = self.chpe_fields()
        if f and f[1] and f[2]:
            out = []
            for i in range(f[2]):
                start, length = struct.unpack_from("<II", self.image, f[1] + 8 * i)
                if start & 3 in (0, 1):
                    out.append((start & ~3, (start & ~3) + length))
            return out
        return [(s["va"], s["va"] + min(s["span"], s["rsize"] or s["span"])) for s in self.sections if s["exec"]]

    def imports(self):
        rva, size = self.dir(1)
        out, p = [], rva
        while size and p + 20 <= len(self.image):
            ilt, _, _, name, iat = struct.unpack_from("<IIIII", self.image, p)
            if not name:
                break
            dll = self.image[name:self.image.index(b"\0", name)].decode("latin-1")
            t = ilt or iat
            while self.u64(t):
                v = self.u64(t)
                h = v & 0x7FFFFFFF
                out.append((dll, "#%d" % (v & 0xFFFF) if v >> 63 else
                            self.image[h + 2:self.image.index(b"\0", h + 2)].decode("latin-1")))
                t += 8
            p += 20
        return out


def pages_of(rva, span):
    return range(rva // PAGE, (rva + max(span, 1) - 1) // PAGE + 1)


def page_sets(pe):
    """(code pages, pages written at load, relocations inside code)."""
    code, write = set(), set()
    for s in pe.sections:
        if s["span"]:
            if s["exec"]:
                code.update(pages_of(s["va"], s["span"]))
            elif s["write"]:
                write.update(pages_of(s["va"], s["span"]))
    text_relocs = 0
    for rva, _ in pe.relocs():
        s = pe.section_at(rva)
        if s and s["exec"]:
            text_relocs += 1
        write.add(rva // PAGE)
    iat_rva, iat_size = pe.dir(12)
    if iat_size:
        write.update(pages_of(iat_rva, iat_size))
    for rva in pe.chpe_pointer_rvas():
        write.add(rva // PAGE)
    return code, write, text_relocs


# --- x18 (the Windows TEB register; iOS does not preserve it) -------------------------------
# Classifier ported from Madeira's ios_insn_x18_role (build/ntdll-unix/virtual_ios.c).
ROLE_RN, ROLE_RM, ROLE_RT2 = 1, 2, 3


def x18_role(insn):
    rn, rm, rt2 = (insn >> 5) & 31, (insn >> 16) & 31, (insn >> 10) & 31
    if 18 not in (rn, rm, rt2):
        return 0
    top8, top11 = insn >> 24, insn >> 21
    if (top8 & 0x3F) in (0x39, 0x3D) and rn == 18:      # LDR/STR (unsigned immediate)
        return ROLE_RN
    if (top11 & 0x1F9) == 0x1C1 and ((insn >> 10) & 3) == 2:  # LDR/STR (register offset)
        if rn == 18:
            return ROLE_RN
        if rm == 18:
            return ROLE_RM
    if (top11 & 0x3E7) == 0x1C0 and rn == 18:           # LDR/STR (pre/post-index, unscaled)
        return ROLE_RN
    if (top8 & 0x3E) in (0x28, 0x2C):                   # LDP/STP
        if rn == 18:
            return ROLE_RN
        # Rt2 is a general register only when V (bit 26) is clear: `stp q21, q18, [sp]` stores
        # vector register 18, not x18 (Madeira's classifier takes it for x18 too; the rewrite
        # then put a scratch GPR's number in the vector field).
        if rt2 == 18 and not (insn >> 26) & 1:
            return ROLE_RT2
    if (top11 & 0x7FF) in (0x150, 0x550) and rm == 18:  # MOV (ORR Rd, ZR, Rm)
        return ROLE_RM
    if (top8 & 0x5F) == 0x11 and rn == 18:              # ADD/SUB (immediate)
        return ROLE_RN
    if (top8 & 0x5F) == 0x0B:                           # ADD/SUB (register)
        if rn == 18:
            return ROLE_RN
        if rm == 18:
            return ROLE_RM
    return 0


def replace_field(insn, role, reg):
    shift = {ROLE_RN: 5, ROLE_RM: 16, ROLE_RT2: 10}[role]
    return (insn & ~(31 << shift)) | (reg << shift)


def literal_words(words):
    """Word indices that are LDR (literal) targets: data inside code, never patched."""
    data = set()
    for i, w in enumerate(words):
        lit = {0x18: 1, 0x1C: 1, 0x98: 1, 0x58: 2, 0x5C: 2, 0xD8: 2, 0x9C: 4}.get(w >> 24)
        if lit:
            imm19 = (w >> 5) & 0x7FFFF
            if imm19 & 0x40000:
                imm19 -= 0x80000
            data.update(range(i + imm19, i + imm19 + lit))
    return data


def x18_sites(pe):
    """[(rva, insn, role)] of x18 uses in ARM64 code, and the TSD-read triplet count."""
    sites, tsd = [], 0
    for start, end in pe.arm64_ranges():
        n = (end - start) // 4
        words = struct.unpack_from("<%dI" % n, pe.image, start)
        data = literal_words(words)
        for i, w in enumerate(words):
            if i in data or start + 4 * i in pe.dead_literals:
                continue
            role = x18_role(w)
            if role:
                sites.append((start + 4 * i, w, role))
            if (w & 0xFFFFFFE0) == 0xD53BD060 and i + 2 < n:
                r = w & 31
                if words[i + 1] == (0x927DF000 | (r << 5) | r) and (words[i + 2] & 0xFFC003FF) == (0xF9400000 | (r << 5) | r):
                    tsd += 1
    return sites, tsd


def b(frm, to):
    d = to - frm
    assert d % 4 == 0 and -(1 << 27) <= d < (1 << 27), "branch out of range"
    return 0x14000000 | ((d >> 2) & 0x3FFFFFF)


# --- absolute addresses in code (Wine's aarch64 syscall thunks) -------------------------------
def reloc_entries(pe):
    """[(entry offset in the image, target rva, type)] of the base relocations."""
    rva, size = pe.dir(5)
    out, p, end = [], rva, rva + size
    while p + 8 <= end:
        page, block = struct.unpack_from("<II", pe.image, p)
        if block < 8:
            break
        for i in range((block - 8) // 2):
            e = struct.unpack_from("<H", pe.image, p + 8 + 2 * i)[0]
            if e >> 12:
                out.append((p + 8 + 2 * i, page + (e & 0xFFF), e >> 12))
        p += block
    return out


def rewrite_code_literals(pe):
    """Position-independent loads for addresses kept in code. Wine's aarch64 thunks (every
    syscall stub in ntdll and win32u) read an absolute address from a literal pool:
        ldr x16, 1f ; ldr x16, [x16] ; blr x16 ; ... 1: .quad &__wine_syscall_dispatcher
    The literal needs a base relocation, i.e. a write to a signed code page, which iOS refuses.
    Each `ldr Xt, literal` becomes `adr Xt, target` (within 1 MB), or `adrp Xt, target` with the
    page offset folded into the `ldr Xt, [Xt]` that follows; the literal's relocation becomes
    padding (type 0) and the literal is dead data. Returns (rewritten loads, relocations left)."""
    code = pe.arm64_ranges()
    in_code = lambda r: any(s <= r < e for s, e in code)  # noqa: E731
    users = {}   # literal rva -> [rva of each `ldr Xt, literal` (64-bit) reading it]
    for start, end in code:
        n = (end - start) // 4
        words = struct.unpack_from("<%dI" % n, pe.image, start)
        for i, w in enumerate(words):
            if w >> 24 == 0x58:
                imm19 = (w >> 5) & 0x7FFFF
                if imm19 & 0x40000:
                    imm19 -= 0x80000
                users.setdefault(start + 4 * i + 4 * imm19, []).append(start + 4 * i)
    done, left = 0, 0
    for at, rva, typ in reloc_entries(pe):
        if not in_code(rva):
            continue
        loads = users.get(rva)
        target = pe.u64(rva) - pe.image_base if typ == 10 else None
        if not loads or target is None or not 0 <= target < pe.size_of_image:
            left += 1
            continue
        plan = []
        for u in loads:
            w = pe.u32(u)
            rt = w & 31
            d = target - u
            if -(1 << 20) <= d < (1 << 20):
                plan.append((u, 0x10000000 | ((d & 3) << 29) | (((d >> 2) & 0x7FFFF) << 5) | rt, None))
                continue
            nxt = pe.u32(u + 4)
            if nxt != (0xF9400000 | (rt << 5) | rt) or target % 8:
                plan = None
                break
            pd = (target >> 12) - (u >> 12)
            plan.append((u, 0x90000000 | ((pd & 3) << 29) | (((pd >> 2) & 0x7FFFF) << 5) | rt,
                         0xF9400000 | (((target & 0xFFF) // 8) << 10) | (rt << 5) | rt))
        if not plan:
            left += 1
            continue
        for u, first, second in plan:
            struct.pack_into("<I", pe.image, u, first)
            if second is not None:
                struct.pack_into("<I", pe.image, u + 4, second)
            done += 1
        struct.pack_into("<H", pe.image, at, 0)           # IMAGE_REL_BASED_ABSOLUTE: padding
        pe.dead_literals.update((rva, rva + 4))
    return done, left


def x18_plan(insn, role):
    """(scratch s, scratch t, rewritten insn) or None when the rewrite cannot be done.
    The trampoline pushes s and t, so the instruction must not address memory off sp."""
    rt, rn, rm, rt2 = insn & 31, (insn >> 5) & 31, (insn >> 16) & 31, (insn >> 10) & 31
    is_mov = (insn >> 21) & 0x7FF in (0x150, 0x550) and rn == 31
    if rn == 31 and not is_mov:
        return None                                   # sp-based: our push would move it
    if role == ROLE_RT2 and rn == 31:
        return None
    used = {rt, rn, rm, rt2, 18}
    free = [r for r in (17, 16, 15, 14, 13, 12, 11, 10, 9) if r not in used]
    if len(free) < 2:
        return None
    s, t = free[0], free[1]
    new = replace_field(insn, role, s)
    for r2 in (ROLE_RN, ROLE_RM, ROLE_RT2):          # x18 twice (`add x0, x18, x18`)
        if x18_role(new) == r2:
            new = replace_field(new, r2, s)
    if x18_role(new):
        return None
    return s, t, new


def rewrite_x18(pe, var_rel):
    """Rewrite every x18 site in place into `b tramp`. Returns (trampoline bytes, patched,
    unpatchable). The trampoline area sits directly BEFORE the image (its offsets are
    negative RVAs), the TEB slot offset word at RVA var_rel:
        stp  s, t, [sp, #-16]!
        mrs  s, TPIDRRO_EL0          ; thread's TSD array (iOS keeps it, unlike x18)
        and  s, s, #~7
        adrp t, teb_offset ; ldr t, [t, :lo12:teb_offset]
        ldr  s, [s, t]               ; s = TEB
        <original instruction with x18 -> s>
        ldp  s, t, [sp], #16
        b    <next instruction>"""
    sites, _ = x18_sites(pe)
    plans = [(rva, x18_plan(w, role)) for rva, w, role in sites]
    ok = [(rva, p) for rva, p in plans if p]
    size = 36 * len(ok)
    area = -(-size // PAGE) * PAGE
    tramp = bytearray(area)
    for k, (rva, (s, t, new)) in enumerate(ok):
        at = -area + 36 * k                           # RVA of this trampoline
        page_delta = (var_rel >> 12) - ((at + 12) >> 12)
        lo12 = var_rel & 0xFFF
        insns = [
            0xA9800000 | (0x7E << 15) | (t << 10) | (31 << 5) | s,
            0xD53BD060 | s,
            0x927DF000 | (s << 5) | s,
            0x90000000 | ((page_delta & 3) << 29) | (((page_delta >> 2) & 0x7FFFF) << 5) | t,
            0xF9400000 | ((lo12 // 8) << 10) | (t << 5) | t,
            0xF8606800 | (t << 16) | (s << 5) | s,
            new,
            0xA8C00000 | (2 << 15) | (t << 10) | (31 << 5) | s,
            b(at + 32, rva + 4),
        ]
        struct.pack_into("<9I", tramp, 36 * k, *insns)
        struct.pack_into("<I", pe.image, rva, b(rva, at))
    return bytes(tramp), len(ok), len(sites) - len(ok)


def audit(pe):
    code, write, text_relocs = page_sets(pe)
    conflicts = sorted(code & write)
    sites, tsd = x18_sites(pe)
    unpatchable = sum(1 for _, w, role in sites if not x18_plan(w, role))
    blockers = []
    if conflicts:
        blockers.append("%d code/data pages" % len(conflicts))
    if text_relocs:
        blockers.append("%d relocs in code" % text_relocs)
    if unpatchable:
        blockers.append("%d x18 sites the rewrite cannot handle" % unpatchable)
    if tsd:
        blockers.append("%d TSD reads" % tsd)
    return {
        "dll": pe.name,
        "machine": MACHINES.get(pe.machine, hex(pe.machine)),
        "arm64ec": bool(pe.chpe_metadata_rva()),
        "size_of_image": pe.size_of_image,
        "section_alignment": pe.section_alignment,
        "code_bytes": sum(s["span"] for s in pe.sections if s["exec"]),
        "sections": ["%s%s%s@%#x" % (s["name"], "X" if s["exec"] else "", "W" if s["write"] else "", s["va"])
                     for s in pe.sections],
        "conflict_pages": len(conflicts),
        "relocs": len(pe.relocs()),
        "text_relocs": text_relocs,
        "x18_sites": len(sites),
        "x18_unpatchable": unpatchable,
        "tsd_reads": tsd,
        "imports": len(pe.imports()),
        "ready": not blockers,
        "blockers": blockers,
    }


def cmd_convert(a):
    pe = PE(a.dll)
    lit_done, lit_left = rewrite_code_literals(pe)
    if lit_done or lit_left:
        print("code literals: %d loads made position-independent, %d relocations in code left" % (lit_done, lit_left))
    r = audit(pe)
    print(json.dumps(r, indent=1))
    for d, n in pe.imports():
        print("import %s!%s" % (d, n))
    code, write, _ = page_sets(pe)
    split = (max(code) + 1) * PAGE if code else PAGE    # end of the last code page
    early = sorted(p for p in write if p * PAGE < split)
    problems = list(r["blockers"])
    if early:
        problems.append("written at load but below the end of code: pages %s" % ", ".join("%#x" % (p * PAGE) for p in early[:8]))
    if a.strict and problems:
        sys.exit("%s: not loadable from a signed dylib: %s" % (pe.name, "; ".join(problems)))

    img_size = -(-pe.size_of_image // PAGE) * PAGE
    tramp, patched, skipped = rewrite_x18(pe, img_size)
    after, _ = x18_sites(pe)
    print("x18: %d sites rewritten to trampolines (%d KB), %d left, %d remain after rewrite"
          % (patched, len(tramp) >> 10, skipped, len(after)))
    img = bytes(pe.image) + bytes(img_size - len(pe.image))
    os.makedirs(a.outdir, exist_ok=True)
    stem = pe.name.lower() if a.keep_ext else os.path.splitext(pe.name)[0]
    for part, data in (("tramp", tramp), ("text", img[:split]), ("data", img[split:])):
        with open(os.path.join(a.outdir, "%s.%s.img" % (stem, part)), "wb") as f:
            f.write(data)
    with open(os.path.join(a.outdir, stem + ".S"), "w") as f:
        f.write("// Generated by engine/pedylib/pe2dylib.py from %s: the PE image as mapped.\n"
                "// __TEXT: x18 trampolines, then headers + code (RVA 0-%#x); __DATA: the rest\n"
                "// directly after, then the TEB slot offset word the loader sets.\n"
                "    .section __TEXT,__pe_image,regular,pure_instructions\n"
                "    .p2align 14\n"
                "    .globl _myiosdeck_x18_tramps\n"
                "_myiosdeck_x18_tramps:\n"
                "    .incbin \"%s.tramp.img\"\n"
                "    .globl _myiosdeck_pe_image\n"
                "_myiosdeck_pe_image:\n"
                "    .incbin \"%s.text.img\"\n"
                "    .section __DATA,__pe_data\n"
                "    .p2align 14\n"
                "    .globl _myiosdeck_pe_data\n"
                "_myiosdeck_pe_data:\n"
                "    .incbin \"%s.data.img\"\n"
                "    .globl _myiosdeck_pe_image_end\n"
                "_myiosdeck_pe_image_end:\n"
                "    .globl _myiosdeck_teb_offset\n"
                "_myiosdeck_teb_offset:\n"
                "    .quad %#x\n" % (pe.name, split, stem, stem, stem, TEB_TSD_OFFSET_DEFAULT))
    print("wrote %s.S: %d KB trampolines + %d KB headers/code in __TEXT, %d KB data in __DATA"
          % (stem, len(tramp) >> 10, split >> 10, (img_size - split) >> 10))


def cmd_audit(a):
    rows = []
    for p in a.dlls:
        try:
            pe = PE(p)
            rewrite_code_literals(pe)   # as convert does: what is left is a real blocker
            rows.append(audit(pe))
        except Exception as e:  # noqa: BLE001 - report and keep going
            rows.append({"dll": os.path.basename(p), "error": str(e), "ready": False, "blockers": [str(e)]})
    ok = [r for r in rows if "error" not in r]
    tot = lambda k: sum(r[k] for r in ok)  # noqa: E731
    lines = [
        "## Wine DLLs vs. the signed-dylib (no-JIT) scheme",
        "",
        "%d DLLs, %.1f MB of code. Ready (x18 rewritten at build time): **%d**. Section alignment: %s." % (
            len(rows), tot("code_bytes") / 1048576, sum(r["ready"] for r in rows),
            ", ".join(sorted({"%#x" % r["section_alignment"] for r in ok}))),
        "",
        "| total | conflict pages | relocs in code | x18 sites | x18 unpatchable | TSD reads |",
        "|---|---|---|---|---|---|",
        "| all DLLs | %d | %d | %d | %d | %d |" % (tot("conflict_pages"), tot("text_relocs"), tot("x18_sites"),
                                                 tot("x18_unpatchable"), tot("tsd_reads")),
        "",
        "Not ready: " + (", ".join("%s (%s)" % (r["dll"], "; ".join(r["blockers"])) for r in rows if not r["ready"]) or "none"),
        "",
        "| DLL | image | code/data pages | x18 sites | x18 unpatchable | imports |",
        "|---|---|---|---|---|---|",
    ]
    for r in sorted(ok, key=lambda r: -r["code_bytes"])[:25]:
        lines.append("| %s | %d KB | %d | %d | %d | %d |" % (
            r["dll"], r["size_of_image"] >> 10, r["conflict_pages"], r["x18_sites"], r["x18_unpatchable"], r["imports"]))
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
    c.add_argument("--keep-ext", action="store_true",
                   help="name outputs after the full file name (kernel32.dll -> libkernel32.dll.dylib), as Wine looks them up")
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
