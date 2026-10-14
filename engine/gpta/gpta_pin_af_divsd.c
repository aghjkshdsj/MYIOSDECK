// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME divsd
#define AF_TABLE t_af_divsd
#define AF_LOW(V) LD0(V)
#define AF_RESULT(D,S) gpta_scalar_divsd(D,S)
#include "gpta_afcj.inc"
