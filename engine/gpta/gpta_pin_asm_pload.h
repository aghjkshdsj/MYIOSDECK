// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_ASM_PLOAD_H
#define GPTA_PIN_ASM_PLOAD_H
#include "gpta_asm_layout.h"
#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none) && !GPTA_PACKED_XMM
#define GPTA_USE_ASM_PLOAD 1
#define GPTA_ASM_PLOAD_TABLES(M) M(t_apl_paddb) M(t_apl_paddw) M(t_apl_paddd) M(t_apl_paddq) M(t_apl_psubb) M(t_apl_psubw) M(t_apl_psubd) M(t_apl_psubq) M(t_apl_pand) M(t_apl_pandn) M(t_apl_por) M(t_apl_pxor)
#define APL_DECLARE(N) extern const PFn gpta_##N[4][8][8][17][17], gpta_##N##_r1[4][8][8][17][17];
GPTA_ASM_PLOAD_TABLES(APL_DECLARE)
#undef APL_DECLARE
#define t_apl_paddb GPTA_T(t_apl_paddb)
#define t_apl_paddw GPTA_T(t_apl_paddw)
#define t_apl_paddd GPTA_T(t_apl_paddd)
#define t_apl_paddq GPTA_T(t_apl_paddq)
#define t_apl_psubb GPTA_T(t_apl_psubb)
#define t_apl_psubw GPTA_T(t_apl_psubw)
#define t_apl_psubd GPTA_T(t_apl_psubd)
#define t_apl_psubq GPTA_T(t_apl_psubq)
#define t_apl_pand GPTA_T(t_apl_pand)
#define t_apl_pandn GPTA_T(t_apl_pandn)
#define t_apl_por GPTA_T(t_apl_por)
#define t_apl_pxor GPTA_T(t_apl_pxor)
#else
#define GPTA_USE_ASM_PLOAD 0
#define GPTA_ASM_PLOAD_TABLES(M)
#endif
#endif
