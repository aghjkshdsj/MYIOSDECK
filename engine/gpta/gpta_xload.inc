// SPDX-License-Identifier: GPL-3.0-or-later
// Corpus-selected pair: load an XMM register, then arithmetic on that register.
// A single guest access precedes every visible effect, preserving its fault
// state. Keep the old destination live through that access. If S aliases D,
// the second instruction reads the loaded value, not the entry value of D.
#include "gpta_pin.h"
#include "gpta_pin_xload.h"
#define XL_addps(D,S) ((GptaV)((VF)(D) + (VF)(S)))
#define XL_subps(D,S) ((GptaV)((VF)(D) - (VF)(S)))
#define XL_mulps(D,S) ((GptaV)((VF)(D) * (VF)(S)))
#define XL_addpd(D,S) ((GptaV)((VD)(D) + (VD)(S)))
#define XL_subpd(D,S) ((GptaV)((VD)(D) - (VD)(S)))
#define XL_mulpd(D,S) ((GptaV)((VD)(D) * (VD)(S)))
#define XL_addss(D,S) xlo_f(D,LF0(D) + LF0(S))
#define XL_subss(D,S) xlo_f(D,LF0(D) - LF0(S))
#define XL_mulss(D,S) xlo_f(D,LF0(D) * LF0(S))
#define XL_addsd(D,S) xlo_d(D,LD0(D) + LD0(S))
#define XL_subsd(D,S) xlo_d(D,LD0(D) - LD0(S))
#define XL_mulsd(D,S) xlo_d(D,LD0(D) * LD0(S))
#define XL_KEEP_0 XKEEP_0
#define XL_KEEP_1 XKEEP_1
#define XL_KEEP_2 XKEEP_2
#define XL_KEEP_3 XKEEP_3
#define XL_KEEP_4 XKEEP_4
#define XL_KEEP_5 XKEEP_5
#define XL_KEEP_6 XKEEP_6
#define XL_KEEP_7 XKEEP_7
#define XL_KEEP_8 ((void)0)
#define XL_KEEP_9 ((void)0)
#define XL_KEEP_10 ((void)0)
#define XL_KEEP_11 ((void)0)
#define XL_KEEP_12 ((void)0)
#define XL_KEEP_13 ((void)0)
#define XL_KEEP_14 ((void)0)
#define XL_KEEP_15 ((void)0)
#define XL_DEF(B,S,D,OP,W) \
    PH p_xload_##OP##_##D##_##S##_##B(GPTA_PARAMS) { \
        GptaV d = xldn(g##B + (uint64_t)u->disp,W); \
        XL_KEEP_##D; \
        GptaV s = (S) == (D) ? d : XS_##S; XW_##D(XL_##OP(d,s)); PNEXT(); }
#define XL_SRC(S,D,OP,W) R17C(XL_DEF,S,D,OP,W)
#define XL_DST(D,OP,W) R16B(XL_SRC,D,OP,W)
#define XL_OP(OP,W) R16(XL_DST,OP,W)
XL_LIST(XL_OP)
#define XL_E(B,S,D,OP) p_xload_##OP##_##D##_##S##_##B,
#define XL_ROW(S,D,OP) { R17C(XL_E,S,D,OP) },
#define XL_PLANE(D,OP) { R16B(XL_ROW,D,OP) },
#define XL_ENTRY(OP,W) { R16(XL_PLANE,OP) },
const PFn XL_TABLE[3][16][16][17] = { XL_LIST(XL_ENTRY) };
