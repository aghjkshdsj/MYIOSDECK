// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME subsd
#define AF_TABLE t_af_subsd
#define AF_LOW(V) LD0(V)
#define AF_RESULT(D,S) xlo_d(D,LD0(D) - LD0(S))
#include "gpta_afcj.inc"
