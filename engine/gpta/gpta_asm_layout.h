// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_ASM_LAYOUT_H
#define GPTA_ASM_LAYOUT_H
#define GPTA_ASM_DISP 32
#ifdef GPTA_PROFILE
#define GPTA_ASM_UOP_SIZE 88
#define GPTA_ASM_PROF 80
#else
#define GPTA_ASM_UOP_SIZE 80
#endif
#ifndef __ASSEMBLER__
_Static_assert(__builtin_offsetof(Uop,p) == 0, "static assembly dispatch pointer");
_Static_assert(sizeof(Uop) == GPTA_ASM_UOP_SIZE, "static assembly uop stride");
_Static_assert(__builtin_offsetof(Uop,disp) == GPTA_ASM_DISP, "static assembly displacement");
#ifdef GPTA_PROFILE
_Static_assert(__builtin_offsetof(Uop,prof) == GPTA_ASM_PROF, "static assembly profile counter");
#endif
#endif
#endif
