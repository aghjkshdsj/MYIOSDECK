// SPDX-License-Identifier: GPL-3.0-or-later
// Single x86 SSE arithmetic instructions with memory operands. Address calculation
// and arithmetic share a dispatch, for any guest instruction with these forms.
// No guest sequence, loop, address, constant or program identity is recognized.
#include "gpta_pin.h"

// Separate translation units keep each compiler process bounded as the static
// scale specializations grow. Only scale zero also emits base-only handlers.
#ifndef GPTA_XMEM_SCALE
#define GPTA_XMEM_SCALE 0
#endif
#if GPTA_XMEM_SCALE == 0
#define XM_TABLE t_xmemi0
#elif GPTA_XMEM_SCALE == 1
#define XM_TABLE t_xmemi1
#elif GPTA_XMEM_SCALE == 2
#define XM_TABLE t_xmemi2
#else
#define XM_TABLE t_xmemi3
#endif

#define XM_addps(D, S) ((GptaV)((VF)(D) + (VF)(S)))
#define XM_subps(D, S) ((GptaV)((VF)(D) - (VF)(S)))
#define XM_mulps(D, S) ((GptaV)((VF)(D) * (VF)(S)))
#define XM_addpd(D, S) ((GptaV)((VD)(D) + (VD)(S)))
#define XM_subpd(D, S) ((GptaV)((VD)(D) - (VD)(S)))
#define XM_mulpd(D, S) ((GptaV)((VD)(D) * (VD)(S)))
#define XM_addss(D, S) xlo_f(D, LF0(D) + LF0(S))
#define XM_subss(D, S) xlo_f(D, LF0(D) - LF0(S))
#define XM_mulss(D, S) xlo_f(D, LF0(D) * LF0(S))
#define XM_addsd(D, S) xlo_d(D, LD0(D) + LD0(S))
#define XM_subsd(D, S) xlo_d(D, LD0(D) - LD0(S))
#define XM_mulsd(D, S) xlo_d(D, LD0(D) * LD0(S))
#define XM_LIST(M) M(addps,16) M(subps,16) M(mulps,16) \
    M(addpd,16) M(subpd,16) M(mulpd,16) M(addss,4) M(subss,4) M(mulss,4) \
    M(addsd,8) M(subsd,8) M(mulsd,8)

// Keep the original destination live through the load: a fault must still expose
// the state before this instruction. The disassembly precision gate checks it.
#define XM_INDEX(I, B, D, OP, W, SC) \
    PH xmi##SC##_##OP##_##D##_##B##_##I(GPTA_PARAMS) { \
        GptaV s = xldn(g##B + (g##I << SC) + (uint64_t)u->disp, W); XKEEP_##D; \
        XW_##D(XM_##OP(XD_##D, s)); PNEXT(); }
#define XM_BASE(B, D, OP, W, SC) R17B(XM_INDEX, B, D, OP, W, SC)
#define XM_DST(D, OP, W, SC) R17(XM_BASE, D, OP, W, SC)
#define XM_DEF(OP, W) X9(XM_DST, OP, W, GPTA_XMEM_SCALE)
XM_LIST(XM_DEF)

#if GPTA_XMEM_SCALE == 0
#define XM_BBASE(B, D, OP, W) \
    PH xmb_##OP##_##D##_##B(GPTA_PARAMS) { \
        GptaV s = xldn(g##B + (uint64_t)u->disp, W); XKEEP_##D; \
        XW_##D(XM_##OP(XD_##D, s)); PNEXT(); }
#define XM_BDST(D, OP, W) R17(XM_BBASE, D, OP, W)
#define XM_BDEF(OP, W) X9(XM_BDST, OP, W)
XM_LIST(XM_BDEF)
#endif

#define XM_ELEM(I, B, D, OP, SC) xmi##SC##_##OP##_##D##_##B##_##I,
#define XM_ROW(B, D, OP, SC) { R17B(XM_ELEM, B, D, OP, SC) },
#define XM_PLANE(D, OP, SC) { R17(XM_ROW, D, OP, SC) },
#define XM_ENTRY(OP, W) { X9(XM_PLANE, OP, GPTA_XMEM_SCALE) },
const PFn XM_TABLE[12][9][17][17] = { XM_LIST(XM_ENTRY) };
#if GPTA_XMEM_SCALE == 0
#define XM_BELEM(B, D, OP) xmb_##OP##_##D##_##B,
#define XM_BROW(D, OP) { R17(XM_BELEM, D, OP) },
#define XM_BENTRY(OP, W) { X9(XM_BROW, OP) },
const PFn t_xmemb[12][9][17] = { XM_LIST(XM_BENTRY) };
#endif
