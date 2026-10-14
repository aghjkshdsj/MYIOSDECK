// SPDX-License-Identifier: GPL-3.0-or-later
#define AF_NAME mulss
#define AF_TABLE t_af_mulss
#define AF_LOW(V) LF0(V)
#define AF_RESULT(D,S) xlo_f(D,LF0(D) * LF0(S))
#include "gpta_afcj.inc"
