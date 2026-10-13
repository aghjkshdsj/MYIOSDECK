// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_ALU_MEM_H
#define GPTA_PIN_ALU_MEM_H
#define GPTA_AM_TABLES(X) X(t_am_add) X(t_am_sub) X(t_am_and) X(t_am_or) X(t_am_xor)
#define AM_DECLARE(N) extern const PFn gpta_##N[2][2][17][17][16], gpta_##N##_r1[2][2][17][17][16];
GPTA_AM_TABLES(AM_DECLARE)
#undef AM_DECLARE
#define t_am_add GPTA_T(t_am_add)
#define t_am_sub GPTA_T(t_am_sub)
#define t_am_and GPTA_T(t_am_and)
#define t_am_or GPTA_T(t_am_or)
#define t_am_xor GPTA_T(t_am_xor)
#endif
