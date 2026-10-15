// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_ASM_FLOAD_H
#define GPTA_PIN_ASM_FLOAD_H
#include "gpta_pin_asm_pload.h"
#if GPTA_USE_ASM_PLOAD
#define GPTA_ASM_FLOAD_TABLES(M) \
    M(t_afl_addps) M(t_afl_subps) M(t_afl_mulps) \
    M(t_afl_addpd) M(t_afl_subpd) M(t_afl_mulpd) \
    M(t_afl_addss) M(t_afl_subss) M(t_afl_mulss) \
    M(t_afl_addsd) M(t_afl_subsd) M(t_afl_mulsd)
#define AFL_DECLARE(N) extern const PFn gpta_##N[4][8][8][17][17], gpta_##N##_r1[4][8][8][17][17];
GPTA_ASM_FLOAD_TABLES(AFL_DECLARE)
#undef AFL_DECLARE
#define t_afl_addps GPTA_T(t_afl_addps)
#define t_afl_subps GPTA_T(t_afl_subps)
#define t_afl_mulps GPTA_T(t_afl_mulps)
#define t_afl_addpd GPTA_T(t_afl_addpd)
#define t_afl_subpd GPTA_T(t_afl_subpd)
#define t_afl_mulpd GPTA_T(t_afl_mulpd)
#define t_afl_addss GPTA_T(t_afl_addss)
#define t_afl_subss GPTA_T(t_afl_subss)
#define t_afl_mulss GPTA_T(t_afl_mulss)
#define t_afl_addsd GPTA_T(t_afl_addsd)
#define t_afl_subsd GPTA_T(t_afl_subsd)
#define t_afl_mulsd GPTA_T(t_afl_mulsd)
#else
#define GPTA_ASM_FLOAD_TABLES(M)
#endif
#endif
