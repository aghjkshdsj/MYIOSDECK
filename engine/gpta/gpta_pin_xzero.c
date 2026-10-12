// SPDX-License-Identifier: GPL-3.0-or-later
// SSE moves with base + scaled index and no displacement. Scale zero already
// has this form in gpta_pin_sse.c. Select once at decode, not in the handler.
#include "gpta_pin.h"
#include "gpta_pin_xzero.h"

#define DEF_XZ(I,B,X,S) \
    PH xl_zero##S##_##B##_##I##_##X(GPTA_PARAMS) { \
        XW_##X(xld((const void *)(uintptr_t)(g##B + (g##I << S)))); PNEXT(); } \
    PH xs_zero##S##_##B##_##I##_##X(GPTA_PARAMS) { \
        xst((void *)(uintptr_t)(g##B + (g##I << S)), XS_##X); PNEXT(); }
#define DEF_XZ_B(B,X,S) R17B(DEF_XZ,B,X,S)
#define DEF_XZ_X(X,S) R17(DEF_XZ_B,X,S)
X9(DEF_XZ_X,1) X9(DEF_XZ_X,2) X9(DEF_XZ_X,3)
#define E_XZ(I,B,X,NAME) NAME##_##B##_##I##_##X,
#define ROW_XZ(B,X,NAME) { R17B(E_XZ,B,X,NAME) },
#define TABLE_XZ(X,NAME) { R17(ROW_XZ,X,NAME) },
const PFn t_xlz[3][9][17][17] = {
    { X9(TABLE_XZ,xl_zero1) }, { X9(TABLE_XZ,xl_zero2) }, { X9(TABLE_XZ,xl_zero3) }
};
const PFn t_xsz[3][9][17][17] = {
    { X9(TABLE_XZ,xs_zero1) }, { X9(TABLE_XZ,xs_zero2) }, { X9(TABLE_XZ,xs_zero3) }
};
