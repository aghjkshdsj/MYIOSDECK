// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_FCJ_SELECT_H
#define GPTA_FCJ_SELECT_H
#include "gpta_fcj_cond.h"
#if defined(__aarch64__)
// ARM unordered NZCV=0011 matches x86 COMIS conditions through these branches.
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
#define FCJ_SELECT(CC) do { if (fcj_cond(CC,a,b)) goto fcj_taken; goto fcj_fall; } while (0)
#endif
#endif
