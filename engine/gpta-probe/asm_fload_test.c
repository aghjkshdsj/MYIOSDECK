// SPDX-License-Identifier: GPL-3.0-or-later
// Native correctness matrix, never used to select handlers or report speed.
#include "../gpta/gpta_pin.h"
#include "../gpta/gpta_pin_asm_fload.h"
#if !GPTA_USE_ASM_PLOAD
#error ARM64 preserve_none is required for this gate
#endif

static GPTA_CC void finish(GPTA_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X(); c->exc_info[0] = T;
}
#define TABLE_SET(S) { \
    gpta_t_afl_addps##S,gpta_t_afl_subps##S,gpta_t_afl_mulps##S, \
    gpta_t_afl_addpd##S,gpta_t_afl_subpd##S,gpta_t_afl_mulpd##S, \
    gpta_t_afl_addss##S,gpta_t_afl_subss##S,gpta_t_afl_mulss##S, \
    gpta_t_afl_addsd##S,gpta_t_afl_subsd##S,gpta_t_afl_mulsd##S }
static const PFn (*const tables[2][12])[8][8][17][17] = {
    TABLE_SET(), TABLE_SET(_r1)
};
#undef TABLE_SET

// Scalar C reference deliberately does not use the generator's NEON code.
static void reference(unsigned char out[16], int op, const unsigned char a[16], const unsigned char b[16]) {
    unsigned kind = (unsigned)op/3, operation = (unsigned)op%3;
    memset(out,0,16);
    if (kind == 0 || kind == 2) {
        for (unsigned lane = 0; lane < (kind == 0 ? 4u : 1u); lane++) {
            float x,y,r;
            memcpy(&x,a+4*lane,4); memcpy(&y,b+4*lane,4);
            r = operation == 0 ? x+y : operation == 1 ? x-y : x*y;
            memcpy(out+4*lane,&r,4);
        }
    } else {
        for (unsigned lane = 0; lane < (kind == 1 ? 2u : 1u); lane++) {
            double x,y,r;
            memcpy(&x,a+8*lane,8); memcpy(&y,b+8*lane,8);
            r = operation == 0 ? x+y : operation == 1 ? x-y : x*y;
            memcpy(out+8*lane,&r,8);
        }
    }
}
static void make_vector(unsigned char out[16], int is_double, int pattern, int ordinal, int loaded) {
    static const uint32_t f_special[4] = { UINT32_C(0x80000000), UINT32_C(0x7f800000), UINT32_C(0xff800000), 0 };
    static const uint64_t d_special[2] = { UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000) };
    for (int lane = 0; lane < (is_double ? 2 : 4); lane++) {
        double value = (double)(ordinal+1)*0.625 + (double)(lane-2)*1.75;
        if (loaded) value = -value;
        if (is_double) {
            uint64_t bits; memcpy(&bits,&value,8);
            if ((pattern == 1 && loaded) || (pattern == 2 && !loaded))
                bits = UINT64_C(0x7ff8000000012345) + (unsigned)lane;
            if (pattern == 3) bits = d_special[(lane+loaded)&1];
            memcpy(out+8*lane,&bits,8);
        } else {
            float scalar = (float)value; uint32_t bits; memcpy(&bits,&scalar,4);
            if ((pattern == 1 && loaded) || (pattern == 2 && !loaded))
                bits = UINT32_C(0x7fc12345) + (unsigned)lane;
            if (pattern == 3) bits = f_special[(lane+loaded)&3];
            memcpy(out+4*lane,&bits,4);
        }
    }
}
int main(void) {
    FxiCpu c; memset(&c,0,sizeof c); c.gpta_xmap = GPTA_XIDENTITY;
    uint64_t g[17];
    for (int r = 0; r < 16; r++) g[r] = UINT64_C(0x8765000000000000) + (uint64_t)(r+1)*UINT64_C(0x123456789);
    g[16] = 0;
    const uint64_t f0 = (UINT64_C(0xa5a5) << 32) | LF(LF_RAW,3);
    const uint64_t f1 = UINT64_C(0xfedcba9876543210), f2 = UINT64_C(0x123456789abcdef0);
    const uint64_t f3 = UINT64_C(0xdeadbeef09876543), temp = UINT64_C(0x13579bdf2468ace0);
    Uop u[2]; memset(u,0,sizeof u); u[1].p = finish;
    unsigned long long checked = 0;
    for (int pattern = 0; pattern < 4; pattern++) for (int op = 0; op < 12; op++) {
        unsigned char initial[16][16], memory[32] = {0}, loaded[16], expected[16];
        GptaV x[8];
        for (int r = 0; r < 16; r++) make_vector(initial[r],(op/3)&1,pattern,r,0);
        make_vector(memory+3,(op/3)&1,pattern,19,1);
        memset(loaded,0,16);
        memcpy(loaded,memory+3,op < 6 ? 16 : op < 9 ? 4 : 8);
        for (int r = 0; r < 8; r++) { memcpy(&x[r],initial[r],16); memcpy(&c.gpta_xhi[r],initial[r+8],16); }
        for (int d = 0; d < 8; d++) for (int s = 0; s < 8; s++) {
            reference(expected,op,loaded,d == s ? loaded : initial[s]);
            u[0].dst = (uint16_t)d; u[0].src = (uint16_t)s;
            for (int sc = 0; sc < 4; sc++) for (int b = 0; b < 17; b++) for (int i = 0; i < 17; i++) {
                uint64_t displacement = (uint64_t)(uintptr_t)(memory+3) - g[b] - (g[i] << sc);
                memcpy(&u[0].disp,&displacement,sizeof displacement);
                u[0].base = (uint8_t)b; u[0].index = (uint8_t)i; u[0].scale = (uint8_t)sc;
                for (int replica = 0; replica < 2; replica++) {
                    PFn fn = tables[replica][op][sc][d][s][b][i]; u[0].p = fn;
                    fn(g[15],u,g[0],g[1],g[2],g[6],g[7],g[4],g[3],g[5],
                       g[8],g[9],g[10],g[11],g[12],g[13],g[14],&c,temp,f0,f1,f2,f3,
                       x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7]);
                    int ok = !memcmp(c.r,g,16*sizeof(uint64_t)) && c.r[R_ZERO] == 0 &&
                        c.lf_op == (uint32_t)f0 && c.lf_cin == (uint32_t)(f0 >> 32) &&
                        c.lf_a == f1 && c.lf_b == f2 && c.lf_res == f3 && c.exc_info[0] == temp;
                    for (int r = 0; r < 16; r++)
                        if (memcmp(&c.xmm[r],r == d ? expected : initial[r],16)) ok = 0;
                    if (!ok) {
                        fprintf(stderr,"floating assembly matrix failed op=%d d=%d s=%d scale=%d base=%d index=%d replica=%d pattern=%d\n",
                            op,d,s,sc,b,i,replica,pattern);
                        return 1;
                    }
                    checked++;
                }
            }
        }
    }
    printf("assembly floating load/ALU matrix: %llu exact cases\n",checked);
    return 0;
}
