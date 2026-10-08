// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: general-register loads and stores with a [base + index*scale + disp] operand in one uop
// (specialised on base, index and data register; the scale and displacement come from the uop).
// Without them the address takes a separate uop (T). See fxr_pin.c.

#include "fxr_pin.h"

#define BIX(B, I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_LDST(D, I, B)                                                                  \
    PH p_ldi32_##B##_##I##_##D(FXR_PARAMS) { g##D = ld32(BIX(B, I)); PNEXT(); }            \
    PH p_ldi64_##B##_##I##_##D(FXR_PARAMS) { g##D = ld64(BIX(B, I)); PNEXT(); }            \
    PH p_ldiz8_##B##_##I##_##D(FXR_PARAMS) { g##D = ld8(BIX(B, I)); PNEXT(); }             \
    PH p_sti8_##B##_##I##_##D(FXR_PARAMS) { st8(BIX(B, I), g##D); PNEXT(); }               \
    PH p_sti32_##B##_##I##_##D(FXR_PARAMS) { st32(BIX(B, I), g##D); PNEXT(); }             \
    PH p_sti64_##B##_##I##_##D(FXR_PARAMS) { st64(BIX(B, I), g##D); PNEXT(); }
#define DEF_LDST_I(I, B) R16C(DEF_LDST, I, B)
#define DEF_LDST_B(B, _) R16B(DEF_LDST_I, B)
R17(DEF_LDST_B, _)
#define E_LDST(D, I, B, NAME) p_##NAME##_##B##_##I##_##D,
#define ROW_LDST_I(I, B, NAME) { R16C(E_LDST, I, B, NAME) },
#define ROW_LDST_B(B, NAME) { R16B(ROW_LDST_I, B, NAME) },
#define T_LDST(NAME) { R17(ROW_LDST_B, NAME) },
const PFn t_ldi[FL_COUNT][17][16][16] = { T_LDST(ldi32) T_LDST(ldi64) T_LDST(ldiz8) T_LDST(sti8) T_LDST(sti32) T_LDST(sti64) };

// movzx r, byte [base + disp]
#define DEF_LDZ8S(D, B) PH p_ldz8s_##B##_##D(FXR_PARAMS) { g##D = ld8(g##B + (uint64_t)u->disp); PNEXT(); }
#define DEF_LDZ8S_B(B, _) R16B(DEF_LDZ8S, B)
R17(DEF_LDZ8S_B, _)
#define E_LDZ8S(D, B) p_ldz8s_##B##_##D,
#define ROW_LDZ8S(B, _) { R16B(E_LDZ8S, B) },
const PFn t_ldz8s[17][16] = { R17(ROW_LDZ8S, _) };
