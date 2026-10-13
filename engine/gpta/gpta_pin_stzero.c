// SPDX-License-Identifier: GPL-3.0-or-later
// Immediate stores with no decoded displacement. The immediate remains data;
// no value, address, or surrounding guest instructions are special-cased.
#include "gpta_pin.h"
#include "gpta_pin_stzero.h"

#define DEF_STIZ(I,B,SC) \
    PH psz_8_##SC##_##B##_##I(GPTA_PARAMS) { st8(g##B + (g##I << SC), u->imm); PNEXT(); } \
    PH psz_16_##SC##_##B##_##I(GPTA_PARAMS) { st16(g##B + (g##I << SC), u->imm); PNEXT(); } \
    PH psz_32_##SC##_##B##_##I(GPTA_PARAMS) { st32(g##B + (g##I << SC), u->imm); PNEXT(); } \
    PH psz_64_##SC##_##B##_##I(GPTA_PARAMS) { st64(g##B + (g##I << SC), u->imm); PNEXT(); }
#define DEF_STIZ_B(B,SC) R17B(DEF_STIZ,B,SC)
R17(DEF_STIZ_B,0) R17(DEF_STIZ_B,1) R17(DEF_STIZ_B,2) R17(DEF_STIZ_B,3)
#define E_STIZ(I,B,NAME) NAME##_##B##_##I,
#define ROW_STIZ(B,NAME) { R17B(E_STIZ,B,NAME) },
#define TABLE_STIZ(SC) { { R17(ROW_STIZ,psz_8_##SC) }, { R17(ROW_STIZ,psz_16_##SC) }, \
    { R17(ROW_STIZ,psz_32_##SC) }, { R17(ROW_STIZ,psz_64_##SC) } }
const PFn t_stiz[4][4][17][17] = {
    TABLE_STIZ(0), TABLE_STIZ(1), TABLE_STIZ(2), TABLE_STIZ(3)
};
