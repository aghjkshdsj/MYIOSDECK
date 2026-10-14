// SPDX-License-Identifier: GPL-3.0-or-later
// Register COMIS/UCOMIS + JCC with one successor proven to be the next uop.
// Each guest comparison/branch still executes; no guest loop is replaced.
#include "gpta_pin.h"
#include "gpta_fcj_select.h"
#define FCJ_TRACE(CC, EDGE) do { \
    FCJ_SELECT(CC); \
    fcj_taken: if ((EDGE) == 0) PNEXT(); PCHAIN(u->ulink, p_miss_t); \
    fcj_fall: if ((EDGE) == 1) PNEXT(); PCHAIN(u->ulink2, p_miss_f); \
} while (0)
#define DEF_FCT(S, D, CC, EDGE) \
    PH p_fctd_##EDGE##_##CC##_##D##_##S(GPTA_PARAMS) { \
        double a = LD0(XD_##D), b = LD0(XS_##S); FCJ_TRACE(CC, EDGE); } \
    PH p_fcts_##EDGE##_##CC##_##D##_##S(GPTA_PARAMS) { \
        double a = LF0(XD_##D), b = LF0(XS_##S); FCJ_TRACE(CC, EDGE); }
#define DEF_FCT_D(D, CC, EDGE) R16B(DEF_FCT, D, CC, EDGE)
#define DEF_FCT_CC(CC, EDGE) R16(DEF_FCT_D, CC, EDGE)
C16(DEF_FCT_CC, 0) C16(DEF_FCT_CC, 1)
#define E_FCT(S, D, CC, EDGE, TY) p_fct##TY##_##EDGE##_##CC##_##D##_##S,
#define ROW_FCT_D(D, CC, EDGE, TY) { R16B(E_FCT, D, CC, EDGE, TY) },
#define ROW_FCT_CC(CC, EDGE, TY) { R16(ROW_FCT_D, CC, EDGE, TY) },
#define T_FCT(EDGE) { { C16(ROW_FCT_CC, EDGE, s) }, { C16(ROW_FCT_CC, EDGE, d) } }
const PFn t_fcj_trace[2][2][16][16][16] = { T_FCT(0), T_FCT(1) };
