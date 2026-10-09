// SPDX-License-Identifier: GPL-3.0-or-later
// FXI decoder: x86-64 bytes -> a block of uops, decoded once and cached.
// After decoding, a backward pass picks the no-flags variant of every op whose
// flags are overwritten before anything reads them, and cmp/test + jcc pairs
// have already been fused into one compare-and-branch uop.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "fxi_internal.h"

#define MAX_INSNS 128          // instructions per block before a goto is appended
#define MAX_UOPS (MAX_INSNS + 4)

typedef struct {
    OpFn alt;                  // no-flags variant, if any
    uint8_t kill;              // writes every arithmetic flag
    uint8_t reads;             // reads flags (or needs the old CF)
    uint8_t cc_live;           // set u->cc = flags-live-after (rotates, inc/dec/neg)
} Meta;

typedef struct {
    const uint8_t *p, *start;
    uint64_t rip;              // start of the current instruction
    int rex, rexw, rexr, rexx, rexb, opsize16, rep, repne, seg, addr32, seg_bad, lock;
    int addr32_used;           // a 67 prefix on a memory operand (unsupported); on registers it is a no-op
    // ModRM
    int mod, reg, rm, is_mem, riprel;
    uint8_t base, index, scale;
    int64_t disp;
    // Block being built
    Uop u[MAX_UOPS];
    Meta m[MAX_UOPS];
    int n;
    // Last instruction was a fusable cmp/test (index of its uop, else -1)
    int fuse_at, fuse_op, fuse_form, fuse_si;
} Dec;

static Uop *emit(Dec *d, OpFn fn) {
    Uop *u = &d->u[d->n];
    memset(u, 0, sizeof *u);
    memset(&d->m[d->n], 0, sizeof d->m[0]);
    u->fn = fn;
    u->base = R_ZERO; u->index = R_ZERO;
    u->rip = d->rip;
    d->n++;
    return u;
}
static Meta *meta(Dec *d) { return &d->m[d->n - 1]; }

static OpFn named(const char *name) {
    OpFn f = fxi_named(name);
    if (!f) { fprintf(stderr, "fxi: missing handler %s\n", name); abort(); }
    return f;
}

static inline uint8_t  rd_u8(Dec *d) { return *d->p++; }
static inline int64_t  rd_s8(Dec *d) { return (int8_t)*d->p++; }
static inline int64_t  rd_s16(Dec *d) { int16_t v; memcpy(&v, d->p, 2); d->p += 2; return v; }
static inline int64_t  rd_s32(Dec *d) { int32_t v; memcpy(&v, d->p, 4); d->p += 4; return v; }
static inline uint64_t rd_u64(Dec *d) { uint64_t v; memcpy(&v, d->p, 8); d->p += 8; return v; }

static void modrm(Dec *d) {
    uint8_t b = rd_u8(d);
    d->mod = b >> 6;
    d->reg = ((b >> 3) & 7) | (d->rexr << 3);
    d->rm = (b & 7) | (d->rexb << 3);
    d->is_mem = d->mod != 3;
    if (d->is_mem && d->addr32) d->addr32_used = 1;
    d->riprel = 0;
    d->base = R_ZERO; d->index = R_ZERO; d->scale = 0; d->disp = 0;
    if (!d->is_mem) return;
    int rm = b & 7;
    if (rm == 4) {
        uint8_t sib = rd_u8(d);
        int idx = ((sib >> 3) & 7) | (d->rexx << 3), bs = sib & 7;
        d->scale = sib >> 6;
        d->index = (idx == 4) ? R_ZERO : (uint8_t)idx;
        if (bs == 5 && d->mod == 0) { d->base = R_ZERO; d->disp = rd_s32(d); }
        else d->base = (uint8_t)(bs | (d->rexb << 3));
    } else if (rm == 5 && d->mod == 0) {
        d->riprel = 1; d->disp = rd_s32(d);
    } else {
        d->base = (uint8_t)(rm | (d->rexb << 3));
    }
    if (d->mod == 1) d->disp += rd_s8(d);
    else if (d->mod == 2) d->disp += rd_s32(d);
    // FS/GS override: the segment base (register slot 17/18) takes the EA's free base or
    // index slot, so `mov rax, gs:[0x30]` (the Windows TEB) costs nothing extra.
    if (d->seg) {
        uint8_t s = d->seg == 0x64 ? R_FS : R_GS;
        if (d->base == R_ZERO) d->base = s;
        else if (d->index == R_ZERO) { d->index = s; d->scale = 0; }
        else d->seg_bad = 1;
    }
}

// Copy the decoded memory operand into a uop. Call once all immediates are read
// (RIP-relative addresses count from the end of the instruction).
static void set_mem(Dec *d, Uop *u) {
    u->base = d->base; u->index = d->index; u->scale = d->scale;
    u->disp = d->disp + (d->riprel ? (int64_t)(d->rip + (uint64_t)(d->p - d->start)) : 0);
}
static uint64_t next_rip(Dec *d) { return d->rip + (uint64_t)(d->p - d->start); }

// Byte offset of a general register in the register file for an operand size.
static uint16_t gpr(Dec *d, int r, int bits) {
    if (bits == 8 && !d->rex && r >= 4 && r < 8) return (uint16_t)((r - 4) * 8 + 1);   // AH CH DH BH
    return (uint16_t)(r * 8);
}
static int si_of(int bits) { return bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3; }
static int vsize(Dec *d) { return d->rexw ? 64 : d->opsize16 ? 16 : 32; }
static int64_t imm_z(Dec *d, int bits) { return bits == 16 ? rd_s16(d) : rd_s32(d); }   // Iz, sign-extended

static int unimplemented(Dec *d, const char *what) {
    char *msg = malloc(160);
    int len = (int)(d->p - d->start); if (len < 1) len = 1;
    int o = snprintf(msg, 160, "%s [", what);
    const uint8_t *q = d->start;
    for (int i = 0; i < 12 && o < 150; i++) o += snprintf(msg + o, 160 - (size_t)o, i ? " %02x" : "%02x", q[i]);
    snprintf(msg + o, 160 - (size_t)o, "]");
    Uop *u = emit(d, named("fail_ud"));
    u->imm = (uint64_t)(uintptr_t)msg;
    u->aux = d->rip;
    return 1;   // terminates the block
}

// ---- emission helpers ----
static void emit_alu(Dec *d, int op, int form, int bits, uint16_t dst, uint16_t src, uint64_t imm) {
    int si = si_of(bits);
    if (d->lock && (form == F_MR || form == F_MI) && op != ALU_CMP && op != ALU_TEST) {
        Uop *u = emit(d, named("lock_alu"));   // atomic read-modify-write (fxi_atomic.c)
        u->cc = (uint8_t)si; u->aux = (uint64_t)op; u->imm = imm;
        u->src = form == F_MI ? 0xffff : src;
        set_mem(d, u);
        meta(d)->kill = 1;
        meta(d)->reads = (op == ALU_ADC || op == ALU_SBB);
        return;
    }
    Uop *u = emit(d, fxi_alu_tab[op][form][si][1]);
    u->dst = dst; u->src = src; u->imm = imm;
    if (form == F_RM || form == F_MR || form == F_MI) set_mem(d, u);
    Meta *m = meta(d);
    m->alt = fxi_alu_tab[op][form][si][0];
    m->kill = 1;
    m->reads = (op == ALU_ADC || op == ALU_SBB);
    if ((op == ALU_CMP || op == ALU_TEST) && (form == F_RR || form == F_RI) && bits >= 32) {
        d->fuse_at = d->n - 1; d->fuse_op = op; d->fuse_form = form; d->fuse_si = bits == 64;
    }
}

