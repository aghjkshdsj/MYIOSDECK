// SPDX-License-Identifier: GPL-3.0-or-later
// Retain the usual lazy kind and only the operands that its CF formula reads.
// ADD needs a/result, SUB needs a/b, and logic needs no operands for CF=0.
#include "gpta_pin.h"
#include "gpta_pin_cfonly.h"
#define CF_add(a,b) ((a)+(b))
#define CF_sub(a,b) ((a)-(b))
#define CF_and(a,b) ((a)&(b))
#define CF_or(a,b) ((a)|(b))
#define CF_xor(a,b) ((a)^(b))
#define CK_add LF_ADD
#define CK_sub LF_SUB
#define CK_and LF_LOGIC
#define CK_or LF_LOGIC
#define CK_xor LF_LOGIC
#define CF_BODY(D,B,OP,SZ) \
    uint64_t a = g##D & M##SZ, b = (B) & M##SZ, r = CF_##OP(a,b) & M##SZ; \
    g##D = r; F0 = LF(CK_##OP,SI##SZ); \
    if (CK_##OP == LF_ADD) { F1 = a; F3 = r; } \
    if (CK_##OP == LF_SUB) { F1 = a; F2 = b; } \
    PNEXT();
#define DEF_CF_RR(S,D,OP,SZ) \
    PH p_cf_rr_##OP##_##SZ##_##D##_##S(GPTA_PARAMS) { CF_BODY(D,g##S,OP,SZ) }
#define DEF_CF_D(D,OP,SZ) R16B(DEF_CF_RR,D,OP,SZ) \
    PH p_cf_ri_##OP##_##SZ##_##D(GPTA_PARAMS) { CF_BODY(D,u->imm,OP,SZ) }
#define DEF_CF_OP(OP) R16(DEF_CF_D,OP,32) R16(DEF_CF_D,OP,64)
DEF_CF_OP(add) DEF_CF_OP(sub) DEF_CF_OP(and) DEF_CF_OP(or) DEF_CF_OP(xor)
#define E_CF_RR(S,D,OP,SZ) p_cf_rr_##OP##_##SZ##_##D##_##S,
#define ROW_CF_RR(D,OP,SZ) { R16B(E_CF_RR,D,OP,SZ) },
#define TABLE_CF_RR(OP) { { R16(ROW_CF_RR,OP,32) }, { R16(ROW_CF_RR,OP,64) } },
const PFn t_cf_rr[5][2][16][16] = {
    TABLE_CF_RR(add) TABLE_CF_RR(sub) TABLE_CF_RR(and) TABLE_CF_RR(or) TABLE_CF_RR(xor)
};
#define E_CF_RI(D,OP,SZ) p_cf_ri_##OP##_##SZ##_##D,
#define TABLE_CF_RI(OP) { { R16(E_CF_RI,OP,32) }, { R16(E_CF_RI,OP,64) } },
const PFn t_cf_ri[5][2][16] = {
    TABLE_CF_RI(add) TABLE_CF_RI(sub) TABLE_CF_RI(and) TABLE_CF_RI(or) TABLE_CF_RI(xor)
};
