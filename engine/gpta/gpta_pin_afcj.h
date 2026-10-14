// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_AFCJ_H
#define GPTA_PIN_AFCJ_H
#define GPTA_AF_BRANCH 128     // private Uop.fdir marker
#define GPTA_AFCJ_TABLES(M) M(t_af_addss) M(t_af_subss) M(t_af_mulss) M(t_af_divss) \
    M(t_af_addsd) M(t_af_subsd) M(t_af_mulsd) M(t_af_divsd)
// 0..15 are ordinary COMIS conditions; 16..19 are the four ordered/unordered
// conditions needed when the arithmetic result was the second operand.
// Layout 0: both edges chained; 1: taken adjacent; 2: fallthrough adjacent.
#define AF_DECL(N) extern const PFn gpta_##N[3][20][9][9][9], gpta_##N##_r1[3][20][9][9][9];
GPTA_AFCJ_TABLES(AF_DECL)
#undef AF_DECL
#define t_af_addss GPTA_T(t_af_addss)
#define t_af_subss GPTA_T(t_af_subss)
#define t_af_mulss GPTA_T(t_af_mulss)
#define t_af_divss GPTA_T(t_af_divss)
#define t_af_addsd GPTA_T(t_af_addsd)
#define t_af_subsd GPTA_T(t_af_subsd)
#define t_af_mulsd GPTA_T(t_af_mulsd)
#define t_af_divsd GPTA_T(t_af_divsd)
#endif
