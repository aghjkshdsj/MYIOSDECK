// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME addsd
#define AF_TABLE t_af_addsd
#define AF_LOW(V) LD0(V)
#define AF_RESULT(D,S) gpta_scalar_addsd(D,S)
#include "gpta_afcj.inc"
