// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_XLOAD_H
#define GPTA_PIN_XLOAD_H
#define GPTA_XLOAD_BASE_TABLES(M) M(t_xload_ps) M(t_xload_pd) M(t_xload_ss) M(t_xload_sd)
#define GPTA_XLOAD_INDEX_TABLES(M) M(t_xloadi_ps) M(t_xloadi_pd) M(t_xloadi_ss) M(t_xloadi_sd)
#define GPTA_XLOAD_TABLES(M) GPTA_XLOAD_BASE_TABLES(M) GPTA_XLOAD_INDEX_TABLES(M)
#define XL_DECLARE(N) extern const PFn gpta_##N[3][16][16][17], gpta_##N##_r1[3][16][16][17];
GPTA_XLOAD_BASE_TABLES(XL_DECLARE)
#undef XL_DECLARE
#define XLI_DECLARE(N) extern const PFn gpta_##N[3][9][9][17][16], gpta_##N##_r1[3][9][9][17][16];
GPTA_XLOAD_INDEX_TABLES(XLI_DECLARE)
#undef XLI_DECLARE
#define t_xload_ps GPTA_T(t_xload_ps)
#define t_xload_pd GPTA_T(t_xload_pd)
#define t_xload_ss GPTA_T(t_xload_ss)
#define t_xload_sd GPTA_T(t_xload_sd)
#define t_xloadi_ps GPTA_T(t_xloadi_ps)
#define t_xloadi_pd GPTA_T(t_xloadi_pd)
#define t_xloadi_ss GPTA_T(t_xloadi_ss)
#define t_xloadi_sd GPTA_T(t_xloadi_sd)
#endif
