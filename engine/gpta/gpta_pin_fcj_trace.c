// SPDX-License-Identifier: GPL-3.0-or-later
// Register COMIS/UCOMIS + JCC with one successor proven to be the next uop.
// Each guest comparison/branch still executes; no guest loop is replaced.
#include "gpta_pin.h"
#include "gpta_fcj_cond.h"

#if defined(__aarch64__)
// Keep a scalar comparison in the host condition flags. Clang otherwise uses
// a vector comparison, narrowing and a vector-to-GPR transfer before branching.
// FCMP unordered is NZCV=0011: LT includes unordered, GE excludes it; LE
// includes unordered and GT excludes it. x86 ZF additionally includes unordered.
#define FCJ_ASM(COND) do { \
    __asm__ goto volatile("fcmp %d0, %d1\n\tb." COND " %l[fcj_taken]" \
        : : "w"(a), "w"(b) : "cc" : fcj_taken); \
    goto fcj_fall; \
} while (0)
#define FCJ_SELECT(CC) do { \
    switch (CC) { \
    case 0: case 8: case 12: goto fcj_fall; \
    case 1: case 9: case 13: goto fcj_taken; \
    case 2: FCJ_ASM("lt"); case 3: FCJ_ASM("ge"); \
    case 6: FCJ_ASM("le"); case 7: FCJ_ASM("gt"); \
    case 10: FCJ_ASM("vs"); case 11: FCJ_ASM("vc"); \
    case 4: case 14: \
        __asm__ goto volatile("fcmp %d0, %d1\n\tb.eq %l[fcj_taken]\n\tb.vs %l[fcj_taken]" \
            : : "w"(a), "w"(b) : "cc" : fcj_taken); \
        goto fcj_fall; \
    default: \
        __asm__ goto volatile("fcmp %d0, %d1\n\tb.eq %l[fcj_fall]\n\tb.vs %l[fcj_fall]" \
            : : "w"(a), "w"(b) : "cc" : fcj_fall); \
        goto fcj_taken; \
    } \
} while (0)
#else
#define FCJ_SELECT(CC) do { if (fcj_cond(CC, a, b)) goto fcj_taken; goto fcj_fall; } while (0)
#endif
#define FCJ_TRACE(CC, EDGE) do { \
    FCJ_SELECT(CC); \
    fcj_taken: if ((EDGE) == 0) PNEXT(); PCHAIN(u->ulink, p_miss_t); \
    fcj_fall: if ((EDGE) == 1) PNEXT(); PCHAIN(u->ulink2, p_miss_f); \
} while (0)
#define DEF_FCT(S, D, CC, EDGE) \
    PH p_fctd_##EDGE##_##CC##_##D##_##S(GPTA_PARAMS) { \
        double a = LD0(XD_##D), b = LD0(XS_##S); FCJ_TRACE(CC, EDGE); } \
    PH p_fcts_##EDGE##_##CC##_##D##_##S(GPTA_PARAMS) { \
        double a = LF0(XD_##D), b = LF0(XS_##S); FCJ_TRACE(CC, EDGE); }
#define DEF_FCT_D(D, CC, EDGE) R16B(DEF_FCT, D, CC, EDGE)
#define DEF_FCT_CC(CC, EDGE) R16(DEF_FCT_D, CC, EDGE)
C16(DEF_FCT_CC, 0) C16(DEF_FCT_CC, 1)
#define E_FCT(S, D, CC, EDGE, TY) p_fct##TY##_##EDGE##_##CC##_##D##_##S,
#define ROW_FCT_D(D, CC, EDGE, TY) { R16B(E_FCT, D, CC, EDGE, TY) },
#define ROW_FCT_CC(CC, EDGE, TY) { R16(ROW_FCT_D, CC, EDGE, TY) },
#define T_FCT(EDGE) { { C16(ROW_FCT_CC, EDGE, s) }, { C16(ROW_FCT_CC, EDGE, d) } }
const PFn t_fcj_trace[2][2][16][16][16] = { T_FCT(0), T_FCT(1) };
