// SPDX-License-Identifier: GPL-3.0-or-later
// Exhaustive static-pair correctness gate, never a speed benchmark.
#include "../gpta/gpta_pin.h"
#include "../gpta/gpta_pin_asm_salu.h"
#if !GPTA_USE_ASM_PLOAD
#error ARM64 preserve_none is required for this gate
#endif
static GPTA_CC void finish(GPTA_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X(); c->exc_info[0] = T;
}
#define TEST_ROW(SH,SUF) { \
    gpta_t_asal_##SH##_add##SUF,gpta_t_asal_##SH##_sub##SUF,gpta_t_asal_##SH##_and##SUF, \
    gpta_t_asal_##SH##_or##SUF,gpta_t_asal_##SH##_xor##SUF }
#define TEST_SET(SUF) { TEST_ROW(shl,SUF),TEST_ROW(shr,SUF),TEST_ROW(sar,SUF),TEST_ROW(rol,SUF),TEST_ROW(ror,SUF) }
static const PFn (*const tables[2][5][5])[64][16][16] = { TEST_SET(),TEST_SET(_r1) };
#undef TEST_SET
#undef TEST_ROW

// Independent unsigned reference, including arithmetic sign extension.
static uint64_t reference(int sh, int op, unsigned width, uint64_t x, uint64_t y, unsigned n, int alias) {
    uint64_t mask = width == 32 ? UINT64_C(0xffffffff) : UINT64_MAX;
    x &= mask;
    uint64_t shifted;
    if (sh == 0) shifted = x << n;
    else if (sh == 1) shifted = x >> n;
    else if (sh == 2) shifted = (x >> n) | ((x & (UINT64_C(1) << (width-1))) ? mask ^ (mask >> n) : 0);
    else if (sh == 3) shifted = (x << n) | (x >> (width-n));
    else shifted = (x >> n) | (x << (width-n));
    shifted &= mask;
    y = alias ? shifted : y & mask;
    return (op == 0 ? shifted+y : op == 1 ? shifted-y : op == 2 ? shifted&y :
            op == 3 ? shifted|y : shifted^y) & mask;
}
int main(void) {
    FxiCpu c; memset(&c,0,sizeof c); c.gpta_xmap = GPTA_XIDENTITY;
    const uint64_t f0 = (UINT64_C(0xa5a5) << 32) | LF(LF_RAW,3);
    const uint64_t f1 = UINT64_C(0xfedcba9876543210), f2 = UINT64_C(0x123456789abcdef0);
    const uint64_t f3 = UINT64_C(0xdeadbeef09876543), temp = UINT64_C(0x13579bdf2468ace0);
    Uop u[2]; memset(u,0,sizeof u); u[1].p = finish;
    GptaV x[8]; unsigned char initial[16][16];
    for (int r = 0; r < 16; r++) for (int j = 0; j < 16; j++) initial[r][j] = (unsigned char)(r*29+j*17);
    for (int r = 0; r < 8; r++) { memcpy(&x[r],initial[r],16); memcpy(&c.gpta_xhi[r],initial[r+8],16); }
    unsigned long long checked = 0;
    for (int pattern = 0; pattern < 2; pattern++) {
        uint64_t g[17];
        for (int r = 0; r < 16; r++) g[r] = (pattern ? UINT64_C(0x87654321fedcba98) : 0) +
            (uint64_t)(r+1)*UINT64_C(0x1020304050607081);
        g[16] = 0;
        for (int sh = 0; sh < 5; sh++) for (int op = 0; op < 5; op++) for (int size = 0; size < 2; size++)
            for (unsigned n = 0; n < 64; n++) for (int d = 0; d < 16; d++) for (int e = 0; e < 16; e++) {
                unsigned width = size ? 64 : 32;
                uint64_t expected[17]; memcpy(expected,g,sizeof g);
                if (n && n < width) expected[d] = reference(sh,op,width,g[d],g[e],n,d == e);
                for (int replica = 0; replica < 2; replica++) {
                    PFn fn = tables[replica][sh][op][size][n][d][e];
                    if (!n || n >= width) {
                        if (fn) { fprintf(stderr,"invalid static count has a handler\n"); return 1; }
                        continue;
                    }
                    u[0].p = fn;
                    fn(g[15],u,g[0],g[1],g[2],g[6],g[7],g[4],g[3],g[5],
                       g[8],g[9],g[10],g[11],g[12],g[13],g[14],&c,temp,f0,f1,f2,f3,
                       x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7]);
                    // Output flags are dead, so lazy placeholders stay intact.
                    int ok = !memcmp(c.r,expected,sizeof expected) &&
                        c.lf_op == (uint32_t)f0 && c.lf_cin == (uint32_t)(f0 >> 32) &&
                        c.lf_a == f1 && c.lf_b == f2 && c.lf_res == f3 && c.exc_info[0] == temp &&
                        !memcmp(c.xmm,initial,sizeof initial);
                    if (!ok) {
                        fprintf(stderr,"shift/ALU matrix failed shift=%d op=%d width=%u d=%d e=%d count=%u replica=%d pattern=%d\n",
                            sh,op,width,d,e,n,replica,pattern);
                        return 1;
                    }
                    checked++;
                }
            }
    }
    printf("assembly shift/ALU matrix: %llu exact register cases (dead flags)\n",checked);
    return 0;
}
