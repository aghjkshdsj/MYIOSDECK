// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: fused compare-and-branch handlers, specialised on the condition code. See fxr_pin.c.

#include "fxr_pin.h"

// ---- fused cmp/test + jcc, specialised on the condition code; the flags are recorded only if
// something after the branch reads them. RI: the immediate rides in disp (imm is the target). ----
#define FJ_BR(TAKEN) do { if (TAKEN) PCHAIN(u->ulink, p_miss_t); PCHAIN(u->ulink2, p_miss_f); } while (0)
#define DEF_FJC_RR(S, D, SZ, CC)                                                           \
    PH p_fjc_rr_##SZ##_##CC##_##D##_##S(FXR_PARAMS) {                                      \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ;                                       \
        if (u->flive) SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0);                      \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }
#define DEF_FJC_RR_ROW(D, SZ, CC) R16B(DEF_FJC_RR, D, SZ, CC)
#define DEF_FJ_D(D, SZ, CC)                                                                \
    PH p_fjc_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        if (u->flive) SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0);                      \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fjt_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = a & b;               \
        if (u->flive) SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                  \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }                                                  \
    PH p_fjt_rr_##SZ##_##CC##_##D(FXR_PARAMS) {   /* test r, r: the same register */       \
        uint64_t a = g##D & M##SZ;                                                         \
        if (u->flive) SETF(LF_LOGIC, SI##SZ, a, a, a, 0);                                  \
        FJ_BR(cc_logic(CC, a, SI##SZ)); }
#define DEF_FJ_CC(CC, SZ) R16(DEF_FJC_RR_ROW, SZ, CC) R16(DEF_FJ_D, SZ, CC)
C16(DEF_FJ_CC, 32) C16(DEF_FJ_CC, 64)
// test of two different registers (less common): the condition code at run time
#define DEF_FJT_RRX(S, D, SZ)                                                              \
    PH p_fjt_rrx_##SZ##_##D##_##S(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = a & b;                            \
        if (u->flive) SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                  \
        FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
#define DEF_FJT_RRX_ROW(D, SZ) R16B(DEF_FJT_RRX, D, SZ)
R16(DEF_FJT_RRX_ROW, 32) R16(DEF_FJT_RRX_ROW, 64)
#define E_FJC_RR(S, D, SZ, CC) p_fjc_rr_##SZ##_##CC##_##D##_##S,
#define ROW_FJC_RR(D, SZ, CC) { R16B(E_FJC_RR, D, SZ, CC) },
#define CC_FJC_RR(CC, SZ) { R16(ROW_FJC_RR, SZ, CC) },
const PFn t_fjc_rr[2][16][16][16] = { { C16(CC_FJC_RR, 32) }, { C16(CC_FJC_RR, 64) } };   // [64?][cc][D][S]
#define E_FJ_X(D, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##D,
#define CC_FJ_X(CC, SZ, NAME) { R16(E_FJ_X, SZ, CC, NAME) },
const PFn t_fjc_ri[2][16][16] = { { C16(CC_FJ_X, 32, fjc_ri) }, { C16(CC_FJ_X, 64, fjc_ri) } };   // [64?][cc][D]
const PFn t_fjt_ri[2][16][16] = { { C16(CC_FJ_X, 32, fjt_ri) }, { C16(CC_FJ_X, 64, fjt_ri) } };
const PFn t_fjt_rr[2][16][16] = { { C16(CC_FJ_X, 32, fjt_rr) }, { C16(CC_FJ_X, 64, fjt_rr) } };
const PFn t_fjt_rrx[2][16][16] = { { R16(ROW_RR, fjt_rrx_32) }, { R16(ROW_RR, fjt_rrx_64) } };   // [64?][D][S]
