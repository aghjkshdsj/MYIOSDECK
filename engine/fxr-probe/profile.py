#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Diagnostic (CI only): names the handlers in an FXR_PROFILE dump (fxr_profile_dump in
# engine/fxr/fxr_pin.c) and shows the guest instructions of each hot block.
#   profile.py <fxr built with -DFXR_PROFILE> <guest.elf> <stderr of the run>
import bisect, re, shutil, struct, subprocess, sys

fxr, guest, dump = sys.argv[1:4]
syms = []
for line in subprocess.run(['nm', '-n', fxr], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
base = next(a for a, n in syms if n == 'fxr_lower')

def name(off):
    a = base + off
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 and syms[i][0] == a else '?%#x' % a

d = open(guest, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
lo = min(struct.unpack_from('<IIQQ', d, phoff + i * phentsize)[3] for i in range(phnum)
         if struct.unpack_from('<I', d, phoff + i * phentsize)[0] == 1)
objdump = next((t for t in ('llvm-objdump-19', 'llvm-objdump') if shutil.which(t)), None)

def guest_code(rips):
    if not rips or not objdump:
        return
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', '--x86-asm-syntax=intel',
                          '--start-address=%#x' % min(rips), '--stop-address=%#x' % (max(rips) + 16), guest],
                         capture_output=True, text=True).stdout
    for l in out.splitlines():
        if re.match(r'\s+[0-9a-f]+:', l):
            print('      guest ' + l.strip())

rips = []
for line in open(dump, errors='replace'):
    if not line.startswith('[fxr-prof]'):
        continue
    m = re.match(r'\[fxr-prof\]\s+(\d+)\s+@(-?\d+)\s+img\+(0x[0-9a-f]+|0)\s*$', line)
    if m:
        rip = lo + int(m[3], 16)
        if rip:
            rips.append(rip)
        print('  %12s  %-40s %#x' % (m[1], name(int(m[2])), rip))
        continue
    guest_code(rips)
    rips = []
    print(re.sub(r'img\+(0x[0-9a-f]+|0)', lambda g: 'guest %#x' % (lo + int(g[1], 16)), line.rstrip()))
guest_code(rips)
