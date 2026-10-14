// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_EAREUSE_H
#define GPTA_PIN_EAREUSE_H
extern const PFn gpta_t_rf[3][17][17][16], gpta_t_rf_r1[3][17][17][16];
extern const PFn gpta_t_rs[3][2][16], gpta_t_rs_r1[3][2][16];
#define GPTA_EAREUSE_XTABLES(M) M(t_xf0) M(t_xf1) M(t_xf2) M(t_xf3)
#define ER_DECL(N) extern const PFn gpta_##N[3][9][17][17], gpta_##N##_r1[3][9][17][17];
GPTA_EAREUSE_XTABLES(ER_DECL)
#undef ER_DECL
extern const PFn gpta_t_xs[3][2][9], gpta_t_xs_r1[3][2][9];
#define t_rf GPTA_T(t_rf)
#define t_rs GPTA_T(t_rs)
#define t_xf0 GPTA_T(t_xf0)
#define t_xf1 GPTA_T(t_xf1)
#define t_xf2 GPTA_T(t_xf2)
#define t_xf3 GPTA_T(t_xf3)
#define t_xs GPTA_T(t_xs)
#endif
