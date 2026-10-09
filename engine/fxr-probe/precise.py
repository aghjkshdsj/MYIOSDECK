#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# FXR exact-fault check (CI): for every pinned handler (section fxr_h of an ARM64 build), does a
# guest memory access ever come after a guest-visible side effect on some path through the
# handler? A host fault at that access would then see a half-done instruction: a pinned guest
# register, a lazy-flag word, XMM0-7 or the CPU structure already changed, or another guest store
# already done. Handlers with no such path fault with the x64 state exactly as before the
# instruction (fxi_win_host_state, fxr_pin.c).
#   precise.py <objdump -d --no-show-raw-insn -j fxr_h output>   (exit 1 if a handler Windows mode
#   uses has such a path; prints a summary)
# Model: guest addresses come from the pinned guest registers and T (taint, through ALU ops and
# loads from guest memory); x10 (CPU), x21 (uop), sp and static data are FXR's own. Conservative: every path
# through the handler's own code (branches inside it followed both ways).
import collections, re, sys

PINNED_GPR = {22, 23, 24, 25, 26, 27, 28, 0, 1, 2, 3, 4, 5, 6, 7, 20}  # guest RAX..R15 (R15: x20)
FLAGS = {12, 13, 14, 9}                                                 # F0..F3 (clang assigns x9 last)
T_REG = 11
INTERNAL = {10, 21, 'sp'}   # the CPU pointer, the uop

def reg(tok):
    """Register name -> 'rN' for x/w N, 'vN' for SIMD, 'sp', or None."""
    tok = tok.strip().lstrip('{').rstrip('}').strip()
    m = re.match(r'^([xw])(\d+)$', tok)
    if m: return 'r' + m[2]
    if tok in ('sp', 'wsp'): return 'sp'
    m = re.match(r'^([vqdshb])(\d+)(\.|\[|$)', tok)
    if m: return 'v' + m[2]
    return None

def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '[{': depth += 1
        if ch in ']}': depth -= 1
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

NODEST = re.compile(r'^(b|bl|br|blr|ret|cbz|cbnz|tbz|tbnz|cmp|cmn|tst|fcmp|fcmpe|ccmp|ccmn|fccmp|fccmpe|prfm|nop|b\..*|st.*|dmb|dsb|isb)$')

