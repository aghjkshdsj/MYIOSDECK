// SPDX-License-Identifier: GPL-3.0-or-later
// One scalar arithmetic instruction plus comparison of its result and JCC.
// The independent corpus selected the family. Preserve instruction semantics;
// no guest loop is replaced with a host loop.
#include "gpta_pin.h"
#include "gpta_scalar_fp.h"
#include "gpta_pin_afcj.h"
#include "gpta_fcj_select.h"
#define AF_C_0 XD_0
#define AF_C_1 XD_1
#define AF_C_2 XD_2
#define AF_C_3 XD_3
#define AF_C_4 XD_4
#define AF_C_5 XD_5
#define AF_C_6 XD_6
#define AF_C_7 XD_7
#define AF_C_M xld(&c->gpta_xhi[u->base-8])
#define AF_ID_0 0
#define AF_ID_1 1
#define AF_ID_2 2
#define AF_ID_3 3
#define AF_ID_4 4
#define AF_ID_5 5
#define AF_ID_6 6
#define AF_ID_7 7
#define AF_ID_M 8
#define AF_X9C(F,...) F(0,__VA_ARGS__) F(1,__VA_ARGS__) F(2,__VA_ARGS__) F(3,__VA_ARGS__) \
    F(4,__VA_ARGS__) F(5,__VA_ARGS__) F(6,__VA_ARGS__) F(7,__VA_ARGS__) F(M,__VA_ARGS__)
#define AF_C20(M,E) C16(M,E) M(16,E) M(17,E) M(18,E) M(19,E)
// The translator marks only actual adjacency in each individual trace copy.
// The final backedge and unproved links keep the ordinary chained dispatch.
#if defined(__aarch64__)
// Start the adjacent dispatch load before FP work, without another ABI
// argument. Tied vector outputs enforce that ordering even for pure FP
// instructions, while leaving their actual values unchanged. The load is
// from immutable interpreter-owned uop storage, not guest memory.
#define AF_PRELOAD(EDGE) \
    Uop *af_next_ = u+1; PFn af_fn_ = 0; \
    if ((EDGE) != 0) { \
        __asm__ volatile("ldr %0, [%9, #%c10]" \
            : "=&r"(af_fn_), "+w"(x0), "+w"(x1), "+w"(x2), "+w"(x3), \
              "+w"(x4), "+w"(x5), "+w"(x6), "+w"(x7) \
            : "r"(u), "i"(sizeof(Uop)) : "memory"); \
    }
#else
#define AF_PRELOAD(EDGE) Uop *af_next_ = u+1; PFn af_fn_ = (EDGE) ? af_next_->p : 0
#endif
#define AF_CHAIN(LINK,MISS,ADJ) do { \
    if (ADJ) { GPTA_COUNT(af_next_); __attribute__((musttail)) return af_fn_ GPTA_CALL(af_next_); } \
    PCHAIN(LINK,MISS); \
} while (0)
#if defined(__aarch64__)
#define AF_SELECT(CC) do { \
    if ((CC) == 16) FCJ_ASM("hi"); \
    else if ((CC) == 17) FCJ_ASM("ls"); \
    else if ((CC) == 18) FCJ_ASM("hs"); \
    else if ((CC) == 19) FCJ_ASM("lo"); \
    else FCJ_SELECT(CC); \
} while (0)
#else
#define AF_SELECT(CC) do { \
    if ((CC) < 16) { FCJ_SELECT(CC); } \
    else { int take = (CC) == 16 ? !(a <= b) : (CC) == 17 ? a <= b : \
        (CC) == 18 ? !(a < b) : a < b; \
        if (take) goto fcj_taken; goto fcj_fall; } \
} while (0)
#endif
#define AF_DEF(C,S,D,CC,E) AF_DEF_EXPAND(C,S,D,CC,E,AF_NAME)
#define AF_DEF_EXPAND(C,S,D,CC,E,N) AF_DEF2(C,S,D,CC,E,N)
#define AF_DEF2(C,S,D,CC,E,N) \
    PH p_af_##N##_##CC##_##D##_##S##_##C##_##E(GPTA_PARAMS) { \
        AF_PRELOAD(E); \
        GptaV result = AF_RESULT(XD_##D,XS_##S), other = AF_C_##C; \
        if (AF_ID_##C == AF_ID_##D && (AF_ID_##D != 8 || u->base == u->dst)) other = result; \
        XW_##D(result); double a = AF_LOW(result), b = AF_LOW(other); \
        AF_SELECT(CC); \
        fcj_taken: AF_CHAIN(u->ulink,p_miss_t,(E)==1); \
        fcj_fall: AF_CHAIN(u->ulink2,p_miss_f,(E)==2); }
#define AF_SRC(S,D,CC,E) AF_X9C(AF_DEF,S,D,CC,E)
#define AF_DST(D,CC,E) X9B(AF_SRC,D,CC,E)
#define AF_CC(CC,E) X9(AF_DST,CC,E)
AF_C20(AF_CC,0) AF_C20(AF_CC,1) AF_C20(AF_CC,2)
#define AF_E(C,S,D,CC,E) AF_E_EXPAND(C,S,D,CC,E,AF_NAME)
#define AF_E_EXPAND(C,S,D,CC,E,N) AF_E2(C,S,D,CC,E,N)
#define AF_E2(C,S,D,CC,E,N) p_af_##N##_##CC##_##D##_##S##_##C##_##E,
#define AF_ROW(S,D,CC,E) { AF_X9C(AF_E,S,D,CC,E) },
#define AF_PLANE(D,CC,E) { X9B(AF_ROW,D,CC,E) },
#define AF_COND(CC,E) { X9(AF_PLANE,CC,E) },
const PFn AF_TABLE[3][20][9][9][9] = {
    { AF_C20(AF_COND,0) }, { AF_C20(AF_COND,1) }, { AF_C20(AF_COND,2) } };
