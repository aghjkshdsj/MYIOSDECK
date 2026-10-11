// SPDX-License-Identifier: GPL-3.0-or-later
// Register-copy + register-ALU pairs from the independent corpus:
// mov D,S ; op D,R -> D = S op R, with dead output flags.
// The lowering substitutes S for R when R aliases the copied destination.
#include "gpta_pin.h"

#define OP3R_add(a, b) ((a) + (b))
#define OP3R_sub(a, b) ((a) - (b))
#define OP3R_and(a, b) ((a) & (b))
#define OP3R_or(a, b) ((a) | (b))
#define OP3R_xor(a, b) ((a) ^ (b))
#define OP3R_mul(a, b) ((a) * (b))
#define DEF_OP3R(R, S, D, SZ, OP) \
    PH p_op3r_##OP##_##SZ##_##D##_##S##_##R(GPTA_PARAMS) { \
        g##D = OP3R_##OP(g##S, g##R) & M##SZ; PNEXT(); }
#define DEF_OP3R_S(S, D, SZ, OP) R16C(DEF_OP3R, S, D, SZ, OP)
#define DEF_OP3R_D(D, SZ, OP) R16B(DEF_OP3R_S, D, SZ, OP)
#define DEF_OP3R_ALL(OP) R16(DEF_OP3R_D, 32, OP) R16(DEF_OP3R_D, 64, OP)
DEF_OP3R_ALL(add) DEF_OP3R_ALL(sub) DEF_OP3R_ALL(and)
DEF_OP3R_ALL(or) DEF_OP3R_ALL(xor) DEF_OP3R_ALL(mul)
#define E_OP3R(R, S, D, SZ, OP) p_op3r_##OP##_##SZ##_##D##_##S##_##R,
#define ROW_OP3R_S(S, D, SZ, OP) { R16C(E_OP3R, S, D, SZ, OP) },
#define ROW_OP3R_D(D, SZ, OP) { R16B(ROW_OP3R_S, D, SZ, OP) },
#define T_OP3R(OP) { { R16(ROW_OP3R_D, 32, OP) }, { R16(ROW_OP3R_D, 64, OP) } },
const PFn t_op3r[6][2][16][16][16] = {
    T_OP3R(add) T_OP3R(sub) T_OP3R(and) T_OP3R(or) T_OP3R(xor) T_OP3R(mul)
};