static void emit_mov(Dec *d, int form, int bits, uint16_t dst, uint16_t src, uint64_t imm) {
    Uop *u = emit(d, fxi_mov_tab[form][si_of(bits)]);
    u->dst = dst; u->src = src; u->imm = imm;
    if (form == F_RM || form == F_MR || form == F_MI) set_mem(d, u);
}

// Conditional branch; fuses with an immediately preceding cmp/test.
static void emit_jcc(Dec *d, unsigned cc, uint64_t target) {
    uint64_t fall = next_rip(d);
    if (d->fuse_at == d->n - 1 && d->fuse_at >= 0) {
        Uop *p = &d->u[d->fuse_at];
        uint16_t dst = p->dst, src = p->src; uint64_t imm = p->imm, rip = p->rip;
        OpFn f = fxi_fjcc_tab[d->fuse_op == ALU_TEST][d->fuse_form == F_RI][d->fuse_si][cc];
        memset(p, 0, sizeof *p);
        memset(&d->m[d->fuse_at], 0, sizeof d->m[0]);
        p->fn = f; p->dst = dst; p->src = src; p->disp = (int64_t)imm; p->rip = rip;
        p->base = R_ZERO; p->index = R_ZERO;
        p->imm = target; p->aux = fall;
        return;
    }
    Uop *u = emit(d, named("jcc"));
    u->cc = (uint8_t)cc; u->imm = target; u->aux = fall;
    meta(d)->reads = 1;
}

// ---- SSE helpers ----
static int sse_prefix(Dec *d) { return d->rep ? 3 : d->repne ? 2 : d->opsize16 ? 1 : 0; }   // none 66 F2 F3 -> 0 1 2 3
// xmm op: dst = reg, src = rm (register or memory)
static void emit_x(Dec *d, const char *base, int has_imm) {
    char name[32];
    snprintf(name, sizeof name, "%s_%s", base, d->is_mem ? "RM" : "RR");
    uint64_t imm = has_imm ? rd_u8(d) : 0;
    Uop *u = emit(d, named(name));
    u->dst = (uint16_t)d->reg; u->src = (uint16_t)d->rm; u->imm = imm;
    if (d->is_mem) set_mem(d, u);
}
// store form: memory (or rm register) <- xmm reg
static void emit_xstore(Dec *d, const char *mem_name, const char *reg_name) {
    Uop *u = emit(d, named(d->is_mem ? mem_name : reg_name));
    if (d->is_mem) { u->src = (uint16_t)d->reg; set_mem(d, u); }
    else { u->dst = (uint16_t)d->rm; u->src = (uint16_t)d->reg; }
}

