// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_XLOAD_H
#define GPTA_PIN_XLOAD_H
#define GPTA_XLOAD_TABLES(M) M(t_xload_ps) M(t_xload_pd) M(t_xload_ss) M(t_xload_sd)
#define XL_DECLARE(N) extern const PFn gpta_##N[3][16][16][17], gpta_##N##_r1[3][16][16][17];
GPTA_XLOAD_TABLES(XL_DECLARE)
#undef XL_DECLARE
#define t_xload_ps GPTA_T(t_xload_ps)
#define t_xload_pd GPTA_T(t_xload_pd)
#define t_xload_ss GPTA_T(t_xload_ss)
#define t_xload_sd GPTA_T(t_xload_sd)
#endif
