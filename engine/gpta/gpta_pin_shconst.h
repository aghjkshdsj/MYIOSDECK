// SPDX-License-Identifier: GPL-3.0-or-later
// Separate declarations avoid recompiling unrelated handler families.
#ifndef GPTA_PIN_SHCONST_H
#define GPTA_PIN_SHCONST_H
extern const PFn gpta_t_shconst[5][2][64][16];
extern const PFn gpta_t_shconst_r1[5][2][64][16];
#define t_shconst GPTA_T(t_shconst)
#endif
