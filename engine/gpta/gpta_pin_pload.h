// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_PIN_PLOAD_H
#define GPTA_PIN_PLOAD_H
#define GPTA_PLOAD_TABLES(M) M(t_pload_paddb) M(t_pload_paddw) M(t_pload_paddd) M(t_pload_paddq) M(t_pload_psubb) M(t_pload_psubw) M(t_pload_psubd) M(t_pload_psubq) M(t_pload_pand) M(t_pload_pandn) M(t_pload_por) M(t_pload_pxor)
#define PL_DECLARE(N) extern const PFn gpta_##N[9][9][17][17], gpta_##N##_r1[9][9][17][17];
GPTA_PLOAD_TABLES(PL_DECLARE)
#undef PL_DECLARE
#define t_pload_paddb GPTA_T(t_pload_paddb)
#define t_pload_paddw GPTA_T(t_pload_paddw)
#define t_pload_paddd GPTA_T(t_pload_paddd)
#define t_pload_paddq GPTA_T(t_pload_paddq)
#define t_pload_psubb GPTA_T(t_pload_psubb)
#define t_pload_psubw GPTA_T(t_pload_psubw)
#define t_pload_psubd GPTA_T(t_pload_psubd)
#define t_pload_psubq GPTA_T(t_pload_psubq)
#define t_pload_pand GPTA_T(t_pload_pand)
#define t_pload_pandn GPTA_T(t_pload_pandn)
#define t_pload_por GPTA_T(t_pload_por)
#define t_pload_pxor GPTA_T(t_pload_pxor)
// Complete wrapping add/sub lane widths and bitwise family. The matcher
// uses instruction semantics only; no program identities or operand values.
static inline int gpta_pload_index(const char *name) {
    static const char *const names[] = { "paddb","paddw","paddd","paddq","psubb","psubw","psubd","psubq","pand","pandn","por","pxor" };
    for (int i = 0; i < 12; i++) if (!strcmp(name,names[i])) return i;
    return -1;
}
#endif
