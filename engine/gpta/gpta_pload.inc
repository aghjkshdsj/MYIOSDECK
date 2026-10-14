// SPDX-License-Identifier: GPL-3.0-or-later
// Independent-corpus pair: full vector load followed by packed integer ALU
// on its result. One guest access precedes every architectural side effect.
#include "gpta_pin.h"
#include "gpta_pin_pload.h"
#define PL_paddb(D,S) ((GptaV)((VU16)(D) + (VU16)(S)))
#define PL_paddw(D,S) ((GptaV)((VU8)(D) + (VU8)(S)))
#define PL_paddd(D,S) ((GptaV)((VU4)(D) + (VU4)(S)))
#define PL_paddq(D,S) ((D) + (S))
#define PL_psubb(D,S) ((GptaV)((VU16)(D) - (VU16)(S)))
#define PL_psubw(D,S) ((GptaV)((VU8)(D) - (VU8)(S)))
#define PL_psubd(D,S) ((GptaV)((VU4)(D) - (VU4)(S)))
#define PL_psubq(D,S) ((D) - (S))
#define PL_pand(D,S) ((D) & (S))
#define PL_pandn(D,S) (~(D) & (S))
#define PL_por(D,S) ((D) | (S))
#define PL_pxor(D,S) ((D) ^ (S))
#if defined(__aarch64__)
#define PL_LOADED(V) __asm__ volatile("" : "+w"(V) :: "memory")
#else
#define PL_LOADED(V) __asm__ volatile("" : "+x"(V) :: "memory")
#endif
#define PL_ID_0 0
#define PL_ID_1 1
#define PL_ID_2 2
#define PL_ID_3 3
#define PL_ID_4 4
#define PL_ID_5 5
#define PL_ID_6 6
#define PL_ID_7 7
#define PL_ID_M 8
#define PL_DEF(I,B,S,D,OP) PL_DEF_I(I,B,S,D,OP)
#define PL_DEF_I(I,B,S,D,OP) \
    PH p_pload_##OP##_##D##_##S##_##B##_##I(GPTA_PARAMS) { \
        GptaV d = xld((const void *)(uintptr_t)(g##B + (g##I << u->scale) + (uint64_t)u->disp)); \
        /* Even self-cancelling ALU must execute the preceding guest load. */ \
        PL_LOADED(d); XKEEP_##D; \
        int alias = PL_ID_##D == PL_ID_##S && (PL_ID_##D != 8 || u->dst == u->src); \
        GptaV s = alias ? d : XS_##S; XW_##D(PL_##OP(d,s)); PNEXT(); }
#define PL_BASE(B,S,D,OP) R17B(PL_DEF,B,S,D,OP)
#define PL_SRC(S,D,OP) R17C(PL_BASE,S,D,OP)
#define PL_DST(D,OP) X9B(PL_SRC,D,OP)
X9(PL_DST,PL_OP)
#define PL_E(I,B,S,D,OP) PL_E_I(I,B,S,D,OP)
#define PL_E_I(I,B,S,D,OP) p_pload_##OP##_##D##_##S##_##B##_##I,
#define PL_ROW(B,S,D,OP) { R17B(PL_E,B,S,D,OP) },
#define PL_PLANE(S,D,OP) { R17C(PL_ROW,S,D,OP) },
#define PL_CUBE(D,OP) { X9B(PL_PLANE,D,OP) },
const PFn PL_TABLE[9][9][17][17] = { X9(PL_CUBE,PL_OP) };