static int decode_sse(Dec *d, uint8_t op) {
    int pf = sse_prefix(d);
    static const char *const arith[16][4] = {
        // 0x51..0x5F by (op - 0x50), prefixes none 66 F2 F3
        [1] = { "sqrtps", "sqrtpd", "sqrtsd", "sqrtss" },
        [2] = { "rsqrtps", 0, 0, "rsqrtss" }, [3] = { "rcpps", 0, 0, "rcpss" },
        [4] = { "pand", "pand", 0, 0 }, [5] = { "pandn", "pandn", 0, 0 },
        [6] = { "por", "por", 0, 0 }, [7] = { "pxor", "pxor", 0, 0 },
        [8] = { "addps", "addpd", "addsd", "addss" }, [9] = { "mulps", "mulpd", "mulsd", "mulss" },
        [0xa] = { "cvtps2pd", "cvtpd2ps", "cvtsd2ss", "cvtss2sd" },
        [0xb] = { "cvtdq2ps", "cvtps2dq", 0, "cvttps2dq" },
        [0xc] = { "subps", "subpd", "subsd", "subss" }, [0xd] = { "minps", "minpd", "minsd", "minss" },
        [0xe] = { "divps", "divpd", "divsd", "divss" }, [0xf] = { "maxps", "maxpd", "maxsd", "maxss" },
    };
    modrm(d);
    if (op >= 0x51 && op <= 0x5f) {
        const char *n = arith[op - 0x50][pf];
        if (!n) return unimplemented(d, "sse op/prefix");
        emit_x(d, n, 0); return 0;
    }
    if (pf == 1 && ((op >= 0x60 && op <= 0x6d) || (op >= 0x74 && op <= 0x76) || op >= 0xd1)) {
        static const char *const p66[256] = {
            [0x60] = "punpcklbw", [0x61] = "punpcklwd", [0x62] = "punpckldq", [0x63] = "packsswb",
            [0x64] = "pcmpgtb", [0x65] = "pcmpgtw", [0x66] = "pcmpgtd", [0x67] = "packuswb",
            [0x68] = "punpckhbw", [0x69] = "punpckhwd", [0x6a] = "punpckhdq", [0x6b] = "packssdw",
            [0x6c] = "punpcklqdq", [0x6d] = "punpckhqdq", [0x74] = "pcmpeqb", [0x75] = "pcmpeqw", [0x76] = "pcmpeqd",
            [0xd1] = "psrlwx", [0xd2] = "psrldx", [0xd3] = "psrlqx", [0xe1] = "psrawx", [0xe2] = "psradx",
            [0xf1] = "psllwx", [0xf2] = "pslldx", [0xf3] = "psllqx",
            [0xd8] = "psubusb", [0xd9] = "psubusw", [0xdc] = "paddusb", [0xdd] = "paddusw",
            [0xe8] = "psubsb", [0xe9] = "psubsw", [0xec] = "paddsb", [0xed] = "paddsw",
            [0xe0] = "pavgb", [0xe3] = "pavgw", [0xe4] = "pmulhuw", [0xe5] = "pmulhw", [0xf5] = "pmaddwd",
            [0xd4] = "paddq", [0xd5] = "pmullw", [0xda] = "pminub", [0xdb] = "pand", [0xde] = "pmaxub", [0xdf] = "pandn",
            [0xea] = "pminsw", [0xeb] = "por", [0xee] = "pmaxsw", [0xef] = "pxor", [0xf4] = "pmuludq", [0xf6] = "psadbw",
            [0xf8] = "psubb", [0xf9] = "psubw", [0xfa] = "psubd", [0xfb] = "psubq", [0xfc] = "paddb", [0xfd] = "paddw", [0xfe] = "paddd",
        };
        if (p66[op]) { emit_x(d, p66[op], 0); return 0; }
    }
    switch (op) {
    case 0x10:
        if (pf <= 1) emit_x(d, "movx", 0);
        else { const char *n = pf == 3 ? "movss" : "movsd"; char nm[16]; snprintf(nm, 16, "%s_%s", n, d->is_mem ? "RM" : "RR");
               Uop *u = emit(d, named(nm)); u->dst = (uint16_t)d->reg; u->src = (uint16_t)d->rm; if (d->is_mem) set_mem(d, u); }
        return 0;
    case 0x11:
        if (pf <= 1) emit_xstore(d, "movx_MR", "movx_RR");
        else if (pf == 3) emit_xstore(d, "movss_MR", "movss_RR");
        else emit_xstore(d, "movsd_MR", "movsd_RR");
        return 0;
    case 0x12:
        if (pf <= 1) { if (d->is_mem) emit_x(d, "movlps", 0); else if (pf == 0) emit_x(d, "movhlps", 0); else return unimplemented(d, "movlpd reg"); return 0; }
        return unimplemented(d, "movddup/movsldup");
    case 0x13: if (pf <= 1 && d->is_mem) { emit_xstore(d, "movsd_MR", "movsd_RR"); return 0; } return unimplemented(d, "0f13");
    case 0x14: emit_x(d, pf == 1 ? "punpcklqdq" : "punpckldq", 0); return 0;
    case 0x15: emit_x(d, pf == 1 ? "punpckhqdq" : "punpckhdq", 0); return 0;
    case 0x16:
        if (pf <= 1) { if (d->is_mem) emit_x(d, "movhps", 0); else if (pf == 0) emit_x(d, "movlhps", 0); else return unimplemented(d, "movhpd reg"); return 0; }
        return unimplemented(d, "movshdup");
    case 0x17: if (pf <= 1 && d->is_mem) { emit_xstore(d, "movhps_MR", "movhps_MR"); return 0; } return unimplemented(d, "0f17");
    case 0x28: if (pf <= 1) { emit_x(d, "movx", 0); return 0; } break;
    case 0x29: if (pf <= 1) { emit_xstore(d, "movx_MR", "movx_RR"); return 0; } break;
    case 0x2a: case 0x2c: case 0x2d: {
        if (pf < 2) break;
        const char *n = op == 0x2a ? (pf == 3 ? "cvtsi2ss" : "cvtsi2sd")
                      : op == 0x2c ? (pf == 3 ? "cvttss2si" : "cvttsd2si") : (pf == 3 ? "cvtss2si" : "cvtsd2si");
        char nm[24];
        snprintf(nm, sizeof nm, "%s_%s%d", n, d->is_mem ? "M" : "R", op == 0x2a ? (d->rexw ? 64 : 32) : (d->rexw ? 64 : 32));
        if (op != 0x2a && d->is_mem) snprintf(nm, sizeof nm, "%s_M%d", n, d->rexw ? 64 : 32);
        Uop *u = emit(d, named(nm));
        if (op == 0x2a) { u->dst = (uint16_t)d->reg; u->src = (uint16_t)(d->rm * 8); }   // xmm <- gpr
        else { u->dst = (uint16_t)(d->reg * 8); u->src = (uint16_t)d->rm; }             // gpr <- xmm
        if (d->is_mem) set_mem(d, u);
        return 0;
    }
    case 0x2e: case 0x2f: if (pf <= 1) { emit_x(d, pf == 1 ? "comisd" : "comiss", 0); meta(d)->reads = 1; return 0; } break;
    case 0x50: if (pf <= 1 && !d->is_mem) { Uop *u = emit(d, named(pf ? "movmskpd_RR" : "movmskps_RR")); u->dst = (uint16_t)(d->reg * 8); u->src = (uint16_t)d->rm; return 0; } break;
    case 0x6e: if (pf == 1) {
        if (d->is_mem) { Uop *u = emit(d, named(d->rexw ? "movsd_RM" : "movss_RM")); u->dst = (uint16_t)d->reg; set_mem(d, u); }
        else { Uop *u = emit(d, named(d->rexw ? "movd_XR64" : "movd_XR32")); u->dst = (uint16_t)d->reg; u->src = (uint16_t)(d->rm * 8); }
        return 0; } break;
    case 0x6f: if (pf == 1 || pf == 3) { emit_x(d, "movx", 0); return 0; } break;
    case 0x70: { const char *n = pf == 1 ? "pshufd" : pf == 2 ? "pshuflw" : pf == 3 ? "pshufhw" : 0;
                 if (n) { emit_x(d, n, 1); return 0; } break; }
    case 0x71: case 0x72: case 0x73: {
        if (pf != 1 || d->is_mem) break;
        static const char *const sh[3][8] = {
            { 0, 0, "psrlw", 0, "psraw", 0, "psllw", 0 },
            { 0, 0, "psrld", 0, "psrad", 0, "pslld", 0 },
            { 0, 0, "psrlq", "psrldq", 0, 0, "psllq", "pslldq" },
        };
        const char *n = sh[op - 0x71][d->reg & 7];
        if (!n) break;
        char nm[16]; snprintf(nm, sizeof nm, "%s_RI", n);
        Uop *u = emit(d, named(nm)); u->dst = (uint16_t)d->rm; u->imm = rd_u8(d);
        return 0;
    }
    case 0x7e:
        if (pf == 1) {
            if (d->is_mem) { Uop *u = emit(d, named(d->rexw ? "movsd_MR" : "movss_MR")); u->src = (uint16_t)d->reg; set_mem(d, u); }
            else { Uop *u = emit(d, named(d->rexw ? "movd_RX64" : "movd_RX32")); u->dst = (uint16_t)(d->rm * 8); u->src = (uint16_t)d->reg; }
            return 0;
        }
        if (pf == 3) {
            if (d->is_mem) { Uop *u = emit(d, named("movsd_RM")); u->dst = (uint16_t)d->reg; set_mem(d, u); }
            else { Uop *u = emit(d, named("movq_RR")); u->dst = (uint16_t)d->reg; u->src = (uint16_t)d->rm; }
            return 0;
        }
        break;
    case 0x7f: if (pf == 1 || pf == 3) { emit_xstore(d, "movx_MR", "movx_RR"); return 0; } break;
    case 0xc2: { const char *n = pf == 0 ? "cmpps" : pf == 1 ? "cmppd" : pf == 2 ? "cmpsd" : "cmpss"; emit_x(d, n, 1); return 0; }
    case 0xc6: if (pf <= 1) { emit_x(d, pf ? "shufpd" : "shufps", 1); return 0; } break;
    case 0xd6: if (pf == 1) {
        if (d->is_mem) { Uop *u = emit(d, named("movsd_MR")); u->src = (uint16_t)d->reg; set_mem(d, u); }
        else { Uop *u = emit(d, named("movq_RR")); u->dst = (uint16_t)d->rm; u->src = (uint16_t)d->reg; }
        return 0; } break;
    case 0xd7: if (pf == 1 && !d->is_mem) { Uop *u = emit(d, named("pmovmskb_RR")); u->dst = (uint16_t)(d->reg * 8); u->src = (uint16_t)d->rm; return 0; } break;
    case 0xe6: if (pf == 3) { emit_x(d, "cvtdq2pd", 0); return 0; } if (pf == 1) { emit_x(d, "cvttpd2dq", 0); return 0; }
               if (pf == 2) { emit_x(d, "cvtpd2dq", 0); return 0; } break;
    case 0x2b: if (pf <= 1 && d->is_mem) { emit_xstore(d, "movx_MR", "movx_MR"); return 0; } break;   // movntps/pd
    case 0xe7: if (pf == 1 && d->is_mem) { emit_xstore(d, "movx_MR", "movx_MR"); return 0; } break;   // movntdq
    case 0xc4: if (pf == 1) {                          // pinsrw xmm, r32/m16, imm8
        uint64_t imm = rd_u8(d);                       // before set_mem: rip-relative counts from the end
        Uop *u = emit(d, named(d->is_mem ? "pinsrw_M" : "pinsrw_R"));
        u->dst = (uint16_t)d->reg; u->src = (uint16_t)(d->rm * 8); u->imm = imm;
        if (d->is_mem) set_mem(d, u);
        return 0; } break;
    case 0xc5: if (pf == 1 && !d->is_mem) {            // pextrw r32, xmm, imm8
        Uop *u = emit(d, named("pextrw_R"));
        u->dst = (uint16_t)(d->reg * 8); u->src = (uint16_t)d->rm; u->imm = rd_u8(d);
        return 0; } break;
    }
    return unimplemented(d, "sse");
}

