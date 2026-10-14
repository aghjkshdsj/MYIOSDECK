// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME mulsd
#define AF_TABLE t_af_mulsd
#define AF_LOW(V) LD0(V)
#define AF_RESULT(D,S) gpta_scalar_mulsd(D,S)
#include "gpta_afcj.inc"
