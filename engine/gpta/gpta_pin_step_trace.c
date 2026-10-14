// SPDX-License-Identifier: GPL-3.0-or-later
// Existing register/immediate add/sub + compare/branch forms with a successor
// proven adjacent by trace construction. Every guest step and test still runs.
#include "gpta_pin.h"
#include "gpta_pin_step_noadj.h"

#define STEP_OP_add(a, b) ((a) + (b))
#define STEP_OP_sub(a, b) ((a) - (b))
#define STEP_TRACE_BR(SZ, CC, EDGE, D) do { \
    int taken = cc_sub(CC, g##D, (uint64_t)u->disp & M##SZ, SI##SZ); \
    if (taken) { if ((EDGE) == 0) PNEXT(); PCHAIN(u->ulink, p_miss_t); } \
    if ((EDGE) == 1) PNEXT(); PCHAIN(u->ulink2, p_miss_f); \
} while (0)
#define DEF_STI(D, CC, OP, SZ, EDGE) \
    PH p_sti_##OP##_##SZ##_##CC##_##D##_##EDGE(GPTA_PARAMS) { \
        g##D = STEP_OP_##OP(g##D, (uint64_t)(int64_t)u->fimm) & M##SZ; \
        STEP_TRACE_BR(SZ, CC, EDGE, D); }
#define DEF_STR(S, D, CC, OP, SZ, EDGE) \
    PH p_str_##OP##_##SZ##_##CC##_##D##_##S##_##EDGE(GPTA_PARAMS) { \
        g##D = STEP_OP_##OP(g##D, g##S) & M##SZ; \
        STEP_TRACE_BR(SZ, CC, EDGE, D); }
#define DEF_STR_D(D, CC, OP, SZ, EDGE) R16B(DEF_STR, D, CC, OP, SZ, EDGE)
#define DEF_ST_CC(CC, OP, SZ, EDGE) R16(DEF_STI, CC, OP, SZ, EDGE) R16(DEF_STR_D, CC, OP, SZ, EDGE)
#define DEF_ST_EDGE(EDGE) C10(DEF_ST_CC, add, 32, EDGE) C10(DEF_ST_CC, add, 64, EDGE) \
    C10(DEF_ST_CC, sub, 32, EDGE) C10(DEF_ST_CC, sub, 64, EDGE)
DEF_ST_EDGE(0) DEF_ST_EDGE(1) DEF_ST_EDGE(2)

#define E_STI(D, CC, OP, SZ, EDGE) p_sti_##OP##_##SZ##_##CC##_##D##_##EDGE,
#define E_STR(S, D, CC, OP, SZ, EDGE) p_str_##OP##_##SZ##_##CC##_##D##_##S##_##EDGE,
#define ROW_STR(D, CC, OP, SZ, EDGE) { R16B(E_STR, D, CC, OP, SZ, EDGE) },
#define CC_STI(CC, OP, SZ, EDGE) { R16(E_STI, CC, OP, SZ, EDGE) },
#define CC_STR(CC, OP, SZ, EDGE) { R16(ROW_STR, CC, OP, SZ, EDGE) },
#define T_ST(K, EDGE) { { { C10(CC_##K, add, 32, EDGE) }, { C10(CC_##K, add, 64, EDGE) } }, \
    { { C10(CC_##K, sub, 32, EDGE) }, { C10(CC_##K, sub, 64, EDGE) } } }
const PFn t_sii_trace[2][2][2][10][16] = { T_ST(STI, 0), T_ST(STI, 1) };
const PFn t_sri_trace[2][2][2][10][16][16] = { T_ST(STR, 0), T_ST(STR, 1) };
// EDGE=2 proves neither edge adjacent. The ordinary block and final trace
// backedge use links without checking whether either equals u+1.
const PFn t_sii_noadj[2][2][10][16] = T_ST(STI, 2);
const PFn t_sri_noadj[2][2][10][16][16] = T_ST(STR, 2);