// lock inc/dec/not/neg on memory: atomic (fxi_atomic.c). Flags always recorded.
static void emit_lock_unary(Dec *d, int op, int bits) {
    Uop *u = emit(d, named("lock_unary"));
    u->cc = (uint8_t)si_of(bits); u->aux = (uint64_t)op;
    set_mem(d, u);
    if (op == U_INC || op == U_DEC) meta(d)->reads = 1;   // keep CF
    else if (op == U_NEG) meta(d)->kill = 1;
}

// bt/bts/btr/btc (bop 0-3) with the bit index in a register (src) or an immediate.
static void emit_bitop(Dec *d, int bop, int bits, int has_imm) {
    uint64_t imm = has_imm ? rd_u8(d) : 0;
    Uop *u = emit(d, named(d->is_mem ? "bitop_M" : "bitop_R"));
    u->cc = (uint8_t)si_of(bits); u->aux = (uint64_t)bop; u->imm = imm;
    u->src = has_imm ? 0xffff : gpr(d, d->reg, bits);
    if (d->is_mem) set_mem(d, u); else u->dst = gpr(d, d->rm, bits);
    meta(d)->reads = 1;   // only CF changes
}

// ---- one instruction; returns 1 when it ended the block ----
static int decode_one_inner(Dec *d) {
    d->start = d->p;
    d->rex = d->rexw = d->rexr = d->rexx = d->rexb = d->opsize16 = d->rep = d->repne = d->seg = d->addr32 = d->seg_bad = d->lock = 0;
    d->addr32_used = 0;
    uint8_t b;
    for (;;) {   // legacy prefixes
        b = *d->p;
        if (b == 0x66) d->opsize16 = 1;
        else if (b == 0xf3) { d->rep = 1; d->repne = 0; }
        else if (b == 0xf2) { d->repne = 1; d->rep = 0; }
        else if (b == 0xf0) d->lock = 1;
        else if (b == 0x2e || b == 0x3e || b == 0x26 || b == 0x36) { }
        else if (b == 0x64 || b == 0x65) d->seg = b;
        else if (b == 0x67) d->addr32 = 1;
        else break;
        d->p++;
    }
    // Only the REX prefix right before the opcode counts; an earlier one is ignored (MSVC's
    // unwinder-friendly tail jump is "48 41 ff e2", rex.w then rex.b jmp r10: Valve's client).
    while ((b & 0xf0) == 0x40) {
        d->rex = 1; d->rexw = (b >> 3) & 1; d->rexr = (b >> 2) & 1; d->rexx = (b >> 1) & 1; d->rexb = b & 1;
        d->p++; b = *d->p;
    }
    d->p++;
    // 67 only changes memory addressing (and the string/loop/xlat registers): padding elsewhere.
    if (d->addr32 && ((b >= 0xa0 && b <= 0xa7) || (b >= 0xaa && b <= 0xaf) || (b >= 0xe0 && b <= 0xe3) || b == 0xd7))
        return unimplemented(d, "32-bit addressing");
    int bits = vsize(d);
    int prev_fuse = d->fuse_at;
    d->fuse_at = -1;

    // ALU block 00-3F
    if (b < 0x40 && (b & 7) < 6) {
        static const int ops[8] = { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
        int op = ops[b >> 3], kind = b & 7, sz = (kind & 1) ? bits : 8;
        if (kind == 4 || kind == 5) {
            int64_t imm = kind == 4 ? rd_s8(d) : imm_z(d, bits);
            emit_alu(d, op, F_RI, sz, gpr(d, R_AX, sz), 0, (uint64_t)imm);
            return 0;
        }
        modrm(d);
        if (kind < 2) {   // Eb/Ev, Gb/Gv
            if (d->is_mem) emit_alu(d, op, F_MR, sz, 0, gpr(d, d->reg, sz), 0);
            else emit_alu(d, op, F_RR, sz, gpr(d, d->rm, sz), gpr(d, d->reg, sz), 0);
        } else {          // Gb/Gv, Eb/Ev
            if (d->is_mem) emit_alu(d, op, F_RM, sz, gpr(d, d->reg, sz), 0, 0);
            else emit_alu(d, op, F_RR, sz, gpr(d, d->reg, sz), gpr(d, d->rm, sz), 0);
        }
        return 0;
    }

    switch (b) {
    case 0x0f: break;   // two-byte, below
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        Uop *u = emit(d, named("push_R")); u->src = (uint16_t)(((b & 7) | (d->rexb << 3)) * 8); return 0; }
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        Uop *u = emit(d, named("pop_R")); u->dst = (uint16_t)(((b & 7) | (d->rexb << 3)) * 8); return 0; }
    case 0x63:
        modrm(d);
        if (d->rexw) { Uop *u = emit(d, fxi_ext_tab[1][d->is_mem][2][3]); u->dst = gpr(d, d->reg, 64); u->src = gpr(d, d->rm, 32); if (d->is_mem) set_mem(d, u); }
        else if (d->is_mem) emit_mov(d, F_RM, 32, gpr(d, d->reg, 32), 0, 0);
        else emit_mov(d, F_RR, 32, gpr(d, d->reg, 32), gpr(d, d->rm, 32), 0);
        return 0;
    case 0x68: { Uop *u = emit(d, named("push_I")); u->imm = (uint64_t)rd_s32(d); return 0; }
    case 0x6a: { Uop *u = emit(d, named("push_I")); u->imm = (uint64_t)rd_s8(d); return 0; }
    case 0x69: case 0x6b: {
        modrm(d);
        int64_t imm = b == 0x6b ? rd_s8(d) : imm_z(d, bits);
        Uop *u = emit(d, fxi_imul3_tab[d->is_mem][si_of(bits)]);
        u->dst = gpr(d, d->reg, bits); u->src = gpr(d, d->rm, bits); u->imm = (uint64_t)imm;
        if (d->is_mem) set_mem(d, u);
        meta(d)->kill = 1;
        return 0;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
        int64_t rel = rd_s8(d);
        d->fuse_at = prev_fuse;
        emit_jcc(d, b & 15, next_rip(d) + (uint64_t)rel);
        d->fuse_at = -1;
        return 1;
    }
    case 0x80: case 0x81: case 0x83: {
        static const int ops[8] = { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
        int sz = b == 0x80 ? 8 : bits;
        modrm(d);
        int64_t imm = (b == 0x81) ? imm_z(d, bits) : rd_s8(d);
        int op = ops[d->reg & 7];
        if (d->is_mem) emit_alu(d, op, F_MI, sz, 0, 0, (uint64_t)imm);
        else emit_alu(d, op, F_RI, sz, gpr(d, d->rm, sz), 0, (uint64_t)imm);
        return 0;
    }
    case 0x84: case 0x85: {
        int sz = b == 0x84 ? 8 : bits;
        modrm(d);
        if (d->is_mem) emit_alu(d, ALU_TEST, F_MR, sz, 0, gpr(d, d->reg, sz), 0);
        else emit_alu(d, ALU_TEST, F_RR, sz, gpr(d, d->rm, sz), gpr(d, d->reg, sz), 0);
        return 0;
    }
    case 0x86: case 0x87: {
        int sz = b == 0x86 ? 8 : bits;
        modrm(d);
        if (d->is_mem) {   // implicitly locked: atomic
            Uop *u = emit(d, named("xchg_M"));
            u->cc = (uint8_t)si_of(sz); u->src = gpr(d, d->reg, sz); set_mem(d, u);
            return 0;
        }
        char nm[16]; snprintf(nm, sizeof nm, "xchg_R_%d", sz);
        Uop *u = emit(d, named(nm));
        u->dst = gpr(d, d->rm, sz); u->src = gpr(d, d->reg, sz);
        return 0;
    }
    case 0x88: case 0x89: {
        int sz = b == 0x88 ? 8 : bits;
        modrm(d);
        if (d->is_mem) emit_mov(d, F_MR, sz, 0, gpr(d, d->reg, sz), 0);
        else emit_mov(d, F_RR, sz, gpr(d, d->rm, sz), gpr(d, d->reg, sz), 0);
        return 0;
    }
    case 0x8a: case 0x8b: {
        int sz = b == 0x8a ? 8 : bits;
        modrm(d);
        if (d->is_mem) emit_mov(d, F_RM, sz, gpr(d, d->reg, sz), 0, 0);
        else emit_mov(d, F_RR, sz, gpr(d, d->reg, sz), gpr(d, d->rm, sz), 0);
        return 0;
    }
    case 0x8d: {
        modrm(d);
        if (!d->is_mem) return unimplemented(d, "lea reg");
        // 67 lea: the address is computed in 32 bits. Its low 32 bits equal the 64-bit sum, so a
        // 32/16-bit destination is unchanged, and a 64-bit one gets the zero-extended 32-bit
        // address, which is exactly lea_32 (Dokimon: 67 8d 04 0a, lea eax, [edx+ecx]).
        if (d->addr32 && !d->riprel) { d->addr32_used = 0; if (bits == 64) bits = 32; }
        Uop *u = emit(d, fxi_lea_tab[si_of(bits)]); u->dst = gpr(d, d->reg, bits); set_mem(d, u);
        return 0;
    }
    case 0x8f: {
        modrm(d);
        if (d->is_mem) { Uop *u = emit(d, named("pop_M")); set_mem(d, u); }
        else { Uop *u = emit(d, named("pop_R")); u->dst = (uint16_t)(d->rm * 8); }
        return 0;
    }
    case 0x90:
        if (d->rexb) { Uop *u = emit(d, named(d->rexw ? "xchg_R_64" : "xchg_R_32")); u->dst = 0; u->src = 8 * 8; return 0; }
        emit(d, named("nop")); return 0;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        char nm[16]; snprintf(nm, sizeof nm, "xchg_R_%d", bits);
        Uop *u = emit(d, named(nm)); u->dst = 0; u->src = (uint16_t)(((b & 7) | (d->rexb << 3)) * 8); return 0; }
    case 0x98: emit(d, named(d->rexw ? "cdqe" : d->opsize16 ? "cbw" : "cwde")); return 0;
    case 0x99: emit(d, named(d->rexw ? "cqo" : d->opsize16 ? "cwd" : "cdq")); return 0;
    case 0x9c: emit(d, named("pushf")); meta(d)->reads = 1; return 0;
    case 0x9d: emit(d, named("popf")); meta(d)->kill = 1; return 0;
    case 0x9e: emit(d, named("sahf")); meta(d)->reads = 1; return 0;
    case 0x9f: emit(d, named("lahf")); meta(d)->reads = 1; return 0;
    case 0xa8: emit_alu(d, ALU_TEST, F_RI, 8, 0, 0, (uint64_t)rd_s8(d)); return 0;
    case 0xa9: emit_alu(d, ALU_TEST, F_RI, bits, 0, 0, (uint64_t)imm_z(d, bits)); return 0;
    case 0xa4: case 0xa5: case 0xaa: case 0xab: {
        Uop *u = emit(d, named((b == 0xa4 || b == 0xa5) ? "movs" : "stos"));
        u->scale = (b & 1) ? (uint8_t)si_of(bits) : 0;
        u->cc = (uint8_t)(d->rep || d->repne);
        return 0;
    }
    case 0xa6: case 0xa7: case 0xae: case 0xaf: {   // cmps / scas (repe F3, repne F2)
        Uop *u = emit(d, named((b == 0xa6 || b == 0xa7) ? "cmps" : "scas"));
        u->scale = (b & 1) ? (uint8_t)si_of(bits) : 0;
        u->cc = (uint8_t)(d->rep ? 1 : d->repne ? 2 : 0);
        if (u->cc) meta(d)->reads = 1;   // RCX = 0 keeps the old flags
        else meta(d)->kill = 1;
        return 0;
    }
    case 0xac: case 0xad: {                           // lods
        Uop *u = emit(d, named("lods"));
        u->scale = (b & 1) ? (uint8_t)si_of(bits) : 0;
        u->cc = (uint8_t)(d->rep || d->repne);
        return 0;
    }
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        emit_mov(d, F_RI, 8, gpr(d, (b & 7) | (d->rexb << 3), 8), 0, (uint64_t)rd_u8(d)); return 0;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: {
        uint64_t imm = d->rexw ? rd_u64(d) : (uint64_t)(bits == 16 ? (uint16_t)rd_s16(d) : (uint32_t)rd_s32(d));
        emit_mov(d, F_RI, bits, gpr(d, (b & 7) | (d->rexb << 3), bits), 0, imm);
        return 0;
    }
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int sz = (b & 1) ? bits : 8;
        modrm(d);
        int op = d->reg & 7;
        uint64_t cnt = 1; int by_cl = 0;
        if (b == 0xc0 || b == 0xc1) cnt = rd_u8(d);
        else if (b == 0xd2 || b == 0xd3) by_cl = 1;
        Uop *u = emit(d, fxi_shift_tab[op][d->is_mem][si_of(sz)]);
        u->dst = gpr(d, d->rm, sz); u->imm = cnt; u->src = by_cl ? 0xffff : 0;
        if (d->is_mem) set_mem(d, u);
        Meta *m = meta(d);
        unsigned eff = (unsigned)cnt & (sz == 64 ? 63u : 31u);
        if (op == SH_ROL || op == SH_ROR) { m->cc_live = 1; }
        else if (op == SH_RCL || op == SH_RCR) { m->reads = 1; }
        else if (by_cl || eff == 0) { m->reads = 1; }
        else { m->kill = 1; }
        return 0;
    }
    case 0xc2: case 0xc3: {
        Uop *u = emit(d, named("ret"));
        u->aux = b == 0xc2 ? (uint64_t)(uint16_t)rd_s16(d) : 0;
        return 1;
    }
    case 0xc6: case 0xc7: {
        int sz = b == 0xc6 ? 8 : bits;
        modrm(d);
        if ((d->reg & 7) != 0) return unimplemented(d, "c6/c7 non-mov");
        int64_t imm = sz == 8 ? rd_s8(d) : imm_z(d, sz);
        if (d->is_mem) emit_mov(d, F_MI, sz, 0, 0, (uint64_t)imm);
        else emit_mov(d, F_RI, sz, gpr(d, d->rm, sz), 0, (uint64_t)imm);
        return 0;
    }
    case 0xc9: emit(d, named("leave")); return 0;
    case 0x9b: emit(d, named("nop")); return 0;   // fwait: x87 exceptions are never raised
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: {   // x87
        modrm(d);
        Uop *u = emit(d, named("x87"));
        u->aux = (uint64_t)((b - 0xd8) << 8 | d->mod << 6 | (d->reg & 7) << 3 | (d->rm & 7));
        u->imm = d->rip;
        if (d->is_mem) set_mem(d, u);
        meta(d)->reads = 1;   // fcmov reads flags, fcomi writes some
        return 0;
    }
    // CPU exceptions (fxi_ops.c op_trap): u->imm = interrupt vector, 0x100 = hlt, 0x106 = ud2
    case 0xcc: emit(d, named("trap"))->imm = 3; return 1;
    case 0xcd: { uint64_t n = rd_u8(d); emit(d, named("trap"))->imm = n; return 1; }
    case 0xf4: emit(d, named("trap"))->imm = 0x100; return 1;
    case 0xe8: {
        int64_t rel = rd_s32(d);
        Uop *u = emit(d, named("call")); u->aux = next_rip(d); u->imm = next_rip(d) + (uint64_t)rel;
        return 1;
    }
    case 0xe9: case 0xeb: {
        int64_t rel = b == 0xe9 ? rd_s32(d) : rd_s8(d);
        Uop *u = emit(d, named("jmp")); u->imm = next_rip(d) + (uint64_t)rel;
        return 1;
    }
    case 0xf5: emit(d, named("cmc")); meta(d)->reads = 1; return 0;
    case 0xf8: emit(d, named("clc")); meta(d)->reads = 1; return 0;
    case 0xf9: emit(d, named("stc")); meta(d)->reads = 1; return 0;
    case 0xfc: emit(d, named("cld")); return 0;
    case 0xfd: emit(d, named("std")); return 0;
    case 0xf6: case 0xf7: {
        int sz = b == 0xf6 ? 8 : bits;
        modrm(d);
        int op = d->reg & 7;
        if (op <= 1) {
            int64_t imm = sz == 8 ? rd_s8(d) : imm_z(d, sz);
            if (d->is_mem) emit_alu(d, ALU_TEST, F_MI, sz, 0, 0, (uint64_t)imm);
            else emit_alu(d, ALU_TEST, F_RI, sz, gpr(d, d->rm, sz), 0, (uint64_t)imm);
            return 0;
        }
        if (op == 2 || op == 3) {
            if (d->lock && d->is_mem) { emit_lock_unary(d, op == 2 ? U_NOT : U_NEG, sz); return 0; }
            Uop *u = emit(d, fxi_unary_tab[op == 2 ? U_NOT : U_NEG][d->is_mem][si_of(sz)]);
            u->dst = gpr(d, d->rm, sz); if (d->is_mem) set_mem(d, u);
            if (op == 3) meta(d)->cc_live = 1;
            return 0;
        }
        Uop *u = emit(d, fxi_muldiv_tab[op - 4][d->is_mem][si_of(sz)]);
        u->src = gpr(d, d->rm, sz); u->aux = d->rip;
        if (d->is_mem) set_mem(d, u);
        if (op <= 5) meta(d)->kill = 1;
        return 0;
    }
    case 0xfe: case 0xff: {
        int sz = b == 0xfe ? 8 : bits;
        modrm(d);
        int op = d->reg & 7;
        if (op <= 1) {
            if (d->lock && d->is_mem) { emit_lock_unary(d, op == 0 ? U_INC : U_DEC, sz); return 0; }
            Uop *u = emit(d, fxi_unary_tab[op == 0 ? U_INC : U_DEC][d->is_mem][si_of(sz)]);
            u->dst = gpr(d, d->rm, sz); if (d->is_mem) set_mem(d, u);
            meta(d)->cc_live = 1;
            return 0;
        }
        if (b == 0xff && (op == 2 || op == 4)) {
            Uop *u = emit(d, named(op == 2 ? (d->is_mem ? "call_M" : "call_R") : (d->is_mem ? "jmp_M" : "jmp_R")));
            u->src = (uint16_t)(d->rm * 8);
            if (d->is_mem) set_mem(d, u);
            u->aux = next_rip(d);
            return 1;
        }
        if (b == 0xff && op == 6) {
            Uop *u = emit(d, named(d->is_mem ? "push_M" : "push_R"));
            u->src = (uint16_t)(d->rm * 8);
            if (d->is_mem) set_mem(d, u);
            return 0;
        }
        return unimplemented(d, "fe/ff group");
    }
    default:
        return unimplemented(d, "opcode");
    }

    // ---- two-byte opcodes ----
    uint8_t op = *d->p++;
    if (op >= 0x80 && op <= 0x8f) {
        int64_t rel = rd_s32(d);
        d->fuse_at = prev_fuse;
        emit_jcc(d, op & 15, next_rip(d) + (uint64_t)rel);
        d->fuse_at = -1;
        return 1;
    }
    if (op >= 0x40 && op <= 0x4f) {
        modrm(d);
        Uop *u = emit(d, fxi_cmov_tab[d->is_mem][si_of(bits)]);
        u->dst = gpr(d, d->reg, bits); u->src = gpr(d, d->rm, bits); u->cc = op & 15;
        if (d->is_mem) set_mem(d, u);
        meta(d)->reads = 1;
        return 0;
    }
    if (op >= 0x90 && op <= 0x9f) {
        modrm(d);
        Uop *u = emit(d, fxi_setcc_tab[d->is_mem]);
        u->dst = gpr(d, d->rm, 8); u->cc = op & 15;
        if (d->is_mem) set_mem(d, u);
        meta(d)->reads = 1;
        return 0;
    }
    if (op >= 0xc8 && op <= 0xcf) {
        Uop *u = emit(d, named(d->rexw ? "bswap64" : "bswap32"));
        u->dst = (uint16_t)(((op & 7) | (d->rexb << 3)) * 8);
        return 0;
    }
    switch (op) {
    case 0x05: { Uop *u = emit(d, named("syscall")); u->aux = next_rip(d); meta(d)->reads = 1; return 1; }
    case 0x0b: emit(d, named("trap"))->imm = 0x106; return 1;   // ud2
    case 0x0d: case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        modrm(d); emit(d, named("nop")); return 0;   // prefetch, hint nops, endbr64
    case 0x3a: {   // SSE4.1 subset: pinsrd/q, pextrd/q (66 prefix)
        uint8_t op3 = *d->p++;
        if (!d->opsize16 || (op3 != 0x22 && op3 != 0x16)) return unimplemented(d, "0f 3a");
        modrm(d);
        uint64_t lane = rd_u8(d);
        char nm[16];
        snprintf(nm, sizeof nm, "%s%c_%s", op3 == 0x22 ? "pinsr" : "pextr", d->rexw ? 'q' : 'd', d->is_mem ? "M" : "R");
        Uop *u = emit(d, named(nm));
        u->imm = lane;
        if (op3 == 0x22) { u->dst = (uint16_t)d->reg; u->src = (uint16_t)(d->rm * 8); }   // xmm <- r/m
        else { u->dst = (uint16_t)(d->rm * 8); u->src = (uint16_t)d->reg; }              // r/m <- xmm
        if (d->is_mem) set_mem(d, u);
        return 0;
    }
    case 0x31: emit(d, named("rdtsc")); return 0;
    case 0xa2: emit(d, named("cpuid")); return 0;
    case 0xa3: case 0xab: case 0xb3: case 0xbb:   // bt bts btr btc r/m, r
        modrm(d);
        emit_bitop(d, (op >> 3) & 3, bits, 0);
        return 0;
    case 0xba:                                     // bt bts btr btc r/m, imm8 (/4-/7)
        modrm(d);
        if ((d->reg & 7) < 4) return unimplemented(d, "0f ba /0-3");
        emit_bitop(d, d->reg & 3, bits, 1);
        return 0;
    case 0xa4: case 0xa5: case 0xac: case 0xad: {  // shld/shrd r/m, r, imm8|cl
        modrm(d);
        uint64_t cnt = (op & 1) ? 0 : rd_u8(d);
        Uop *u = emit(d, named(d->is_mem ? "shxd_M" : "shxd_R"));
        u->cc = (uint8_t)si_of(bits); u->aux = (uint64_t)((op >= 0xac) | ((op & 1) << 1)); u->imm = cnt;
        u->src = gpr(d, d->reg, bits);
        if (d->is_mem) set_mem(d, u); else u->dst = gpr(d, d->rm, bits);
        meta(d)->reads = 1;   // a zero count leaves the flags alone
        return 0;
    }
    case 0xb0: case 0xb1: case 0xc0: case 0xc1: {  // cmpxchg, xadd r/m, r
        int sz = (op & 1) ? bits : 8, is_xadd = op >= 0xc0;
        modrm(d);
        char nm[16]; snprintf(nm, sizeof nm, "%s_%s", is_xadd ? "xadd" : "cmpxchg", d->is_mem ? "M" : "R");
        Uop *u = emit(d, named(nm));
        u->cc = (uint8_t)si_of(sz); u->src = gpr(d, d->reg, sz);
        if (d->is_mem) set_mem(d, u); else u->dst = gpr(d, d->rm, sz);
        meta(d)->kill = 1;
        return 0;
    }
    case 0xae:                                     // fxsave fxrstor ldmxcsr stmxcsr clflush / fences
        modrm(d);
        if (!d->is_mem) {
            if ((d->reg & 7) >= 5) { emit(d, named("fence")); return 0; }   // lfence mfence sfence
            return unimplemented(d, "0f ae reg");
        }
        switch (d->reg & 7) {
        case 0: set_mem(d, emit(d, named("fxsave"))); return 0;
        case 1: set_mem(d, emit(d, named("fxrstor"))); return 0;
        case 2: set_mem(d, emit(d, named("ldmxcsr"))); return 0;
        case 3: set_mem(d, emit(d, named("stmxcsr"))); return 0;
        case 7: emit(d, named("nop")); return 0;   // clflush
        }
        return unimplemented(d, "xsave/xrstor");
    case 0xc7:                                     // cmpxchg8b/16b m
        modrm(d);
        if ((d->reg & 7) != 1 || !d->is_mem) return unimplemented(d, "0f c7");
        set_mem(d, emit(d, named(d->rexw ? "cmpxchg16b" : "cmpxchg8b")));
        meta(d)->reads = 1;   // only ZF changes
        return 0;
    case 0xaf: {
        modrm(d);
        Uop *u = emit(d, fxi_imul2_tab[d->is_mem][si_of(bits)]);
        u->dst = gpr(d, d->reg, bits); u->src = gpr(d, d->rm, bits);
        if (d->is_mem) set_mem(d, u);
        meta(d)->kill = 1;
        return 0;
    }
    case 0xb6: case 0xb7: case 0xbe: case 0xbf: {
        modrm(d);
        int sgn = op >= 0xbe, ss = (op & 1) ? 16 : 8;
        Uop *u = emit(d, fxi_ext_tab[sgn][d->is_mem][ss == 16][si_of(bits)]);
        u->dst = gpr(d, d->reg, bits); u->src = gpr(d, d->rm, ss);
        if (d->is_mem) set_mem(d, u);
        return 0;
    }
    case 0xb8: case 0xbc: case 0xbd: {
        if (op == 0xb8 && !d->rep) return unimplemented(d, "jmpe");
        modrm(d);
        const char *n = op == 0xb8 ? "popcnt" : op == 0xbc ? (d->rep ? "tzcnt" : "bsf") : (d->rep ? "lzcnt" : "bsr");
        char nm[16]; snprintf(nm, sizeof nm, "%s_%s", n, d->is_mem ? "M" : "R");
        Uop *u = emit(d, named(nm));
        u->dst = gpr(d, d->reg, bits); u->src = gpr(d, d->rm, bits); u->cc = (uint8_t)si_of(bits);
        if (d->is_mem) set_mem(d, u);
        meta(d)->reads = 1;
        return 0;
    }
    default:
        break;
    }
    if (op == 0xc3) {                                 // movnti m32/m64, r (a plain store here)
        int bits = d->rexw ? 64 : 32;
        modrm(d);
        if (!d->is_mem) return unimplemented(d, "movnti reg");
        emit_mov(d, F_MR, bits, 0, gpr(d, d->reg, bits), 0);
        return 0;
    }
    if ((op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2f) || (op >= 0x50 && op <= 0x7f) ||
        op == 0xc2 || (op >= 0xc4 && op <= 0xc6) || op >= 0xd0)
        return decode_sse(d, op);
    return unimplemented(d, "two-byte opcode");
}