def parse(text):
    funcs, cur = collections.OrderedDict(), None
    for line in text.splitlines():
        m = re.match(r'^([0-9a-f]+) <(.+)>:$', line)
        if m: cur = m[2]; funcs[cur] = []; continue
        m = re.match(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$', line)
        if m and cur:
            ops = re.sub(r'\s*//.*$', '', m[3]).strip()
            ops = re.sub(r'\s*<[^>]*>$', '', ops)
            funcs[cur].append((int(m[1], 16), m[2], ops))
    return funcs

def analyse(insns):
    """Returns (violations: list of (addr, mnemonic, why)), guest accesses count."""
    if not insns: return [], 0
    addrs = [a for a, _, _ in insns]
    idx = {a: i for i, a in enumerate(addrs)}
    lo, hi = addrs[0], addrs[-1]
    def succ(i):
        a, mn, ops = insns[i]
        tgt = None
        m = re.search(r'\b([0-9a-f]+)$', ops) if (mn in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or mn.startswith('b.')) else None
        if m: tgt = int(m[1], 16)
        out = []
        if tgt is not None and lo <= tgt <= hi and tgt in idx: out.append(idx[tgt])
        if mn in ('b', 'br', 'ret', 'blr') or (mn == 'b' and tgt is None): return out
        if i + 1 < len(insns): out.append(i + 1)
        return out
    # state: (written tags, tainted registers (guest-derived), own registers (FXR pointers: the CPU,
    # the uop, sp, and those plus any offset, e.g. the indirect-branch cache entry))
    init_taint = frozenset({'r%d' % r for r in PINNED_GPR} | {'r%d' % T_REG})
    init_own = frozenset({"r10", "r21", "sp"})
    init_holds = frozenset(("r%d" % r, "r%d" % r) for r in PINNED_GPR | FLAGS)
    state_in = {0: (frozenset(), init_taint, init_own, init_holds)}
    work = [0]
    viol = {}
    guest = 0
    seen_guest = set()
    while work:
        i = work.pop()
        written, taint, own, holds = state_in[i]
        a, mn, ops = insns[i]
        o = split_ops(ops)
        w, t, ow_, hd = set(written), set(taint), set(own), dict(holds)
        mem = next((x for x in o if x.startswith('[')), None)
        if mem is not None:
            inner = mem[1:mem.index(']')]
            parts = [p.strip() for p in inner.split(',')]
            base = reg(parts[0])
            is_guest = base is not None and base not in ow_ and base in t
            is_store = mn.startswith('st') or mn.startswith('cas') or mn.startswith('swp') or mn.startswith('ldadd') \
                or mn.startswith('ldset') or mn.startswith('ldclr') or mn.startswith('ldeor')
            if is_guest:
                if a not in seen_guest: seen_guest.add(a); guest += 1
                if w and a not in viol: viol[a] = (mn, ','.join(sorted(w)))
            # destinations of a load (operands before the memory one)
            dests = []
            if not is_store:
                dests = [reg(x) for x in o[:o.index(mem)]]
            elif mn.startswith(('stxr', 'stlxr', 'stxp', 'stlxp')):
                dests = [reg(o[0])]   # the status register
            elif mn.startswith(('cas', 'swp', 'ldadd', 'ldset', 'ldclr', 'ldeor')):
                dests = [reg(o[1]) if mn.startswith('swp') or mn.startswith('ld') else reg(o[0])]
            for d in dests:
                if d is None: continue
                ow_.discard(d)
                if is_guest: t.add(d)
                else: t.discard(d)
            if ']!' in ops or re.search(r'\],\s*#', ops):   # writeback: the base changes
                dests = dests + [base]
            if is_store:
                if is_guest: w.add('guest-store')
                elif base in ow_ and base not in ('r21', 'sp'): w.add('cpu-store')   # the CPU structure (XMM8-15, ...)
            for d in dests:
                if d is None or d == 'sp': continue
                n = int(d[1:])
                hd.pop(d, None)
                if d[0] == 'r' and (n in PINNED_GPR or n in FLAGS): w.add(d)
                if d[0] == 'v' and n < 8: w.add(d)
        elif o and not NODEST.match(mn):
            d = reg(o[0])
            srcs = [reg(x) for x in o[1:]]
            if d is not None and d != 'sp':
                n = int(d[1:])
                if mn in ('adrp', 'adr'):
                    ow_.add(d); t.discard(d)   # FXR's static data (a jump table, a constant table)
                elif mn in ('add', 'sub', 'mov', 'adds', 'subs') and any(s in ow_ for s in srcs if s):
                    ow_.add(d); t.discard(d)   # an FXR pointer plus an offset stays FXR's memory
                elif any(s in t for s in srcs if s): t.add(d); ow_.discard(d)
                else:
                    ow_.discard(d); t.discard(d)
                # a 64-bit register copy carries which entry value it holds; a guest register given
                # its own entry value back (spilled to a scratch register and restored) is unchanged
                if mn == 'mov' and len(o) == 2 and o[0].startswith('x') and o[1].startswith('x') and srcs[0] in hd:
                    hd[d] = hd[srcs[0]]
                else:
                    hd.pop(d, None)
                if d[0] == 'r' and (n in PINNED_GPR or n in FLAGS):   # taint follows the content
                    if hd.get(d) == d: w.discard(d)
                    else: w.add(d)
                if d[0] == 'v' and n < 8: w.add(d)
            # instructions writing two registers (umull etc. write one); ldp handled above
        out_state = (frozenset(w), frozenset(t), frozenset(ow_), frozenset(hd.items()))
        for s in succ(i):
            if s in state_in:
                pw, pt, po, ph = state_in[s]
                # merge: written and tainted unite; a register is FXR's own only if it is on both paths
                nw, nt, no, nh = pw | out_state[0], pt | out_state[1], po & out_state[2], ph & out_state[3]
                if (nw, nt, no, nh) == (pw, pt, po, ph): continue
                state_in[s] = (nw, nt, no, nh)
            else:
                state_in[s] = out_state
            work.append(s)
    return [(a, mn, why) for a, (mn, why) in sorted(viol.items())], guest

def family(name):
    name = re.sub(r'_(\d+|[ZM]|N|F|IN|I|64N|32N|64F|32F|64|32|16|8)(?=_|$)', '_#', name)
    return re.sub(r'(_#)+', '_#', name)

# Pair fusions Windows mode does not use (fxr_pin.c, try_fuse: g_win): their second instruction
# touches memory after the first wrote a register, so they may report a violation.
WIN_EXCLUDED = ('p_amr_', 'p_alea_', 'p_ami_', 'p_axz_', 'p_pop2_', 'p_push2_', 'p_popret_')

def main():
    funcs = parse(open(sys.argv[1], errors='replace').read())
    total = with_guest = bad = 0
    fam = collections.Counter(); famall = collections.Counter(); examples = {}
    gate = []
    for name, insns in funcs.items():
        total += 1
        v, g = analyse(insns)
        if g: with_guest += 1
        f = family(name)
        famall[f] += 1
        if v:
            bad += 1
            fam[f] += 1
            examples.setdefault(f, (name, v[0]))
            if not name.startswith(WIN_EXCLUDED): gate.append(name)
    print('precise: %d pinned handlers, %d touch guest memory, %d may fault after a side effect, '
          '%d of them used in Windows mode' % (total, with_guest, bad, len(gate)))
    for f, n in fam.most_common(60):
        name, (a, mn, why) = examples[f]
        print('  %6d / %-6d %-28s e.g. %s at +%#x (%s) after %s%s' % (n, famall[f], f, name, a - funcs[name][0][0], mn, why,
              '' if name.startswith(WIN_EXCLUDED) else '   <- used in Windows mode'))
    shown = 0
    for f, n in fam.most_common():   # the code of a few examples used in Windows mode
        name, (a, mn, why) = examples[f]
        if name.startswith(WIN_EXCLUDED) or shown >= 10: continue
        shown += 1
        print('-- %s (violation at %#x)' % (name, a))
        for ia, imn, iops in funcs[name][:40]:
            print('   %s %-8s %s%s' % ('>' if ia == a else ' ', imn, iops, ''))
    sys.exit(1 if gate else 0)

if __name__ == '__main__':
    main()
