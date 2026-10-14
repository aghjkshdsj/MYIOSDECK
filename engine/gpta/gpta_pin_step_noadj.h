// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_STEP_NOADJ_H
#define GPTA_PIN_STEP_NOADJ_H
extern const PFn gpta_t_sii_noadj[2][2][10][16], gpta_t_sii_noadj_r1[2][2][10][16];
extern const PFn gpta_t_sri_noadj[2][2][10][16][16], gpta_t_sri_noadj_r1[2][2][10][16][16];
#define t_sii_noadj GPTA_T(t_sii_noadj)
#define t_sri_noadj GPTA_T(t_sri_noadj)
#endif
