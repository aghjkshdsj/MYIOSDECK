// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: "add/sub/and/or/xor D, S ; jcc" as one uop (the corpus, engine/fxr/corpus.sh: "ALU reg ; jcc:
// register operand"), specialised on both registers and on the 8 conditions ALU results are most
// often tested for (C8). The flags are not recorded: an edge that reads them gets a flag stub
// (fxr_pin_stub.c), or the pair does not fuse. See fxr_pin.c.

#include "fxr_pin.h"

#define AX_add(a, b) ((a) + (b))
#define AX_sub(a, b) ((a) - (b))
#define AX_and(a, b) ((a) & (b))
#define AX_or(a, b) ((a) | (b))
#define AX_xor(a, b) ((a) ^ (b))
FXI_INLINE int arj_add_cond(unsigned cc, uint64_t a, uint64_t b, uint64_t r, unsigned si) {
    uint64_t sb = kSign[si];
    return cc_flags(cc, (((a ^ r) & (b ^ r)) & sb) != 0, r < a, r == 0, (r & sb) != 0, PAR(r));
}
#define AC_add(CC, a, b, r, SI) arj_add_cond(CC, a, b, r, SI)
#define AC_sub(CC, a, b, r, SI) cc_sub(CC, a, b, SI)
#define AC_and(CC, a, b, r, SI) cc_logic(CC, r, SI)
#define AC_or(CC, a, b, r, SI) cc_logic(CC, r, SI)
#define AC_xor(CC, a, b, r, SI) cc_logic(CC, r, SI)
#define DEF_ARJ(S, D, OPN, SZ, CC)                                                         \
    PH p_arj_##OPN##_##SZ##_##CC##_##D##_##S(FXR_PARAMS) {                                 \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = AX_##OPN(a, b) & M##SZ;           \
        g##D = r;                                                                          \
        FJ_BR(AC_##OPN(CC, a, b, r, SI##SZ)); }
#define DEF_ARJ_ROW(D, OPN, SZ, CC) R16B(DEF_ARJ, D, OPN, SZ, CC)
#define DEF_ARJ_CC(CC, OPN, SZ) R16(DEF_ARJ_ROW, OPN, SZ, CC)
#define DEF_ARJ_OP(OPN) C8(DEF_ARJ_CC, OPN, 32) C8(DEF_ARJ_CC, OPN, 64)
DEF_ARJ_OP(add) DEF_ARJ_OP(sub) DEF_ARJ_OP(and) DEF_ARJ_OP(or) DEF_ARJ_OP(xor)
#define E_ARJ(S, D, OPN, SZ, CC) p_arj_##OPN##_##SZ##_##CC##_##D##_##S,
#define ROW_ARJ(D, OPN, SZ, CC) { R16B(E_ARJ, D, OPN, SZ, CC) },
#define CC_ARJ(CC, OPN, SZ) { R16(ROW_ARJ, OPN, SZ, CC) },
#define T_ARJ(OPN) { { C8(CC_ARJ, OPN, 32) }, { C8(CC_ARJ, OPN, 64) } },
const PFn t_arj[5][2][8][16][16] = { T_ARJ(add) T_ARJ(sub) T_ARJ(and) T_ARJ(or) T_ARJ(xor) };   // [op][64?][cc8][D][S]
