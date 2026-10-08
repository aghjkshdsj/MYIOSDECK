// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: loop-step superinstructions, kinds sii and sri (see fxr_step.inc), and the 3-operand pairs
// "mov D, S ; op D, imm" (op: add sub and or xor shl shr sar rol ror) as D = S op imm, chosen from
// the corpus (engine/fxr/corpus.sh: "3-operand: ALU/shift with an immediate").

#include "fxr_pin.h"
#include "fxr_step.inc"

STEP_DEFS(SRI)
const PFn t_sri[2][2][10][16][16] = STEP_TABLE(sri);

#define DEF_SII_CC(CC, OP, SZ) R16(DEF_SII, OP, SZ, CC)
C10(DEF_SII_CC, add, 32) C10(DEF_SII_CC, add, 64) C10(DEF_SII_CC, sub, 32) C10(DEF_SII_CC, sub, 64)
#define E_SII(D, OP, SZ, CC) p_sii_##OP##_##SZ##_##CC##_##D,
#define CC_SII(CC, OP, SZ) { R16(E_SII, OP, SZ, CC) },
const PFn t_sii[2][2][10][16] = { { { C10(CC_SII, add, 32) }, { C10(CC_SII, add, 64) } },
                                  { { C10(CC_SII, sub, 32) }, { C10(CC_SII, sub, 64) } } };

// ---- mov D, S ; op D, imm -> D = S op imm. The immediate (a shift's masked, nonzero count) in
// imm; rotates only when their flags are dead (the lowering checks). ----
#define T3_add(a, b, SZ) ((a) + (b))
#define T3_sub(a, b, SZ) ((a) - (b))
#define T3_and(a, b, SZ) ((a) & (b))
#define T3_or(a, b, SZ) ((a) | (b))
#define T3_xor(a, b, SZ) ((a) ^ (b))
#define T3_shl(a, n, SZ) ((a) << (n))
#define T3_shr(a, n, SZ) ((a) >> (n))
#define T3_sar(a, n, SZ) (uint64_t)((int64_t)((a) << (64 - SZ)) >> (64 - SZ) >> (n))
#define T3_rol(a, n, SZ) (((a) << (n)) | ((a) >> ((SZ - (n)) % SZ)))
#define T3_ror(a, n, SZ) (((a) >> (n)) | ((a) << ((SZ - (n)) % SZ)))
#define K3_add LF_ADD
#define K3_sub LF_SUB
#define K3_and LF_LOGIC
#define K3_or LF_LOGIC
#define K3_xor LF_LOGIC
#define K3_shl LF_SHL
#define K3_shr LF_SHR
#define K3_sar LF_SAR
#define K3_rol 0
#define K3_ror 0
#define F3_add 1
#define F3_sub 1
#define F3_and 1
#define F3_or 1
#define F3_xor 1
#define F3_shl 1
#define F3_shr 1
#define F3_sar 1
#define F3_rol 0
#define F3_ror 0
#define DEF_3OP(S, D, SZ, OPN)                                                             \
    PH p_op3_##OPN##_##SZ##_##D##_##S(FXR_PARAMS) {                                        \
        uint64_t a = g##S & M##SZ, b = u->imm & M##SZ, r = T3_##OPN(a, b, SZ) & M##SZ;     \
        g##D = r;                                                                          \
        PNEXT(); }
#define DEF_3OP_ROW(D, SZ, OPN) R16B(DEF_3OP, D, SZ, OPN)
#define DEF_3OP_ALL(OPN) R16(DEF_3OP_ROW, 32, OPN) R16(DEF_3OP_ROW, 64, OPN)
DEF_3OP_ALL(add) DEF_3OP_ALL(sub) DEF_3OP_ALL(and) DEF_3OP_ALL(or) DEF_3OP_ALL(xor)
DEF_3OP_ALL(shl) DEF_3OP_ALL(shr) DEF_3OP_ALL(sar) DEF_3OP_ALL(rol) DEF_3OP_ALL(ror)
#define T_3OP(OPN) { { R16(ROW_RR, op3_##OPN##_32) }, { R16(ROW_RR, op3_##OPN##_64) } },
const PFn t_3op[10][2][16][16] = { T_3OP(add) T_3OP(sub) T_3OP(and) T_3OP(or) T_3OP(xor)
                                   T_3OP(shl) T_3OP(shr) T_3OP(sar) T_3OP(rol) T_3OP(ror) };   // [op][64?][D][S]
