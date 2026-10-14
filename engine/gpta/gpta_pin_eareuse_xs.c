// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpta_pin.h"
#include "gpta_pin_eareuse.h"
#define EX_SECOND(X,W,Z) \
    PH p_xs_##W##_##Z##_##X(GPTA_PARAMS) { \
        GptaV v = xldn(T + ((Z) ? 0 : u->imm),W); \
        XKEEP_##X; XW_##X(v); PNEXT(); }
#define EX_Z(Z,W) X9(EX_SECOND,W,Z)
EX_Z(0,16) EX_Z(1,16) EX_Z(0,4) EX_Z(1,4) EX_Z(0,8) EX_Z(1,8)
#define EX_SE(X,W,Z) p_xs_##W##_##Z##_##X,
#define EX_SZ(W) { { X9(EX_SE,W,0) }, { X9(EX_SE,W,1) } }
const PFn t_xs[3][2][9] = { EX_SZ(16),EX_SZ(4),EX_SZ(8) };