// A segment override on an operand that already uses both base and index has no free slot
// for the segment base: drop what the instruction emitted and fail it (only if executed).
static int decode_one(Dec *d) {
    int n0 = d->n;
    int ended = decode_one_inner(d);
    if (d->seg_bad) {
        d->n = n0;
        d->fuse_at = -1;
        return unimplemented(d, "fs/gs override with base and index");
    }
    if (d->addr32_used) {      // 67 on a memory operand: 32-bit address arithmetic (not supported)
        d->n = n0;
        d->fuse_at = -1;
        return unimplemented(d, "32-bit addressing");
    }
    return ended;
}

// ---- x86-64 instruction length, independent of what FXI implements ----
// Keeps a linear sweep (fxi --scan-pe) in step after an instruction FXI rejects.
// Returns the length (0: invalid/unknown); *op_end = bytes up to and including the opcode.
int fxi_insn_length(const uint8_t *p, int *op_end) {
    const uint8_t *s = p;
    int osz16 = 0, rexw = 0;
    for (;; p++) {
        uint8_t b = *p;
        if (b == 0x66) osz16 = 1;
        else if (b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x2e || b == 0x36 ||
                 b == 0x3e || b == 0x26 || b == 0x64 || b == 0x65) { }
        else break;
        if (p - s > 14) return 0;
    }
    while ((*p & 0xf0) == 0x40) { rexw = (*p >> 3) & 1; p++; }   // the last REX counts
    int immz = osz16 ? 2 : 4, has_modrm = 0, imm = 0, map = 0;
    uint8_t b = *p++;
    if (b == 0xc4 || b == 0xc5 || b == 0x62) {   // VEX / EVEX
        if (b == 0xc5) { map = 1; p += 1; }
        else if (b == 0xc4) { map = p[0] & 0x1f; p += 2; }
        else { map = p[0] & 7; p += 3; }
        b = *p++;
        has_modrm = 1;
        if (map == 3) imm = 1;
        else if (map == 1 && (b == 0x70 || b == 0x71 || b == 0x72 || b == 0x73 || b == 0xc2 || b == 0xc4 || b == 0xc5 || b == 0xc6)) imm = 1;
    } else if (b == 0x0f) {
        b = *p++;
        if (b == 0x38) { p++; has_modrm = 1; }
        else if (b == 0x3a) { p++; has_modrm = 1; imm = 1; }
        else if (b == 0x0f) { has_modrm = 1; imm = 1; }   // 3DNow!
        else if (b >= 0x80 && b <= 0x8f) imm = 4;
        else if (b == 0x05 || b == 0x06 || b == 0x07 || b == 0x08 || b == 0x09 || b == 0x0b || b == 0x0e ||
                 (b >= 0x30 && b <= 0x37) || b == 0x77 || b == 0xa0 || b == 0xa1 || b == 0xa2 || b == 0xa8 ||
                 b == 0xa9 || b == 0xaa || (b >= 0xc8 && b <= 0xcf)) { }
        else {
            has_modrm = 1;
            if ((b >= 0x70 && b <= 0x73) || b == 0xa4 || b == 0xac || b == 0xba || b == 0xc2 || b == 0xc4 ||
                b == 0xc5 || b == 0xc6) imm = 1;
        }
    } else {
        if (b < 0x40) {
            if ((b & 7) < 4) has_modrm = 1;
            else if ((b & 7) == 4) imm = 1;
            else if ((b & 7) == 5) imm = immz;
            else if (b != 0x26 && b != 0x2e && b != 0x36 && b != 0x3e) return 0;   // invalid in 64-bit mode
        }
        else if (b <= 0x5f) { }
        else if (b == 0x63 || (b >= 0x84 && b <= 0x8f) || (b >= 0xd0 && b <= 0xd3) || (b >= 0xd8 && b <= 0xdf) ||
                 b == 0xfe || b == 0xff) has_modrm = 1;
        else if (b == 0x68 || b == 0xe8 || b == 0xe9) imm = b == 0x68 ? immz : 4;
        else if (b == 0x69 || b == 0x81 || b == 0xc7) { has_modrm = 1; imm = immz; }
        else if (b == 0x6b || b == 0x80 || b == 0x83 || b == 0xc0 || b == 0xc1 || b == 0xc6) { has_modrm = 1; imm = 1; }
        else if (b == 0x6a || (b >= 0x70 && b <= 0x7f) || b == 0xa8 || (b >= 0xb0 && b <= 0xb7) || b == 0xcd ||
                 (b >= 0xe0 && b <= 0xe7) || b == 0xeb) imm = 1;
        else if (b == 0xa9) imm = immz;
        else if (b >= 0xa0 && b <= 0xa3) imm = 8;
        else if (b >= 0xb8 && b <= 0xbf) imm = rexw ? 8 : immz;
        else if (b == 0xc2 || b == 0xca) imm = 2;
        else if (b == 0xc8) imm = 3;
        else if (b == 0xf6 || b == 0xf7) {
            has_modrm = 1;
            if (((p[0] >> 3) & 7) <= 1) imm = b == 0xf6 ? 1 : immz;
        }
        else if ((b >= 0x6c && b <= 0x6f) || (b >= 0x90 && b <= 0x99) || (b >= 0x9b && b <= 0x9f) ||
                 (b >= 0xa4 && b <= 0xa7) || (b >= 0xaa && b <= 0xaf) || b == 0xc3 || b == 0xc9 || b == 0xcb ||
                 b == 0xcc || b == 0xcf || b == 0xd7 || (b >= 0xec && b <= 0xef) || b == 0xf1 || b == 0xf4 ||
                 b == 0xf5 || (b >= 0xf8 && b <= 0xfd)) { }
        else return 0;
    }
    if (op_end) *op_end = (int)(p - s);
    if (has_modrm) {
        uint8_t m = *p++;
        int mod = m >> 6, rm = m & 7;
        if (mod != 3) {
            if (rm == 4) { uint8_t sib = *p++; if (mod == 0 && (sib & 7) == 5) p += 4; }
            else if (mod == 0 && rm == 5) p += 4;
            if (mod == 1) p += 1; else if (mod == 2) p += 4;
        }
    }
    p += imm;
    return (int)(p - s);
}

