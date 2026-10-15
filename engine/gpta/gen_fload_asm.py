#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static versions of the existing corpus-selected vector load/FP pairs.

No guest input. Emit the complete existing ADD/SUB/MUL PS/PD/SS/SD family,
all scales, address operands, pinned vector operands and both replicas.
"""
import argparse
import itertools
import pathlib

from gen_pload_asm import HEADER, TAIL, host_regs, operands

OPS = tuple(op + kind for kind in ('ps', 'pd', 'ss', 'sd')
            for op in ('add', 'sub', 'mul'))


def label(op, values, suffix):
    return 'gpta_afl_' + op + '_' + '_'.join(map(str, values)) + suffix


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out', type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    regs = host_regs()
    for op, replica in itertools.product(OPS, range(2)):
        suffix = '_r1' if replica else ''
        kind = op[-2:]
        path = args.out / f'gpta_pin_afl_{op}{suffix}.S'
        with path.open('w', newline='\n') as out:
            out.write(HEADER)
            for values in operands():
                scale, d, s, base, index = values
                name = label(op, values, suffix)
                rhs = 16 if d == s else s
                out.write(f'FUNC({name})\n')
                out.write(f'\tadd x8, {regs[base]}, {regs[index]}, lsl #{scale}\n')
                out.write('\tldr x15, [x21, #GPTA_ASM_DISP]\n')
                # A scalar SSE load zeros the other lanes. ARM scalar FP
                # arithmetic has the same zero-upper result in this pair.
                load = 's' if kind == 'ss' else 'd' if kind == 'sd' else 'q'
                out.write(f'\tldr {load}16, [x8, x15]\n')
                if kind in ('ps', 'pd'):
                    lanes = '4s' if kind == 'ps' else '2d'
                    out.write(f'\tf{op[:-2]} v{d}.{lanes}, v16.{lanes}, v{rhs}.{lanes}\n')
                else:
                    out.write(f'\tf{op[:-2]} {load}{d}, {load}16, {load}{rhs}\n')
                out.write('''#ifdef GPTA_PROFILE
\tldr x8, [x21, #(GPTA_ASM_UOP_SIZE + GPTA_ASM_PROF)]
\tadd x8, x8, #1
\tstr x8, [x21, #(GPTA_ASM_UOP_SIZE + GPTA_ASM_PROF)]
#endif
\tldr x15, [x21, #GPTA_ASM_UOP_SIZE]!
\tbr x15
''')
                out.write(f'END({name})\n')
            out.write(f'TABLE(gpta_t_afl_{op}{suffix})\n')
            for values in operands():
                out.write(f'\t.quad SYM({label(op,values,suffix)})\n')
            out.write(TAIL)
        print(path.name, flush=True)


if __name__ == '__main__':
    main()
