// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA: FXI's fused cmp/test + jcc. Specialised on the condition code, without recording the flags
// (the usual case: nothing reads them after the branch); when something does, the generic forms
// below take the condition code at run time and record the flags. See gpta_pin.c.

#include "gpta_pin.h"

// A local flag writer can prove the result's width before dispatch. These
// handlers preserve all lazy flags and test only the requested result bit(s).
#define DEF_JRESULT(SZ) \
    PH p_jres_e_##SZ(GPTA_PARAMS) { FJ_BR((F3 & M##SZ) == 0); } \
    PH p_jres_ne_##SZ(GPTA_PARAMS) { FJ_BR((F3 & M##SZ) != 0); } \
    PH p_jres_s_##SZ(GPTA_PARAMS) { FJ_BR((F3 & (1ull << (SZ - 1))) != 0); } \
    PH p_jres_ns_##SZ(GPTA_PARAMS) { FJ_BR((F3 & (1ull << (SZ - 1))) == 0); } \
    PH p_jres_p_##SZ(GPTA_PARAMS) { FJ_BR(PAR(F3)); } \
    PH p_jres_np_##SZ(GPTA_PARAMS) { FJ_BR(!PAR(F3)); }
DEF_JRESULT(8) DEF_JRESULT(16) DEF_JRESULT(32) DEF_JRESULT(64)
#define JRESULT_ROW(SZ) { p_jres_e_##SZ, p_jres_ne_##SZ, p_jres_s_##SZ, p_jres_ns_##SZ, p_jres_p_##SZ, p_jres_np_##SZ }
const PFn t_jresult[4][6] = { JRESULT_ROW(8), JRESULT_ROW(16), JRESULT_ROW(32), JRESULT_ROW(64) };

#define DEF_FJC_RR(S, D, SZ, CC)                                                           \
    PH p_fjc_rr_##SZ##_##CC##_##D##_##S(GPTA_PARAMS) {                                      \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ;                                       \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }
#define DEF_FJC_RR_ROW(D, SZ, CC) R16B(DEF_FJC_RR, D, SZ, CC)
#define DEF_FJ_D(D, SZ, CC)                                                                \
    PH p_fjc_ri_##SZ##_##CC##_##D(GPTA_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fjt_ri_##SZ##_##CC##_##D(GPTA_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        FJ_BR(cc_logic(CC, a & b, SI##SZ)); }                                              \
    PH p_fjt_rr_##SZ##_##CC##_##D(GPTA_PARAMS) {   /* test r, r: the same register */       \
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

// ---- test of two different registers (uncommon): the condition code at run time, the flags
// recorded (so no flag stub). When another edge needs the flags, gpta_pin_stub.c recomputes them. ----
#define DEF_FJT_RRX(S, D, SZ)                                                              \
    PH p_fjt_rrx_##SZ##_##D##_##S(GPTA_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = a & b;                            \
        SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                                \
        FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
#define DEF_FJT_RRX_ROW(D, SZ) R16B(DEF_FJT_RRX, D, SZ)
R16(DEF_FJT_RRX_ROW, 32) R16(DEF_FJT_RRX_ROW, 64)
const PFn t_fjt_rrx[2][16][16] = { { R16(ROW_RR, fjt_rrx_32) }, { R16(ROW_RR, fjt_rrx_64) } };   // [64?][D][S]
