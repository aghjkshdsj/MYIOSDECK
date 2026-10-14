// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_LOADPAIR_H
#define GPTA_PIN_LOADPAIR_H
#define GPTA_DIRECT_LOAD_PAIR 32   // first uop; private immutable lowering metadata
#define LP_FIRST_TABLES(M) M(t_lp_first0) M(t_lp_first1) M(t_lp_first2) M(t_lp_first3)
#define LP_NEXT_TABLES(M) M(t_lp_next0) M(t_lp_next1) M(t_lp_next2) M(t_lp_next3)
#define LP_DECLARE_FIRST(N) extern const PFn gpta_##N[9][9][17][17], gpta_##N##_r1[9][9][17][17];
#define LP_DECLARE_NEXT(N) extern const PFn gpta_##N[9][17][17], gpta_##N##_r1[9][17][17];
LP_FIRST_TABLES(LP_DECLARE_FIRST)
LP_NEXT_TABLES(LP_DECLARE_NEXT)
#undef LP_DECLARE_FIRST
#undef LP_DECLARE_NEXT
#define t_lp_first0 GPTA_T(t_lp_first0)
#define t_lp_first1 GPTA_T(t_lp_first1)
#define t_lp_first2 GPTA_T(t_lp_first2)
#define t_lp_first3 GPTA_T(t_lp_first3)
#define t_lp_next0 GPTA_T(t_lp_next0)
#define t_lp_next1 GPTA_T(t_lp_next1)
#define t_lp_next2 GPTA_T(t_lp_next2)
#define t_lp_next3 GPTA_T(t_lp_next3)
#endif
