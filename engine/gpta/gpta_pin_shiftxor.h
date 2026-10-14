// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_SHIFTXOR_H
#define GPTA_PIN_SHIFTXOR_H
#define GPTA_SHIFTXOR_TABLES(M) M(t_sx_shl) M(t_sx_shr) M(t_sx_sar) M(t_sx_rol) M(t_sx_ror)
#define SX_DECL(N) extern const PFn gpta_##N[2][2][2][16][16], gpta_##N##_r1[2][2][2][16][16];
GPTA_SHIFTXOR_TABLES(SX_DECL)
#undef SX_DECL
#define t_sx_shl GPTA_T(t_sx_shl)
#define t_sx_shr GPTA_T(t_sx_shr)
#define t_sx_sar GPTA_T(t_sx_sar)
#define t_sx_rol GPTA_T(t_sx_rol)
#define t_sx_ror GPTA_T(t_sx_ror)
#endif
