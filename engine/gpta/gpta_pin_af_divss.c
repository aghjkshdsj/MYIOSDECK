// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME divss
#define AF_TABLE t_af_divss
#define AF_LOW(V) LF0(V)
#define AF_RESULT(D,S) xlo_f(D,LF0(D) / LF0(S))
#include "gpta_afcj.inc"
