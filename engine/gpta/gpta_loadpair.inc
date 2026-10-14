// SPDX-License-Identifier: GPL-3.0-or-later
// Consecutive vector loads selected from independent corpus evidence. Each
// load retains its own uop, guest boundary and recoverable handler entry.
// Only the first dispatch becomes a statically compiled direct tail branch.
#include "gpta_pin.h"
#include "gpta_pin_loadpair.h"
#define LP_I17(F,...) F(0,__VA_ARGS__) F(1,__VA_ARGS__) F(2,__VA_ARGS__) F(3,__VA_ARGS__) \
    F(4,__VA_ARGS__) F(5,__VA_ARGS__) F(6,__VA_ARGS__) F(7,__VA_ARGS__) \
    F(8,__VA_ARGS__) F(9,__VA_ARGS__) F(10,__VA_ARGS__) F(11,__VA_ARGS__) \
    F(12,__VA_ARGS__) F(13,__VA_ARGS__) F(14,__VA_ARGS__) F(15,__VA_ARGS__) F(Z,__VA_ARGS__)
// A known direct callee must explicitly consume its pre-load destination.
// Otherwise interprocedural optimization may discard the caller's first
// result when both loads write the same register. The empty barriers bind
// the pinned register (or expose the high bank) before the faulting access.
#define LP_KEEP_0 XKEEP_0
#define LP_KEEP_1 XKEEP_1
#define LP_KEEP_2 XKEEP_2
#define LP_KEEP_3 XKEEP_3
#define LP_KEEP_4 XKEEP_4
#define LP_KEEP_5 XKEEP_5
#define LP_KEEP_6 XKEEP_6
#define LP_KEEP_7 XKEEP_7
#define LP_KEEP_M __asm__ volatile("" ::: "memory")
#define LP_NEXT(I,B,E,SC) LP_NEXT2(I,B,E,SC)
#define LP_NEXT2(I,B,E,SC) \
    PH __attribute__((noinline)) p_lp_next##SC##_##E##_##B##_##I(GPTA_PARAMS) { \
        LP_KEEP_##E; \
        XW_##E(xld((const void *)(uintptr_t)(g##B + (g##I << SC) + (uint64_t)u->disp))); PNEXT(); }
#define LP_NEXT_B(B,E,SC) R17B(LP_NEXT,B,E,SC)
#define LP_NEXT_E(E,SC) R17(LP_NEXT_B,E,SC)
X9(LP_NEXT_E,LP_SCALE)
#define LP_FIRST(I,B,E,D,SC) LP_FIRST2(I,B,E,D,SC)
#define LP_FIRST2(I,B,E,D,SC) \
    PH p_lp_first##SC##_##D##_##E##_##B##_##I(GPTA_PARAMS) { \
        XW_##D(xld((const void *)(uintptr_t)(g##B + (g##I << SC) + (uint64_t)u->disp))); \
        GPTA_COUNT(u+1); \
        __attribute__((musttail)) return p_lp_next##SC##_##E##_##B##_##I GPTA_CALL(u+1); }
#define LP_FIRST_B(B,E,D,SC) LP_I17(LP_FIRST,B,E,D,SC)
#define LP_FIRST_E(E,D,SC) R17C(LP_FIRST_B,E,D,SC)
#define LP_FIRST_D(D,SC) X9B(LP_FIRST_E,D,SC)
X9(LP_FIRST_D,LP_SCALE)
#define LP_E_NEXT(I,B,E,SC) LP_E_NEXT2(I,B,E,SC)
#define LP_E_NEXT2(I,B,E,SC) p_lp_next##SC##_##E##_##B##_##I,
#define LP_ROW_NEXT(B,E,SC) { R17B(LP_E_NEXT,B,E,SC) },
#define LP_PLANE_NEXT(E,SC) { R17(LP_ROW_NEXT,E,SC) },
const PFn LP_NEXT_TABLE[9][17][17] = { X9(LP_PLANE_NEXT,LP_SCALE) };
#define LP_E_FIRST(I,B,E,D,SC) LP_E_FIRST2(I,B,E,D,SC)
#define LP_E_FIRST2(I,B,E,D,SC) p_lp_first##SC##_##D##_##E##_##B##_##I,
#define LP_ROW_FIRST(B,E,D,SC) { LP_I17(LP_E_FIRST,B,E,D,SC) },
#define LP_PLANE_FIRST(E,D,SC) { R17C(LP_ROW_FIRST,E,D,SC) },
#define LP_CUBE_FIRST(D,SC) { X9B(LP_PLANE_FIRST,D,SC) },
const PFn LP_FIRST_TABLE[9][9][17][17] = { X9(LP_CUBE_FIRST,LP_SCALE) };
