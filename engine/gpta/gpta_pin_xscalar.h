// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_XSCALAR_H
#define GPTA_PIN_XSCALAR_H
extern const PFn gpta_t_xscalar_load[2][16][17][17], gpta_t_xscalar_store[2][16][17][17];
extern const PFn gpta_t_xscalar_load_r1[2][16][17][17], gpta_t_xscalar_store_r1[2][16][17][17];
#define t_xscalar_load GPTA_T(t_xscalar_load)
#define t_xscalar_store GPTA_T(t_xscalar_store)
#endif
