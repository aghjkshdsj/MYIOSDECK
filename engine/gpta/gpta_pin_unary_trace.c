// SPDX-License-Identifier: GPL-3.0-or-later
// Existing INC/DEC + ZF/SF branch pair, with one trace edge proven adjacent.
// Flag reconstruction on other edges remains in the existing exit stubs.
#include "gpta_pin.h"
#include "gpta_pin_unary_trace.h"
#define UT_inc(A) ((A) + 1)
#define UT_dec(A) ((A) - 1)
#define UT_BRANCH(CC,SZ,EDGE) do { \
    if (cc_logic(CC,r,SI##SZ)) { \
        if ((EDGE) == 0) PNEXT(); PCHAIN(u->ulink,p_miss_t); \
    } \
    if ((EDGE) == 1) PNEXT(); PCHAIN(u->ulink2,p_miss_f); \
} while (0)
#define UT_DEF(D,CC,SZ,OP,EDGE) \
    PH p_ut_##OP##_##SZ##_##CC##_##D##_##EDGE(GPTA_PARAMS) { \
        uint64_t r = UT_##OP(g##D) & M##SZ; g##D = r; UT_BRANCH(CC,SZ,EDGE); }
#define UT_CC(CC,SZ,OP,EDGE) R16(UT_DEF,CC,SZ,OP,EDGE)
#define UT_CCS(M,...) M(4,__VA_ARGS__) M(5,__VA_ARGS__) M(8,__VA_ARGS__) M(9,__VA_ARGS__)
#define UT_EDGE(EDGE) UT_CCS(UT_CC,32,dec,EDGE) UT_CCS(UT_CC,64,dec,EDGE) \
    UT_CCS(UT_CC,32,inc,EDGE) UT_CCS(UT_CC,64,inc,EDGE)
UT_EDGE(0) UT_EDGE(1) UT_EDGE(2)
#define UT_E(D,CC,SZ,OP,EDGE) p_ut_##OP##_##SZ##_##CC##_##D##_##EDGE,
#define UT_ROW(CC,SZ,OP,EDGE) { R16(UT_E,CC,SZ,OP,EDGE) },
#define UT_TABLE(EDGE) { \
    { { UT_CCS(UT_ROW,32,dec,EDGE) }, { UT_CCS(UT_ROW,64,dec,EDGE) } }, \
    { { UT_CCS(UT_ROW,32,inc,EDGE) }, { UT_CCS(UT_ROW,64,inc,EDGE) } } }
const PFn t_fid_trace[3][2][2][4][16] = { UT_TABLE(0), UT_TABLE(1), UT_TABLE(2) };
