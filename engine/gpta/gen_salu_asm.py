#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static immediate shift/rotate + ALU pairs selected from independent code.

All five shifts, all five ALUs, both widths, every nonzero masked count and
both register operands are emitted. No benchmark or guest input is read.
"""
import argparse
import itertools
import pathlib

from gen_pload_asm import HEADER, TAIL, host_regs

SHIFTS = ('shl','shr','sar','rol','ror')
ALUS = ('add','sub','and','or','xor')


def operands():
    for width in (32,64):
        for count in range(1,width):
            yield from ((width,count,d,e) for d in range(16) for e in range(16))


def label(sh,op,values,suffix):
    return 'gpta_asal_' + sh + '_' + op + '_' + '_'.join(map(str,values)) + suffix


def instructions(sh,op,width,count,d,e,regs):
    prefix = 'w' if width == 32 else 'x'
    rd,re = prefix+regs[d][1:],prefix+regs[e][1:]
    shift = {'shl':'lsl','shr':'lsr','sar':'asr','rol':'ror','ror':'ror'}[sh]
    n = width-count if sh == 'rol' else count
    alu = {'add':'add','sub':'sub','and':'and','or':'orr','xor':'eor'}[op]
    # ADD and bitwise operations commute. ARM encodes a shift on their
    # second input, so the pair can be one architectural operation. ROR is
    # encodable by the bitwise family. Aliases read the completed shift.
    if d != e and op != 'sub' and (shift != 'ror' or op in ('and','or','xor')):
        return [f'{alu} {rd}, {re}, {rd}, {shift} #{n}']
    return [f'{shift} {prefix}15, {rd}, #{n}',
            f'{alu} {rd}, {prefix}15, {prefix+"15" if d == e else re}']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out',type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    regs = host_regs()[:16]
    for sh,op,replica in itertools.product(SHIFTS,ALUS,range(2)):
        suffix = '_r1' if replica else ''
        path = args.out / f'gpta_pin_asal_{sh}_{op}{suffix}.S'
        with path.open('w',newline='\n') as out:
            out.write(HEADER)
            for values in operands():
                width,count,d,e = values
                name = label(sh,op,values,suffix)
                out.write(f'FUNC({name})\n')
                for insn in instructions(sh,op,width,count,d,e,regs): out.write('\t'+insn+'\n')
                out.write('''#ifdef GPTA_PROFILE
\tldr x8, [x21, #(GPTA_ASM_UOP_SIZE + GPTA_ASM_PROF)]
\tadd x8, x8, #1
\tstr x8, [x21, #(GPTA_ASM_UOP_SIZE + GPTA_ASM_PROF)]
#endif
\tldr x15, [x21, #GPTA_ASM_UOP_SIZE]!
\tbr x15
''')
                out.write(f'END({name})\n')
            out.write(f'TABLE(gpta_t_asal_{sh}_{op}{suffix})\n')
            for width,count,d,e in itertools.product((32,64),range(64),range(16),range(16)):
                target = f'SYM({label(sh,op,(width,count,d,e),suffix)})' if 0 < count < width else '0'
                out.write(f'\t.quad {target}\n')
            out.write(TAIL)
        print(path.name,flush=True)


if __name__ == '__main__':
    main()
