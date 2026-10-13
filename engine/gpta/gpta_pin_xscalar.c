// SPDX-License-Identifier: GPL-3.0-or-later
// One handler for scalar SSE memory moves with any base/index/scale/disp.
// Fixed XMM operands also avoid runtime selection of the high register bank.
#include "gpta_pin.h"
#include "gpta_pin_xscalar.h"

#if GPTA_PACKED_XMM
#define XKEEP_8 XKEEP_0
#define XKEEP_9 XKEEP_1
#define XKEEP_10 XKEEP_2
#define XKEEP_11 XKEEP_3
#define XKEEP_12 XKEEP_4
#define XKEEP_13 XKEEP_5
#define XKEEP_14 XKEEP_6
#define XKEEP_15 XKEEP_7
#else
#define XKEEP_8 ((void)0)
#define XKEEP_9 ((void)0)
#define XKEEP_10 ((void)0)
#define XKEEP_11 ((void)0)
#define XKEEP_12 ((void)0)
#define XKEEP_13 ((void)0)
#define XKEEP_14 ((void)0)
#define XKEEP_15 ((void)0)
#endif
#define XSC_EA(B,I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_XSC(I,B,X,SZ) \
    PH xsc_load##SZ##_##X##_##B##_##I(GPTA_PARAMS) { \
        uint64_t v = ld##SZ(XSC_EA(B,I)); XKEEP_##X; \
        XW_##X(((GptaV){ v, 0 })); PNEXT(); } \
    PH xsc_store##SZ##_##X##_##B##_##I(GPTA_PARAMS) { \
        st##SZ(XSC_EA(B,I), (XS_##X)[0]); PNEXT(); }
#define DEF_XSC_B(B,X,SZ) R17B(DEF_XSC,B,X,SZ)
#define DEF_XSC_X(X,SZ) R17(DEF_XSC_B,X,SZ)
R16(DEF_XSC_X,32) R16(DEF_XSC_X,64)
#define E_XSC(I,B,X,N) N##_##X##_##B##_##I,
#define ROW_XSC(B,X,N) { R17B(E_XSC,B,X,N) },
#define TABLE_XSC(X,N) { R17(ROW_XSC,X,N) },
const PFn t_xscalar_load[2][16][17][17] = {
    { R16(TABLE_XSC,xsc_load32) }, { R16(TABLE_XSC,xsc_load64) }
};
const PFn t_xscalar_store[2][16][17][17] = {
    { R16(TABLE_XSC,xsc_store32) }, { R16(TABLE_XSC,xsc_store64) }
};