// Does FXI's decoder accept the instruction at host address p (decoded as if at rip)?
// Returns 1 and leaves why empty if so; else 0 with the decoder's reason in why.
int fxi_probe(const uint8_t *p, uint64_t rip, char *why, size_t why_len) {
    Dec *d = malloc(sizeof *d);
    d->p = p; d->rip = rip; d->n = 0; d->fuse_at = -1;
    decode_one(d);
    OpFn ud = named("fail_ud");
    int ok = 1;
    why[0] = 0;
    for (int i = 0; i < d->n; i++) {
        if (d->u[i].fn != ud) continue;
        char *msg = (char *)(uintptr_t)d->u[i].imm;
        snprintf(why, why_len, "%s", msg);
        free(msg);
        ok = 0;
    }
    free(d);
    return ok;
}

Block *fxi_translate(struct Fxi *vm, uint64_t rip) {
    Dec *d = malloc(sizeof *d);
    d->p = (const uint8_t *)(uintptr_t)rip;
    d->n = 0;
    d->fuse_at = -1;
    int ended = 0;
    for (int insns = 0; insns < MAX_INSNS && !ended; insns++) {
        d->rip = (uint64_t)(uintptr_t)d->p;
        ended = decode_one(d);
    }
    if (!ended) { Uop *u = emit(d, named("goto")); u->aux = (uint64_t)(uintptr_t)d->p; }

    // Flag liveness, backward. Flags are assumed live at the block end.
    int live = 1;
    for (int i = d->n - 1; i >= 0; i--) {
        Meta *m = &d->m[i];
        if (m->cc_live) { d->u[i].cc = (uint8_t)live; m->reads = (uint8_t)live; }
        if (m->kill && !live && m->alt) d->u[i].fn = m->alt;
        live = m->reads || (live && !m->kill);
    }

    Block *b = malloc(sizeof(Block) + (size_t)d->n * sizeof(Uop));
    b->rip = rip;
    b->n = (uint32_t)d->n;
    memcpy(b->u, d->u, (size_t)d->n * sizeof(Uop));
    free(d);
    vm->blocks++;
    return fxr_lower(vm, b);   // FXR: pinned handlers
}
