// SPDX-License-Identifier: GPL-3.0-or-later
// Full immediate domain for the existing corpus-selected copy/shift pair.
#ifndef GPTA_PIN_COPYSHIFT_H
#define GPTA_PIN_COPYSHIFT_H
extern const PFn gpta_t_csh_shl[2][64][16][16];
extern const PFn gpta_t_csh_shl_r1[2][64][16][16];
#define t_csh_shl GPTA_T(t_csh_shl)
extern const PFn gpta_t_csh_shr[2][64][16][16];
extern const PFn gpta_t_csh_shr_r1[2][64][16][16];
#define t_csh_shr GPTA_T(t_csh_shr)
extern const PFn gpta_t_csh_sar[2][64][16][16];
extern const PFn gpta_t_csh_sar_r1[2][64][16][16];
#define t_csh_sar GPTA_T(t_csh_sar)
extern const PFn gpta_t_csh_rol[2][64][16][16];
extern const PFn gpta_t_csh_rol_r1[2][64][16][16];
#define t_csh_rol GPTA_T(t_csh_rol)
extern const PFn gpta_t_csh_ror[2][64][16][16];
extern const PFn gpta_t_csh_ror_r1[2][64][16][16];
#define t_csh_ror GPTA_T(t_csh_ror)
#endif
