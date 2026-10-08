// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: FXI's fused cmp/test + jcc. Specialised on the condition code, without recording the flags
// (the usual case: nothing reads them after the branch); when something does, the generic forms
// below take the condition code at run time and record the flags. See fxr_pin.c.

#include "fxr_pin.h"

#define DEF_FJC_RR(S, D, SZ, CC)                                                           \
    PH p_fjc_rr_##SZ##_##CC##_##D##_##S(FXR_PARAMS) {                                      \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ;                                       \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }
#define DEF_FJC_RR_ROW(D, SZ, CC) R16B(DEF_FJC_RR, D, SZ, CC)
#define DEF_FJ_D(D, SZ, CC)                                                                \
    PH p_fjc_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fjt_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        FJ_BR(cc_logic(CC, a & b, SI##SZ)); }                                              \
    PH p_fjt_rr_##SZ##_##CC##_##D(FXR_PARAMS) {   /* test r, r: the same register */       \
        FJ_BR(cc_logic(CC, g##D & M##SZ, SI##SZ)); }
#define DEF_FJ_CC(CC, SZ) R16(DEF_FJC_RR_ROW, SZ, CC) R16(DEF_FJ_D, SZ, CC)
C16(DEF_FJ_CC, 32) C16(DEF_FJ_CC, 64)
#define E_FJC_RR(S, D, SZ, CC) p_fjc_rr_##SZ##_##CC##_##D##_##S,
#define ROW_FJC_RR(D, SZ, CC) { R16B(E_FJC_RR, D, SZ, CC) },
#define CC_FJC_RR(CC, SZ) { R16(ROW_FJC_RR, SZ, CC) },
const PFn t_fjc_rr[2][16][16][16] = { { C16(CC_FJC_RR, 32) }, { C16(CC_FJC_RR, 64) } };   // [64?][cc][D][S]
#define E_FJ_X(D, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##D,
#define CC_FJ_X(CC, SZ, NAME) { R16(E_FJ_X, SZ, CC, NAME) },
const PFn t_fjc_ri[2][16][16] = { { C16(CC_FJ_X, 32, fjc_ri) }, { C16(CC_FJ_X, 64, fjc_ri) } };   // [64?][cc][D]
const PFn t_fjt_ri[2][16][16] = { { C16(CC_FJ_X, 32, fjt_ri) }, { C16(CC_FJ_X, 64, fjt_ri) } };
const PFn t_fjt_rr[2][16][16] = { { C16(CC_FJ_X, 32, fjt_rr) }, { C16(CC_FJ_X, 64, fjt_rr) } };

// ---- generic: the condition code at run time (u->cc), the flags recorded. test of two different
// registers always comes here (it is uncommon). ----
#define DEF_FJG(S, D, SZ)                                                                  \
    PH p_fjcg_rr_##SZ##_##D##_##S(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ;                                       \
        SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0);                                    \
        FJ_BR(cc_sub(u->cc, a, b, SI##SZ)); }                                              \
    PH p_fjt_rrx_##SZ##_##D##_##S(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = a & b;                            \
        SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                                \
        FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
#define DEF_FJG_ROW(D, SZ) R16B(DEF_FJG, D, SZ)                                            \
    PH p_fjcg_ri_##SZ##_##D(FXR_PARAMS) {                                                  \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0);                                    \
        FJ_BR(cc_sub(u->cc, a, b, SI##SZ)); }                                              \
    PH p_fjtg_ri_##SZ##_##D(FXR_PARAMS) {                                                  \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = a & b;               \
        SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                                \
        FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
R16(DEF_FJG_ROW, 32) R16(DEF_FJG_ROW, 64)
const PFn t_fjcg_rr[2][16][16] = { { R16(ROW_RR, fjcg_rr_32) }, { R16(ROW_RR, fjcg_rr_64) } };   // [64?][D][S]
const PFn t_fjt_rrx[2][16][16] = { { R16(ROW_RR, fjt_rrx_32) }, { R16(ROW_RR, fjt_rrx_64) } };
const PFn t_fjcg_ri[2][16] = { { R16(E1, fjcg_ri_32) }, { R16(E1, fjcg_ri_64) } };   // [64?][D]
const PFn t_fjtg_ri[2][16] = { { R16(E1, fjtg_ri_32) }, { R16(E1, fjtg_ri_64) } };
