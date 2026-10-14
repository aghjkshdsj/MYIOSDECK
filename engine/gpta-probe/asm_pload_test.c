// SPDX-License-Identifier: GPL-3.0-or-later
// Native ARM correctness matrix for the statically compiled load/ALU handlers.
// This is a gate, not a speed benchmark or an input to engine specialization.
#include "../gpta/gpta_pin.h"
#include "../gpta/gpta_pin_asm_pload.h"
#if !GPTA_USE_ASM_PLOAD
#error ARM64 preserve_none is required for this gate
#endif

static GPTA_CC void finish(GPTA_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X(); c->exc_info[0] = T;
}
static const PFn (*const tables[2][12])[8][8][17][17] = {
    { gpta_t_apl_paddb,gpta_t_apl_paddw,gpta_t_apl_paddd,gpta_t_apl_paddq,gpta_t_apl_psubb,gpta_t_apl_psubw,gpta_t_apl_psubd,gpta_t_apl_psubq,gpta_t_apl_pand,gpta_t_apl_pandn,gpta_t_apl_por,gpta_t_apl_pxor },
    { gpta_t_apl_paddb_r1,gpta_t_apl_paddw_r1,gpta_t_apl_paddd_r1,gpta_t_apl_paddq_r1,gpta_t_apl_psubb_r1,gpta_t_apl_psubw_r1,gpta_t_apl_psubd_r1,gpta_t_apl_psubq_r1,gpta_t_apl_pand_r1,gpta_t_apl_pandn_r1,gpta_t_apl_por_r1,gpta_t_apl_pxor_r1 }
};
// Independent byte-wise reference: wrap carries within each SSE lane.
static void reference(unsigned char *out, int op, const unsigned char *a, const unsigned char *b) {
    if (op < 8) {
        unsigned bytes = 1u << (op & 3);
        for (unsigned lane = 0; lane < 16; lane += bytes) {
            uint64_t x = 0, y = 0;
            for (unsigned j = 0; j < bytes; j++) {
                x |= (uint64_t)a[lane+j] << (8*j);
                y |= (uint64_t)b[lane+j] << (8*j);
            }
            uint64_t r = op < 4 ? x+y : x-y;
            for (unsigned j = 0; j < bytes; j++) out[lane+j] = (unsigned char)(r >> (8*j));
        }
    } else for (unsigned j = 0; j < 16; j++)
        out[j] = op == 8 ? a[j] & b[j] : op == 9 ? (unsigned char)~a[j] & b[j] :
            op == 10 ? a[j] | b[j] : a[j] ^ b[j];
}
int main(void) {
    FxiCpu c;
    memset(&c,0,sizeof c);
    c.gpta_xmap = GPTA_XIDENTITY;
    uint64_t g[17];
    for (int r = 0; r < 16; r++) g[r] = UINT64_C(0x8765000000000000) + (uint64_t)(r+1)*UINT64_C(0x123456789);
    g[16] = 0;
    const uint64_t f0 = (UINT64_C(0xa5a5) << 32) | LF(LF_RAW,3);
    const uint64_t f1 = UINT64_C(0xfedcba9876543210), f2 = UINT64_C(0x123456789abcdef0);
    const uint64_t f3 = UINT64_C(0xdeadbeef09876543), temp = UINT64_C(0x13579bdf2468ace0);
    Uop u[2]; memset(u,0,sizeof u); u[1].p = finish;
    unsigned long long checked = 0;
    for (int pattern = 0; pattern < 2; pattern++) {
        unsigned char initial[16][16], memory[32], expected[16];
        GptaV x[8];
        for (int r = 0; r < 16; r++) for (int j = 0; j < 16; j++)
            initial[r][j] = (unsigned char)(r*29 + j*17 + pattern*83);
        for (int j = 0; j < 32; j++) memory[j] = pattern ? (unsigned char)(j*43+19) : 255;
        for (int r = 0; r < 8; r++) { memcpy(&x[r],initial[r],16); memcpy(&c.gpta_xhi[r],initial[r+8],16); }
        for (int op = 0; op < 12; op++) for (int d = 0; d < 8; d++) for (int s = 0; s < 8; s++) {
            reference(expected,op,memory+3,d == s ? memory+3 : initial[s]);
            u[0].dst = (uint16_t)d; u[0].src = (uint16_t)s;
            for (int sc = 0; sc < 4; sc++) for (int b = 0; b < 17; b++) for (int i = 0; i < 17; i++) {
                uint64_t displacement = (uint64_t)(uintptr_t)(memory+3) - g[b] - (g[i] << sc);
                memcpy(&u[0].disp,&displacement,sizeof displacement);
                u[0].base = (uint8_t)b; u[0].index = (uint8_t)i; u[0].scale = (uint8_t)sc;
                for (int replica = 0; replica < 2; replica++) {
                    PFn fn = tables[replica][op][sc][d][s][b][i];
                    u[0].p = fn;
                    fn(g[15],u,g[0],g[1],g[2],g[6],g[7],g[4],g[3],g[5],
                       g[8],g[9],g[10],g[11],g[12],g[13],g[14],&c,temp,f0,f1,f2,f3,
                       x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7]);
                    int ok = !memcmp(c.r,g,16*sizeof(uint64_t)) && c.r[R_ZERO] == 0 &&
                        c.lf_op == (uint32_t)f0 && c.lf_cin == (uint32_t)(f0 >> 32) &&
                        c.lf_a == f1 && c.lf_b == f2 && c.lf_res == f3 && c.exc_info[0] == temp;
                    for (int r = 0; r < 16; r++)
                        if (memcmp(&c.xmm[r],r == d ? expected : initial[r],16)) ok = 0;
                    if (!ok) {
                        fprintf(stderr,"assembly matrix failed op=%d d=%d s=%d scale=%d base=%d index=%d replica=%d pattern=%d\n",
                            op,d,s,sc,b,i,replica,pattern);
                        return 1;
                    }
                    checked++;
                }
            }
        }
    }
    printf("assembly load/ALU matrix: %llu exact cases\n",checked);
    return 0;
}
