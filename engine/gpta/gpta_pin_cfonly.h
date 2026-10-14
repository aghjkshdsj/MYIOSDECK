// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_CFONLY_H
#define GPTA_PIN_CFONLY_H
// Uop.fdir bit: only CF from this ALU writer remains live. Branch edge bits
// and GPTA_NOBOUND are separate. Windows retains full flag state.
#define GPTA_CFONLY 8
extern const PFn gpta_t_cf_rr[5][2][16][16], gpta_t_cf_rr_r1[5][2][16][16];
extern const PFn gpta_t_cf_ri[5][2][16], gpta_t_cf_ri_r1[5][2][16];
#define t_cf_rr GPTA_T(t_cf_rr)
#define t_cf_ri GPTA_T(t_cf_ri)
#endif
