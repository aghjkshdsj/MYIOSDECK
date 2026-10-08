// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: general-register loads with a [base + index*scale + disp] operand in one uop (specialised
// on base, index and destination; the scale and displacement come from the uop), and movzx of a
// byte at [base + disp]. Stores are in fxr_pin_mem2.c. See fxr_pin.c.

#include "fxr_pin.h"

#define BIX(B, I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_LDI(D, I, B)                                                                   \
    PH p_ldi32_##B##_##I##_##D(FXR_PARAMS) { g##D = ld32(BIX(B, I)); PNEXT(); }            \
    PH p_ldi64_##B##_##I##_##D(FXR_PARAMS) { g##D = ld64(BIX(B, I)); PNEXT(); }            \
    PH p_ldiz8_##B##_##I##_##D(FXR_PARAMS) { g##D = ld8(BIX(B, I)); PNEXT(); }
#define DEF_LDI_I(I, B) R16C(DEF_LDI, I, B)
#define DEF_LDI_B(B, _) R16B(DEF_LDI_I, B)
R17(DEF_LDI_B, _)
#define E_LDI(D, I, B, NAME) p_##NAME##_##B##_##I##_##D,
#define ROW_LDI_I(I, B, NAME) { R16C(E_LDI, I, B, NAME) },
#define ROW_LDI_B(B, NAME) { R16B(ROW_LDI_I, B, NAME) },
#define T_LDI(NAME) { R17(ROW_LDI_B, NAME) },
const PFn t_ldi[3][17][16][16] = { T_LDI(ldi32) T_LDI(ldi64) T_LDI(ldiz8) };   // [FL_LD32|FL_LD64|FL_LDZ8][base][index][D]

// movzx r, byte [base + disp]
#define DEF_LDZ8S(D, B) PH p_ldz8s_##B##_##D(FXR_PARAMS) { g##D = ld8(g##B + (uint64_t)u->disp); PNEXT(); }
#define DEF_LDZ8S_B(B, _) R16B(DEF_LDZ8S, B)
R17(DEF_LDZ8S_B, _)
#define E_LDZ8S(D, B) p_ldz8s_##B##_##D,
#define ROW_LDZ8S(B, _) { R16B(E_LDZ8S, B) },
const PFn t_ldz8s[17][16] = { R17(ROW_LDZ8S, _) };
