// SPDX-License-Identifier: GPL-3.0-or-later
// One x86 instruction: register op [base + index*scale + displacement].
// Each operation is a separate translation unit to bound compiler memory.
#include "gpta_pin.h"
#include "gpta_pin_alu_mem.h"
#define AM_add(a,b) ((a)+(b))
#define AM_sub(a,b) ((a)-(b))
#define AM_and(a,b) ((a)&(b))
#define AM_or(a,b) ((a)|(b))
#define AM_xor(a,b) ((a)^(b))
#define AMK_add LF_ADD
#define AMK_sub LF_SUB
#define AMK_and LF_LOGIC
#define AMK_or LF_LOGIC
#define AMK_xor LF_LOGIC
#define AM_KEEP_F KEEP_F()
#define AM_KEEP_N ((void)0)
#define DEF_AM(D,I,B,SZ,FL,OP) \
    PH p_am_##OP##_##SZ##FL##_##B##_##I##_##D(GPTA_PARAMS) { \
        uint64_t b = ld##SZ(g##B + (g##I << u->scale) + (uint64_t)u->disp); \
        KEEP_R(D); AM_KEEP_##FL; \
        uint64_t a = g##D & M##SZ, r = AM_##OP(a,b) & M##SZ; \
        g##D = r; \
        if (FLV_##FL) SETF(AMK_##OP,SI##SZ,a,b,r,0); \
        PNEXT(); }
#define DEF_AM_I(I,B,SZ,FL,OP) R16C(DEF_AM,I,B,SZ,FL,OP)
#define DEF_AM_B(B,SZ,FL,OP) R17B(DEF_AM_I,B,SZ,FL,OP)
#define DEF_AM_OP(OP) \
    R17(DEF_AM_B,32,N,OP) R17(DEF_AM_B,64,N,OP) \
    R17(DEF_AM_B,32,F,OP) R17(DEF_AM_B,64,F,OP)
DEF_AM_OP(GPTA_AM_OP)
#define E_AM(D,I,B,SZ,FL,OP) p_am_##OP##_##SZ##FL##_##B##_##I##_##D,
#define ROW_AM_I(I,B,SZ,FL,OP) { R16C(E_AM,I,B,SZ,FL,OP) },
#define ROW_AM_B(B,SZ,FL,OP) { R17B(ROW_AM_I,B,SZ,FL,OP) },
#define TABLE_AM_FL(OP,FL) { { R17(ROW_AM_B,32,FL,OP) }, { R17(ROW_AM_B,64,FL,OP) } }
#define TABLE_AM(OP) { TABLE_AM_FL(OP,N), TABLE_AM_FL(OP,F) }
const PFn GPTA_AM_TABLE[2][2][17][17][16] = TABLE_AM(GPTA_AM_OP);
