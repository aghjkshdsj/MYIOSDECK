// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA: general-register stores (8/32/64-bit) with a [base + index*scale + disp] operand in one uop,
// specialised on base, index and source register. See gpta_pin_mem.c and gpta_pin.c.

#include "gpta_pin.h"

#define BIX(B, I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_STX(D, I, B)                                                                   \
    PH p_stx8_##B##_##I##_##D(GPTA_PARAMS) { st8(BIX(B, I), g##D); PNEXT(); }               \
    PH p_stx32_##B##_##I##_##D(GPTA_PARAMS) { st32(BIX(B, I), g##D); PNEXT(); }             \
    PH p_stx64_##B##_##I##_##D(GPTA_PARAMS) { st64(BIX(B, I), g##D); PNEXT(); }
#define DEF_STX_I(I, B) R16C(DEF_STX, I, B)
#define DEF_STX_B(B, _) R16B(DEF_STX_I, B)
R17(DEF_STX_B, _)
#define E_STX(D, I, B, NAME) p_##NAME##_##B##_##I##_##D,
#define ROW_STX_I(I, B, NAME) { R16C(E_STX, I, B, NAME) },
#define ROW_STX_B(B, NAME) { R16B(ROW_STX_I, B, NAME) },
#define T_STX(NAME) { R17(ROW_STX_B, NAME) },
const PFn t_stx[3][17][16][16] = { T_STX(stx8) T_STX(stx32) T_STX(stx64) };   // [8|32|64][base][index][src]
