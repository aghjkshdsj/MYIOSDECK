// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpta_pin.h"
#include "gpta_pin_eareuse.h"
#define EX_DEF(I,B,X,W) EX_DEF2(I,B,X,W,EX_SCALE)
#define EX_DEF2(I,B,X,W,SC) \
    PH p_xf_##SC##_##W##_##X##_##B##_##I(GPTA_PARAMS) { \
        T = g##B + (g##I << SC) + (uint64_t)u->disp; \
        GptaV v = xldn(T,W); XKEEP_##X; XW_##X(v); PNEXT(); }
#define EX_BASE(B,X,W) R17C(EX_DEF,B,X,W)
#define EX_X(X,W) R17B(EX_BASE,X,W)
X9(EX_X,16) X9(EX_X,4) X9(EX_X,8)
#define EX_E(I,B,X,W) EX_E2(I,B,X,W,EX_SCALE)
#define EX_E2(I,B,X,W,SC) p_xf_##SC##_##W##_##X##_##B##_##I,
#define EX_ROW(B,X,W) { R17C(EX_E,B,X,W) },
#define EX_PLANE(X,W) { R17B(EX_ROW,X,W) },
#define EX_TABLE_W(W) { X9(EX_PLANE,W) }
const PFn EX_TABLE[3][9][17][17] = { EX_TABLE_W(16),EX_TABLE_W(4),EX_TABLE_W(8) };
