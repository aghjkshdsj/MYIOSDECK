// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_ASM_SALU_H
#define GPTA_PIN_ASM_SALU_H
#include "gpta_pin_asm_pload.h"
#if GPTA_USE_ASM_PLOAD
#define GPTA_ASM_SALU_TABLES(M) \
    M(t_asal_shl_add) M(t_asal_shl_sub) M(t_asal_shl_and) M(t_asal_shl_or) M(t_asal_shl_xor) \
    M(t_asal_shr_add) M(t_asal_shr_sub) M(t_asal_shr_and) M(t_asal_shr_or) M(t_asal_shr_xor) \
    M(t_asal_sar_add) M(t_asal_sar_sub) M(t_asal_sar_and) M(t_asal_sar_or) M(t_asal_sar_xor) \
    M(t_asal_rol_add) M(t_asal_rol_sub) M(t_asal_rol_and) M(t_asal_rol_or) M(t_asal_rol_xor) \
    M(t_asal_ror_add) M(t_asal_ror_sub) M(t_asal_ror_and) M(t_asal_ror_or) M(t_asal_ror_xor)
#define ASAL_DECLARE(N) extern const PFn gpta_##N[2][64][16][16], gpta_##N##_r1[2][64][16][16];
GPTA_ASM_SALU_TABLES(ASAL_DECLARE)
#undef ASAL_DECLARE
#else
#define GPTA_ASM_SALU_TABLES(M)
#endif
#endif
