// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME addss
#define AF_TABLE t_af_addss
#define AF_LOW(V) LF0(V)
#define AF_RESULT(D,S) gpta_scalar_addss(D,S)
#include "gpta_afcj.inc"
