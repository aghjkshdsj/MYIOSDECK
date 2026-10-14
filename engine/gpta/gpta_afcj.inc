// SPDX-License-Identifier: GPL-3.0-or-later
// One scalar arithmetic instruction plus comparison of its result and JCC.
// The independent corpus selected the family. Preserve instruction semantics;
// no guest loop is replaced with a host loop.
#include "gpta_pin.h"
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
#define AF_C20(M) C16(M) M(16) M(17) M(18) M(19)
// The translator marks only actual adjacency in each individual trace copy.
// The final backedge and unproved links keep the ordinary chained dispatch.
#define AF_CHAIN(LINK,MISS,HINT) do { \
    if (u->cc & (HINT)) PNEXT(); \
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
#define AF_DEF(C,S,D,CC) AF_DEF_EXPAND(C,S,D,CC,AF_NAME)
#define AF_DEF_EXPAND(C,S,D,CC,N) AF_DEF2(C,S,D,CC,N)
#define AF_DEF2(C,S,D,CC,N) \
    PH p_af_##N##_##CC##_##D##_##S##_##C(GPTA_PARAMS) { \
        GptaV result = AF_RESULT(XD_##D,XS_##S), other = AF_C_##C; \
        if (AF_ID_##C == AF_ID_##D && (AF_ID_##D != 8 || u->base == u->dst)) other = result; \
        XW_##D(result); double a = AF_LOW(result), b = AF_LOW(other); \
        AF_SELECT(CC); \
        fcj_taken: AF_CHAIN(u->ulink,p_miss_t,GPTA_AF_ADJ_T); \
        fcj_fall: AF_CHAIN(u->ulink2,p_miss_f,GPTA_AF_ADJ_F); }
#define AF_SRC(S,D,CC) AF_X9C(AF_DEF,S,D,CC)
#define AF_DST(D,CC) X9B(AF_SRC,D,CC)
#define AF_CC(CC,...) X9(AF_DST,CC)
AF_C20(AF_CC)
#define AF_E(C,S,D,CC) AF_E_EXPAND(C,S,D,CC,AF_NAME)
#define AF_E_EXPAND(C,S,D,CC,N) AF_E2(C,S,D,CC,N)
#define AF_E2(C,S,D,CC,N) p_af_##N##_##CC##_##D##_##S##_##C,
#define AF_ROW(S,D,CC) { AF_X9C(AF_E,S,D,CC) },
#define AF_PLANE(D,CC) { X9B(AF_ROW,D,CC) },
#define AF_COND(CC,...) { X9(AF_PLANE,CC) },
const PFn AF_TABLE[20][9][9][9] = { AF_C20(AF_COND) };
