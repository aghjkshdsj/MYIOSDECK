#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build-time static ARM assembly; no guest program or benchmark is an input.

Each handler implements the same independently selected load/packed-ALU pair
as gpta_pload.inc. All scales, address registers, pinned vector operands and
aliases are emitted. High vector operands retain the C implementation.
"""
import argparse
import itertools
import pathlib
import re

OPS = ('paddb','paddw','paddd','paddq','psubb','psubw','psubd','psubq',
       'pand','pandn','por','pxor')
HEADER = '''// SPDX-License-Identifier: GPL-3.0-or-later
// Static build output of gen_pload_asm.py. Never generated at runtime.
#include "gpta_asm_layout.h"
#ifdef __APPLE__
#define SYM(N) _##N
#define FUNC(N) .p2align 5 ; SYM(N):
#define END(N)
#define TABLE(N) .section __DATA,__const ; .p2align 3 ; .globl SYM(N) ; SYM(N):
.section __TEXT,__gpta_h,regular,pure_instructions
#else
#define SYM(N) N
#define FUNC(N) .p2align 5 ; .type N,%function ; N:
#define END(N) .size N,.-N
#define TABLE(N) .section .data.rel.ro,"aw",%progbits ; .p2align 3 ; .globl N ; .type N,%object ; N:
.section gpta_h,"ax",%progbits
#endif
'''
TAIL = '''#ifndef __APPLE__
.section .note.GNU-stack,"",%progbits
#endif
'''


def operands():
    return itertools.product(range(4), range(8), range(8), range(17), range(17))


def label(op, values, suffix):
    return 'gpta_apl_' + op + '_' + '_'.join(map(str, values)) + suffix


def alu(op, d, s):
    source = 16 if d == s else s   # the second instruction sees the completed load
    if op.startswith(('padd','psub')):
        lanes = {'b':'16b', 'w':'8h', 'd':'4s', 'q':'2d'}[op[-1]]
        mnemonic = 'add' if op.startswith('padd') else 'sub'
        return f'{mnemonic} v{d}.{lanes}, v16.{lanes}, v{source}.{lanes}'
    if op == 'pandn':
        return f'bic v{d}.16b, v{source}.16b, v16.16b'
    mnemonic = {'pand':'and', 'por':'orr', 'pxor':'eor'}[op]
    return f'{mnemonic} v{d}.16b, v16.16b, v{source}.16b'


def host_regs():
    # Use the recovery map as the single register-number source. CI's native
    # differential and exact-fault gates validate the actual compiler ABI.
    source = pathlib.Path(__file__).with_name('gpta_pin.c').read_text()
    match = re.search(r'k_host_gpr\[16\]\s*=\s*\{([^}]+)\}', source)
    if not match:
        raise SystemExit('missing ARM64 host register map')
    numbers = [int(value.strip()) for value in match[1].split(',')]
    if len(numbers) != 16 or len(set(numbers)) != 16:
        raise SystemExit('invalid ARM64 host register map')
    return [f'x{number}' for number in numbers] + ['xzr']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out', type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    regs = host_regs()
    for op, replica in itertools.product(OPS, range(2)):
        suffix = '_r1' if replica else ''
        path = args.out / f'gpta_pin_apl_{op}{suffix}.S'
        with path.open('w', newline='\n') as out:
            out.write(HEADER)
            for values in operands():
                scale, d, s, base, index = values
                name = label(op, values, suffix)
                out.write(f'FUNC({name})\n')
                out.write(f'\tadd x8, {regs[base]}, {regs[index]}, lsl #{scale}\n')
                out.write('\tldr x15, [x21, #GPTA_ASM_DISP]\n')
                # The access is unconditional, including self-XOR/SUB/ANDNOT.
                # Only scratch registers have changed before this instruction.
                out.write('\tldr q16, [x8, x15]\n')
                out.write('\t' + alu(op,d,s) + '\n')
                out.write('''#ifdef GPTA_PROFILE
\tldr x8, [x21, #(GPTA_ASM_UOP_SIZE + GPTA_ASM_PROF)]
\tadd x8, x8, #1
\tstr x8, [x21, #(GPTA_ASM_UOP_SIZE + GPTA_ASM_PROF)]
#endif
\tldr x15, [x21, #GPTA_ASM_UOP_SIZE]!
\tbr x15
''')
                out.write(f'END({name})\n')
            out.write(f'TABLE(gpta_t_apl_{op}{suffix})\n')
            for values in operands():
                out.write(f'\t.quad SYM({label(op,values,suffix)})\n')
            out.write(TAIL)
        print(path.name, flush=True)


if __name__ == '__main__':
    main()
