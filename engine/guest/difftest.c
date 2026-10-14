// SPDX-License-Identifier: GPL-3.0-or-later
// Differential instruction test: every common integer instruction form runs on hundreds of
// random and edge-case inputs, with random starting flags, and each form prints one hash of
// its results and flags. engine/fxi/ci-test.sh (and engine/fxr/compare.sh) require FXI's and
// FXR's output to equal the native run's line for line, so a wrong result, flag, operand
// size, zero/sign extension or decode shows up on CI, named, before a device build.
// Flags a CPU leaves undefined are masked per form (Intel and AMD runners differ there).
// Built with -mno-red-zone (pushfq/popfq inside the asm).

#include "guest_rt.h"

typedef unsigned long long u64;
typedef unsigned int u32;

#define N 300
#define ARITH 0x8d5ull     // CF PF AF ZF SF OF
#define LOGIC 0x8c5ull     // AF undefined after and/or/xor/test
#define CFOF 0x801ull      // mul/imul: only CF and OF defined

static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u64 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static const u64 kEdge[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000,
    0xffffffff, 0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull,
    0xfffffffffffffffeull, 0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull, 0x0123456789abcdefull,
};
#define NEDGE (sizeof kEdge / sizeof kEdge[0])
// Edge values a third of the time, small values a sixth, random otherwise.
static u64 val(void) {
    u64 r = rnd();
    switch (r % 6) {
    case 0: case 1: return kEdge[(r >> 8) % NEDGE];
    case 2: return (r >> 8) & 0xff;
    default: return rnd();
    }
}
static u64 flags_in(void) { return rnd() & ARITH; }

static u64 mix(u64 h, u64 v) { h ^= v; h *= 1099511628211ull; return h ^ (h >> 29); }
static void out(const char *name, u64 h) { g_puts(name); g_puts(" "); g_putu(h); g_puts("\n"); }

#define PRE "pushq %[fi]\n\tpopfq\n\t"
#define POST "\n\tpushfq\n\tpopq %[f]"

// r = r OP b (reg, reg)
#define RR(NAME, INSN, MASK)                                                                  \
    static void NAME(void) {                                                                  \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 r = val(), b = val(), fi = flags_in(), f;                                     \
            __asm__ volatile(PRE INSN POST : [r] "+r"(r), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(h, r), f & (MASK));                                                   \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// [m] = [m] OP b (memory destination) and r = r OP [m] (memory source)
#define RM(NAME, INSN, MASK)                                                                  \
    static void NAME(void) {                                                                  \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 r = val(), m = val(), fi = flags_in(), f;                                     \
            __asm__ volatile(PRE INSN POST : [r] "+r"(r), [m] "+m"(m), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), m), f & (MASK));                                           \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// r = OP r
#define R1(NAME, INSN, MASK)                                                                  \
    static void NAME(void) {                                                                  \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 r = val(), fi = flags_in(), f;                                                \
            __asm__ volatile(PRE INSN POST : [r] "+r"(r), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(h, r), f & (MASK));                                                   \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// Shift/rotate by CL: count from CNT (an expression of rnd()), flags mask from the masked
// count c (MASKEXPR may use c).
#define SH(NAME, INSN, CNT, CMASK, MASKEXPR)                                                  \
    static void NAME(void) {                                                                  \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 r = val(), b = val(), fi = flags_in(), f, cnt = (CNT);                        \
            u64 c = cnt & (CMASK);                                                            \
            __asm__ volatile(PRE INSN POST : [r] "+r"(r), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi), "c"(cnt) : "cc", "memory"); \
            h = mix(mix(h, r), f & (MASKEXPR));                                               \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// Shifts: masked count 0 leaves every flag alone; OF only for a count of 1; AF undefined.
#define SHIFT_MASK (c == 0 ? ARITH : (0xc5ull | (c == 1 ? 0x800ull : 0)))
// Rotates (rol/ror/rcl/rcr): only CF and OF (count 1) change; the rest must stay as they were.
#define ROT_MASK (c == 0 ? ARITH : (0xd5ull | (c == 1 ? 0x800ull : 0)))

#define ALU4(OP, MASK)                                                                        \
    RR(OP##_q, #OP "q %[b], %[r]", MASK) RR(OP##_l, #OP "l %k[b], %k[r]", MASK)               \
    RR(OP##_w, #OP "w %w[b], %w[r]", MASK) RR(OP##_b, #OP "b %b[b], %b[r]", MASK)             \
    RM(OP##_mr_q, #OP "q %[r], %[m]", MASK) RM(OP##_rm_l, #OP "l %[m], %k[r]", MASK)          \
    RM(OP##_mr_b, #OP "b %b[r], %[m]", MASK)
ALU4(add, ARITH) ALU4(adc, ARITH) ALU4(sub, ARITH) ALU4(sbb, ARITH) ALU4(cmp, ARITH)
ALU4(and, LOGIC) ALU4(or, LOGIC) ALU4(xor, LOGIC)
RR(test_q, "testq %[b], %[r]", LOGIC) RR(test_l, "testl %k[b], %k[r]", LOGIC) RR(test_b, "testb %b[b], %b[r]", LOGIC)
RR(add_imm8_q, "addq $-5, %[r]", ARITH) RR(sub_imm32_l, "subl $0x12345678, %k[r]", ARITH)
RR(and_imm_q, "andq $0x7fffff00, %[r]", LOGIC) RR(cmp_imm8_w, "cmpw $-1, %w[r]", ARITH)
RR(adc_imm_b, "adcb $0x7f, %b[r]", ARITH) RR(sbb_imm_q, "sbbq $1, %[r]", ARITH)
// a 67 (address-size) prefix on register-only instructions is padding (Stick Fight: 67 49 f7 e6)
RR(a32_add_q, ".byte 0x67\n\taddq %[b], %[r]", ARITH) RR(a32_imul_l, ".byte 0x67\n\timull %k[b], %k[r]", CFOF)

R1(inc_q, "incq %[r]", ARITH) R1(inc_l, "incl %k[r]", ARITH) R1(inc_w, "incw %w[r]", ARITH) R1(inc_b, "incb %b[r]", ARITH)
R1(dec_q, "decq %[r]", ARITH) R1(dec_l, "decl %k[r]", ARITH) R1(dec_b, "decb %b[r]", ARITH)
R1(neg_q, "negq %[r]", ARITH) R1(neg_l, "negl %k[r]", ARITH) R1(neg_w, "negw %w[r]", ARITH) R1(neg_b, "negb %b[r]", ARITH)
R1(not_q, "notq %[r]", ARITH) R1(not_l, "notl %k[r]", ARITH) R1(not_b, "notb %b[r]", ARITH)
R1(bswap_q, "bswapq %[r]", ARITH) R1(bswap_l, "bswapl %k[r]", ARITH)
R1(shl1_q, "shlq $1, %[r]", 0xcd5ull & ~0x10ull) R1(sar1_l, "sarl $1, %k[r]", 0xcd5ull & ~0x10ull)
R1(shr1_b, "shrb $1, %b[r]", 0xcd5ull & ~0x10ull) R1(rol1_q, "rolq $1, %[r]", 0x8d5ull) R1(ror1_w, "rorw $1, %w[r]", 0x8d5ull)
R1(rcl1_l, "rcll $1, %k[r]", 0x8d5ull) R1(rcr1_b, "rcrb $1, %b[r]", 0x8d5ull)
R1(shl_imm_q, "shlq $13, %[r]", 0xc5ull) R1(shr_imm_l, "shrl $31, %k[r]", 0xc5ull) R1(sar_imm_w, "sarw $7, %w[r]", 0xc5ull)
R1(rol_imm_b, "rolb $3, %b[r]", 0xd5ull) R1(ror_imm_q, "rorq $40, %[r]", 0xd5ull)

SH(shl_q, "shlq %%cl, %[r]", rnd() & 0x7f, 63, SHIFT_MASK) SH(shl_l, "shll %%cl, %k[r]", rnd() & 0x7f, 31, SHIFT_MASK)
SH(shl_w, "shlw %%cl, %w[r]", rnd() % 16, 31, SHIFT_MASK) SH(shl_b, "shlb %%cl, %b[r]", rnd() % 8, 31, SHIFT_MASK)
SH(shr_q, "shrq %%cl, %[r]", rnd() & 0x7f, 63, SHIFT_MASK) SH(shr_l, "shrl %%cl, %k[r]", rnd() & 0x7f, 31, SHIFT_MASK)
SH(shr_w, "shrw %%cl, %w[r]", rnd() % 16, 31, SHIFT_MASK) SH(shr_b, "shrb %%cl, %b[r]", rnd() % 8, 31, SHIFT_MASK)
SH(sar_q, "sarq %%cl, %[r]", rnd() & 0x7f, 63, SHIFT_MASK) SH(sar_l, "sarl %%cl, %k[r]", rnd() & 0x7f, 31, SHIFT_MASK)
SH(sar_w, "sarw %%cl, %w[r]", rnd() % 16, 31, SHIFT_MASK) SH(sar_b, "sarb %%cl, %b[r]", rnd() % 8, 31, SHIFT_MASK)
SH(rol_q, "rolq %%cl, %[r]", rnd() & 0x7f, 63, ROT_MASK) SH(rol_l, "roll %%cl, %k[r]", rnd() & 0x7f, 31, ROT_MASK)
SH(rol_w, "rolw %%cl, %w[r]", rnd() & 0x3f, 31, ROT_MASK) SH(rol_b, "rolb %%cl, %b[r]", rnd() & 0x3f, 31, ROT_MASK)
SH(ror_q, "rorq %%cl, %[r]", rnd() & 0x7f, 63, ROT_MASK) SH(ror_l, "rorl %%cl, %k[r]", rnd() & 0x7f, 31, ROT_MASK)
SH(ror_w, "rorw %%cl, %w[r]", rnd() & 0x3f, 31, ROT_MASK) SH(ror_b, "rorb %%cl, %b[r]", rnd() & 0x3f, 31, ROT_MASK)
SH(rcl_q, "rclq %%cl, %[r]", rnd() & 0x7f, 63, ROT_MASK) SH(rcl_l, "rcll %%cl, %k[r]", rnd() & 0x7f, 31, ROT_MASK)
SH(rcl_w, "rclw %%cl, %w[r]", rnd() & 0x3f, 31, ROT_MASK) SH(rcl_b, "rclb %%cl, %b[r]", rnd() & 0x3f, 31, ROT_MASK)
SH(rcr_q, "rcrq %%cl, %[r]", rnd() & 0x7f, 63, ROT_MASK) SH(rcr_l, "rcrl %%cl, %k[r]", rnd() & 0x7f, 31, ROT_MASK)
SH(rcr_w, "rcrw %%cl, %w[r]", rnd() & 0x3f, 31, ROT_MASK) SH(rcr_b, "rcrb %%cl, %b[r]", rnd() & 0x3f, 31, ROT_MASK)
SH(shld_q, "shldq %%cl, %[b], %[r]", rnd() & 0x7f, 63, SHIFT_MASK) SH(shld_l, "shldl %%cl, %k[b], %k[r]", rnd() & 0x7f, 31, SHIFT_MASK)
SH(shld_w, "shldw %%cl, %w[b], %w[r]", rnd() % 16, 31, SHIFT_MASK)
SH(shrd_q, "shrdq %%cl, %[b], %[r]", rnd() & 0x7f, 63, SHIFT_MASK) SH(shrd_l, "shrdl %%cl, %k[b], %k[r]", rnd() & 0x7f, 31, SHIFT_MASK)
SH(shrd_w, "shrdw %%cl, %w[b], %w[r]", rnd() % 16, 31, SHIFT_MASK)

RR(imul2_q, "imulq %[b], %[r]", CFOF) RR(imul2_l, "imull %k[b], %k[r]", CFOF) RR(imul2_w, "imulw %w[b], %w[r]", CFOF)
RR(imul3_q, "imulq $-7, %[b], %[r]", CFOF) RR(imul3_l, "imull $100000, %k[b], %k[r]", CFOF) RR(imul3_w, "imulw $300, %w[b], %w[r]", CFOF)
RR(movsbq, "movsbq %b[b], %[r]", ARITH) RR(movswq, "movswq %w[b], %[r]", ARITH) RR(movslq, "movslq %k[b], %[r]", ARITH)
RR(movsbl, "movsbl %b[b], %k[r]", ARITH) RR(movsbw, "movsbw %b[b], %w[r]", ARITH) RR(movzbl, "movzbl %b[b], %k[r]", ARITH)
RR(movzwl, "movzwl %w[b], %k[r]", ARITH) RR(movzbw, "movzbw %b[b], %w[r]", ARITH) RR(movl_zext, "movl %k[b], %k[r]", ARITH)
RR(movw_keep, "movw %w[b], %w[r]", ARITH) RR(movb_keep, "movb %b[b], %b[r]", ARITH)
RR(lea_q, "leaq 0x12(%[r],%[b],4), %[r]", ARITH) RR(lea_l, "leal -3(%[r],%[b],8), %k[r]", ARITH)
RR(lea_w, "leaw 7(%[r],%[b]), %w[r]", ARITH)
// 67 lea (32-bit address registers make the assembler emit the prefix; Dokimon: lea eax, [edx+ecx])
RR(lea_a32_l, "leal 5(%k[r],%k[b],2), %k[r]", ARITH) RR(lea_a32_q, "leaq -9(%k[r],%k[b],8), %[r]", ARITH)
RR(lea_a32_w, "leaw 3(%k[r],%k[b]), %w[r]", ARITH)
// Stacked REX prefixes: only the last one counts (Valve's client: 48 41 ff e2, jmp r10)
RR(rex2_add_q, ".byte 0x40\n\taddq %[b], %[r]", ARITH) RR(rex2_mov_l, ".byte 0x48\n\tmovl %k[b], %k[r]", ARITH)
RR(xchg_q, "xchgq %[b], %[r]", ARITH) RR(xchg_b, "xchgb %b[b], %b[r]", ARITH)
RR(xadd_q, "xaddq %[b], %[r]", ARITH) RR(xadd_w, "xaddw %w[b], %w[r]", ARITH)
RR(bt_q, "btq %[b], %[r]", 0x1ull) RR(bts_l, "btsl %k[b], %k[r]", 0x1ull) RR(btr_q, "btrq %[b], %[r]", 0x1ull)
RR(btc_w, "btcw %w[b], %w[r]", 0x1ull) RR(bt_imm_q, "btq $45, %[r]", 0x1ull) RR(bts_imm_l, "btsl $3, %k[r]", 0x1ull)

// bsf/bsr: nonzero source (a zero source leaves the destination undefined); only ZF defined.
#define BS(NAME, INSN)                                                                        \
    static void NAME(void) {                                                                  \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 r = val(), b = val() | (1ull << (rnd() & 63)), fi = flags_in(), f;            \
            __asm__ volatile(PRE INSN POST : [r] "+r"(r), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(h, r), f & 0x40ull);                                                  \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
BS(bsf_q, "bsfq %[b], %[r]") BS(bsr_q, "bsrq %[b], %[r]")

// cmov / setcc / fused cmp+jcc for all 16 conditions, after cmp of two values.
#define CC(X)                                                                                 \
    static void cc_##X(void) {                                                                \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 a = val(), b = (i & 3) ? val() : a, c = val(), r = val(), s = val(), j;       \
            __asm__ volatile("cmpq %[b], %[a]\n\tcmov" #X "q %[c], %[r]\n\tset" #X " %b[s]"    \
                             : [r] "+r"(r), [s] "+r"(s) : [a] "r"(a), [b] "r"(b), [c] "r"(c) : "cc"); \
            __asm__ volatile("cmpl %k[b], %k[a]\n\tj" #X " 1f\n\tmovq $0, %[j]\n\tjmp 2f\n1:\n\tmovq $1, %[j]\n2:" \
                             : [j] "=r"(j) : [a] "r"(a), [b] "r"(b) : "cc");                   \
            u32 r32 = (u32)a;                                                                 \
            __asm__ volatile("testb %b[b], %b[a]\n\tcmov" #X "l %k[c], %k[t]"                  \
                             : [t] "+r"(r32) : [a] "r"(a), [b] "r"(b), [c] "r"(c) : "cc");      \
            h = mix(mix(mix(mix(h, r), s), j), r32);                                          \
        }                                                                                     \
        out("cc_" #X, h);                                                                     \
    }
CC(o) CC(no) CC(b) CC(ae) CC(e) CC(ne) CC(be) CC(a) CC(s) CC(ns) CC(p) CC(np) CC(l) CC(ge) CC(le) CC(g)

// mul / imul (one operand), div / idiv (no #DE: the quotient always fits).
static void muldiv(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        u64 a = val(), b = val(), fi = flags_in(), f, lo = a, hi = 0;
        __asm__ volatile(PRE "mulq %[b]" POST : "+a"(lo), "=d"(hi), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, lo), hi), f & CFOF);
        lo = a; hi = 0;
        __asm__ volatile(PRE "imulq %[b]" POST : "+a"(lo), "=d"(hi), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, lo), hi), f & CFOF);
        lo = a;
        __asm__ volatile(PRE "mull %k[b]" POST : "+a"(lo), "=d"(hi), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, lo), hi), f & CFOF);
        lo = a;
        __asm__ volatile(PRE "imulw %w[b]" POST : "+a"(lo), "+d"(hi), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, lo), hi), f & CFOF);
        lo = a;
        __asm__ volatile(PRE "mulb %b[b]" POST : "+a"(lo), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(h, lo), f & CFOF);
        lo = a;
        __asm__ volatile(PRE "imulb %b[b]" POST : "+a"(lo), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(h, lo), f & CFOF);
        // div: divisor nonzero, high half below the divisor
        u64 d = b | 1, q = a, r = rnd() % d;
        __asm__ volatile("divq %[d]" : "+a"(q), "+d"(r) : [d] "r"(d) : "cc");
        h = mix(mix(h, q), r);
        u32 d32 = (u32)b | 1, q32 = (u32)a, r32 = (u32)rnd() % d32;
        __asm__ volatile("divl %[d]" : "+a"(q32), "+d"(r32) : [d] "r"(d32) : "cc");
        h = mix(mix(h, q32), r32);
        // idiv: sign-extended dividend, divisor not 0 and not -1 with INT_MIN
        long long sd = (long long)((b | 2) & ~1ull), sq = (long long)a, sr;   // even: never 0 or -1
        __asm__ volatile("cqto\n\tidivq %[d]" : "+a"(sq), "=&d"(sr) : [d] "r"(sd) : "cc");
        h = mix(mix(h, (u64)sq), (u64)sr);
        int sd32 = (int)((b | 2) & ~1u), sq32 = (int)a, sr32;
        __asm__ volatile("cltd\n\tidivl %[d]" : "+a"(sq32), "=&d"(sr32) : [d] "r"(sd32) : "cc");
        h = mix(mix(h, (u64)(u32)sq32), (u64)(u32)sr32);
        u64 w = a & 0xffff, wr = 0;   // 16-bit: dx:ax / r16
        u64 wd = (b & 0xffff) | 0x100;
        __asm__ volatile("xorl %%edx, %%edx\n\tdivw %w[d]" : "+a"(w), "+d"(wr) : [d] "r"(wd) : "cc");
        h = mix(mix(h, w & 0xffff), wr & 0xffff);
        u64 bq = a & 0xffff, bd = (b & 0xff) | 0x80;   // 8-bit: ax / r8, quotient < 0x100
        bq &= 0x7fff;
        __asm__ volatile("divb %b[d]" : "+a"(bq) : [d] "q"(bd) : "cc");
        h = mix(h, bq & 0xffff);
    }
    out("muldiv", h);
}

// cbw/cwde/cdqe/cwd/cdq/cqo, lahf/sahf, cmpxchg (reg), xchg with rax
static void misc(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        u64 a = val(), d = val(), fi = flags_in(), f;
        u64 x = a; __asm__ volatile("cbtw" : "+a"(x)); h = mix(h, x);
        x = a; __asm__ volatile("cwtl" : "+a"(x)); h = mix(h, x);
        x = a; __asm__ volatile("cltq" : "+a"(x)); h = mix(h, x);
        x = a; u64 y = d; __asm__ volatile("cwtd" : "+a"(x), "+d"(y)); h = mix(mix(h, x), y);
        x = a; y = d; __asm__ volatile("cltd" : "+a"(x), "+d"(y)); h = mix(mix(h, x), y);
        x = a; y = d; __asm__ volatile("cqto" : "+a"(x), "+d"(y)); h = mix(mix(h, x), y);
        x = a;
        __asm__ volatile(PRE "lahf" POST : "+a"(x), [f] "=&r"(f) : [fi] "r"(fi) : "cc");
        h = mix(mix(h, x & 0xd5ff), f & ARITH);   // AH bit 1 is always 1; bits 3/5 are 0
        x = a;
        __asm__ volatile(PRE "sahf" POST : "+a"(x), [f] "=&r"(f) : [fi] "r"(fi) : "cc");
        h = mix(h, f & ARITH);
        u64 m = (i & 1) ? a : val(), r = a, s = d;
        __asm__ volatile(PRE "cmpxchgq %[s], %[m]" POST : "+a"(r), [m] "+r"(m), [f] "=&r"(f) : [s] "r"(s), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, r), m), f & ARITH);
        u32 m32 = (i & 2) ? (u32)a : (u32)val(), r32 = (u32)a;
        __asm__ volatile(PRE "cmpxchgl %k[s], %[m]" POST : "+a"(r32), [m] "+r"(m32), [f] "=&r"(f) : [s] "r"(s), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, r32), m32), f & ARITH);
    }
    out("misc", h);
}

// ---- SSE/SSE2 (what FXI implements; CPUID reports SSE2 only) ----
typedef long long v2 __attribute__((vector_size(16)));
// Integer lanes: random bits. Float lanes: exact small values (k/8) so results are exact or
// IEEE-rounded identically on every host; no 0/0 (x86 and ARM default NaNs differ in sign).
static v2 vrand(void) { v2 v = { (long long)val(), (long long)val() }; return v; }
static v2 vfloat(int nonzero) {
    union { float f[4]; v2 v; } u;
    for (int i = 0; i < 4; i++) { int k = (int)(rnd() % 2001) - 1000; if (nonzero && !k) k = 3; u.f[i] = (float)k / 8.0f; }
    return u.v;
}
static v2 vdouble(int nonzero) {
    union { double g[2]; v2 v; } u;
    for (int i = 0; i < 2; i++) { long long k = (long long)(rnd() % 4000001) - 2000000; if (nonzero && !k) k = 5; u.g[i] = (double)k / 16.0; }
    return u.v;
}
static u64 vmix(u64 h, v2 v) { return mix(mix(h, (u64)v[0]), (u64)v[1]); }
#define XX(NAME, GEN_A, GEN_B, INSN)                                                          \
    static void NAME(void) {                                                                  \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            v2 a = GEN_A, b = GEN_B, m = GEN_B;                                               \
            __asm__ volatile(INSN : [a] "+x"(a) : [b] "x"(b), [m] "m"(m));                    \
            h = vmix(h, a);                                                                   \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
#define XI(NAME, INSN) XX(NAME, vrand(), vrand(), INSN)
XI(x_paddsb, "paddsb %[b], %[a]") XI(x_paddsw, "paddsw %[m], %[a]") XI(x_paddusb, "paddusb %[b], %[a]")
XI(x_paddusw, "paddusw %[b], %[a]") XI(x_psubsb, "psubsb %[b], %[a]") XI(x_psubsw, "psubsw %[b], %[a]")
XI(x_psubusb, "psubusb %[m], %[a]") XI(x_psubusw, "psubusw %[b], %[a]") XI(x_pavgb, "pavgb %[b], %[a]")
XI(x_pavgw, "pavgw %[b], %[a]") XI(x_pmulhw, "pmulhw %[b], %[a]") XI(x_pmulhuw, "pmulhuw %[m], %[a]")
XI(x_pmaddwd, "pmaddwd %[b], %[a]") XI(x_pmullw, "pmullw %[b], %[a]") XI(x_pmuludq, "pmuludq %[b], %[a]")
XI(x_paddb, "paddb %[b], %[a]") XI(x_paddq, "paddq %[m], %[a]") XI(x_psubd, "psubd %[b], %[a]")
XI(x_pcmpgtb, "pcmpgtb %[b], %[a]") XI(x_pcmpeqw, "pcmpeqw %[b], %[a]") XI(x_pcmpgtd, "pcmpgtd %[b], %[a]")
XI(x_pminub, "pminub %[b], %[a]") XI(x_pmaxsw, "pmaxsw %[b], %[a]") XI(x_psadbw, "psadbw %[b], %[a]")
XI(x_pand, "pand %[b], %[a]") XI(x_pandn, "pandn %[b], %[a]") XI(x_por, "por %[m], %[a]") XI(x_pxor, "pxor %[b], %[a]")
XI(x_punpcklbw, "punpcklbw %[b], %[a]") XI(x_punpckhwd, "punpckhwd %[b], %[a]") XI(x_punpckldq, "punpckldq %[b], %[a]")
XI(x_punpckhqdq, "punpckhqdq %[b], %[a]") XI(x_packsswb, "packsswb %[b], %[a]") XI(x_packuswb, "packuswb %[b], %[a]")
XI(x_packssdw, "packssdw %[b], %[a]")
XI(x_pshufd, "pshufd $0x1b, %[b], %[a]") XI(x_pshuflw, "pshuflw $0x93, %[b], %[a]") XI(x_pshufhw, "pshufhw $0x4e, %[m], %[a]")
XI(x_shufps, "shufps $0xb1, %[b], %[a]") XI(x_shufpd, "shufpd $1, %[b], %[a]")
XI(x_psllw_i, "psllw $5, %[a]") XI(x_psrad_i, "psrad $9, %[a]") XI(x_psrlq_i, "psrlq $33, %[a]")
XI(x_pslldq_i, "pslldq $3, %[a]") XI(x_psrldq_i, "psrldq $11, %[a]")
// shifts by an XMM count: small counts most of the time, sometimes out of range
#define XSH(NAME, INSN) XX(NAME, vrand(), ((v2){ (long long)((rnd() & 3) ? rnd() % 70 : rnd()), 0 }), INSN)
XSH(x_psllw_x, "psllw %[b], %[a]") XSH(x_pslld_x, "pslld %[b], %[a]") XSH(x_psllq_x, "psllq %[m], %[a]")
XSH(x_psrlw_x, "psrlw %[b], %[a]") XSH(x_psrld_x, "psrld %[b], %[a]") XSH(x_psrlq_x, "psrlq %[b], %[a]")
XSH(x_psraw_x, "psraw %[b], %[a]") XSH(x_psrad_x, "psrad %[m], %[a]")
#define XF(NAME, NZ, INSN) XX(NAME, vfloat(0), vfloat(NZ), INSN)
#define XD(NAME, NZ, INSN) XX(NAME, vdouble(0), vdouble(NZ), INSN)
XF(x_addps, 0, "addps %[b], %[a]") XF(x_subps, 0, "subps %[m], %[a]") XF(x_mulps, 0, "mulps %[b], %[a]")
XF(x_divps, 1, "divps %[b], %[a]") XF(x_minps, 0, "minps %[b], %[a]") XF(x_maxps, 0, "maxps %[b], %[a]")
XF(x_cmpps_lt, 0, "cmpltps %[b], %[a]") XF(x_cmpps_neq, 0, "cmpneqps %[b], %[a]")
XF(x_cvtps2dq, 0, "cvtps2dq %[b], %[a]") XF(x_cvttps2dq, 0, "cvttps2dq %[b], %[a]") XF(x_cvtps2pd, 0, "cvtps2pd %[b], %[a]")
XF(x_addss, 0, "addss %[b], %[a]") XF(x_divss, 1, "divss %[m], %[a]") XF(x_cvtss2sd, 0, "cvtss2sd %[b], %[a]")
XD(x_addpd, 0, "addpd %[b], %[a]") XD(x_mulpd, 0, "mulpd %[m], %[a]") XD(x_divpd, 1, "divpd %[b], %[a]")
XD(x_minpd, 0, "minpd %[b], %[a]") XD(x_maxsd, 0, "maxsd %[b], %[a]") XD(x_subsd, 0, "subsd %[b], %[a]")
XD(x_cvtpd2dq, 0, "cvtpd2dq %[b], %[a]") XD(x_cvttpd2dq, 0, "cvttpd2dq %[b], %[a]") XD(x_cvtpd2ps, 0, "cvtpd2ps %[b], %[a]")
XD(x_cvtsd2ss, 0, "cvtsd2ss %[b], %[a]") XD(x_cmpsd_le, 0, "cmplesd %[b], %[a]")
XI(x_cvtdq2ps, "cvtdq2ps %[b], %[a]") XI(x_cvtdq2pd, "cvtdq2pd %[b], %[a]")
XI(x_movsd_rr, "movsd %[b], %[a]") XI(x_movss_rr, "movss %[b], %[a]") XI(x_movhlps, "movhlps %[b], %[a]")
XI(x_movlhps, "movlhps %[b], %[a]") XI(x_movq_rr, "movq %[b], %[a]") XI(x_unpcklps, "unpcklps %[b], %[a]")
static void x_pinsrw(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vrand(); u64 g = val(); unsigned short m16 = (unsigned short)val();
        __asm__ volatile("pinsrw $5, %k[g], %[a]\n\tpinsrw $2, %[m], %[a]" : [a] "+x"(a) : [g] "r"(g), [m] "m"(m16));
        h = vmix(h, a);
    }
    out("x_pinsrw", h);
}

// XMM -> general register: movd/movq, pmovmskb, movmskps/pd, pextrw, cvt(t)sd2si, comisd flags.
static void x_togpr(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vrand(), d = vdouble(0), e = vdouble(0);
        u64 r1, r2, r3, r4, r5, r6, r7, r8, f;
        __asm__ volatile("movq %[a], %[r1]\n\tmovd %[a], %k[r2]\n\tpmovmskb %[a], %k[r3]\n\tmovmskps %[a], %k[r4]\n\t"
                         "movmskpd %[a], %k[r5]\n\tpextrw $6, %[a], %k[r6]"
                         : [r1] "=r"(r1), [r2] "=r"(r2), [r3] "=r"(r3), [r4] "=r"(r4), [r5] "=r"(r5), [r6] "=r"(r6) : [a] "x"(a));
        __asm__ volatile("cvtsd2si %[d], %[r7]\n\tcvttsd2si %[d], %k[r8]" : [r7] "=r"(r7), [r8] "=r"(r8) : [d] "x"(d));
        __asm__ volatile("comisd %[e], %[d]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(f) : [d] "x"(d), [e] "x"(e) : "cc", "memory");
        h = mix(mix(mix(mix(mix(mix(mix(mix(mix(h, r1), r2), r3), r4), r5), r6), r7), r8 & 0xffffffff), f & ARITH);
    }
    out("x_togpr", h);
}

// ---- instruction pairs FXR runs as one uop (superinstructions), for all 16 conditions: cmp/test
// of 8/16-bit registers and of memory, ALU + jcc, dec/inc + jcc, load + test + jcc, (u)comis + jcc.
// The flags are read after the branch too (the fused uop records them only when something does).
#define JX(X) "\n\tj" #X " 1f\n\tmovq $0, %[j]\n\tjmp 2f\n1:\n\tmovq $1, %[j]\n2:\n\tpushfq\n\tpopq %[f]"
#define JN(X) "\n\tj" #X " 1f\n\tmovq $0, %[j]\n\tjmp 2f\n1:\n\tmovq $1, %[j]\n2:"
static u64 g_m[4];
#define FUSED_CC(X)                                                                           \
    static void fz_##X(void) {                                                                \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            u64 a = val(), b = (i & 3) ? val() : a, fi = flags_in(), j, f, r, k = 0, *p = g_m; \
            g_m[1] = (i & 4) ? b : a;                                                         \
            __asm__ volatile("cmpb %b[b], %b[a]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a), [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("cmpw $0x7ff0, %w[a]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("testb %b[a], %b[a]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a) : "cc", "memory"); \
            h = mix(mix(h, j), f & LOGIC);                                                    \
            __asm__ volatile("testw $0x8001, %w[a]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a) : "cc", "memory"); \
            h = mix(mix(h, j), f & LOGIC);                                                    \
            __asm__ volatile("cmpl $0x7fffff00, %[m]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [m] "m"(g_m[1]) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("cmpq %[b], 8(%[p],%[k],8)" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [b] "r"(b), [p] "r"(p), [k] "r"(k) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("cmpq 8(%[p],%[k],8), %[a]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a), [p] "r"(p), [k] "r"(k) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("testb $0x81, %[m]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [m] "m"(g_m[1]) : "cc", "memory"); \
            h = mix(mix(h, j), f & LOGIC);                                                    \
            __asm__ volatile("testq %[a], %[m]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a), [m] "m"(g_m[1]) : "cc", "memory"); \
            h = mix(mix(h, j), f & LOGIC);                                                    \
            r = a; __asm__ volatile(PRE "subl $7, %k[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            r = a; __asm__ volatile(PRE "addq $-3, %[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            r = a; __asm__ volatile(PRE "andl $0xff00, %k[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & LOGIC);                                            \
            r = a; __asm__ volatile(PRE "orq $1, %[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & LOGIC);                                            \
            r = a; __asm__ volatile(PRE "xorl $0x55, %k[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & LOGIC);                                            \
            r = (i & 8) ? 1 : a; __asm__ volatile(PRE "decl %k[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            r = a; __asm__ volatile(PRE "incq %[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            __asm__ volatile("movq %[m], %[r]\n\ttestq %[r], %[r]" JX(X) : [r] "=&r"(r), [j] "=&r"(j), [f] "=&r"(f) : [m] "m"(g_m[(i & 8) ? 0 : 1]) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & LOGIC);                                            \
            /* the same pairs with nothing reading the flags afterwards */                    \
            r = (i & 8) ? 1 : a; __asm__ volatile("decl %k[r]" JN(X) : [r] "+r"(r), [j] "=&r"(j) : : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
            r = a; __asm__ volatile("subq $9, %[r]" JN(X) : [r] "+r"(r), [j] "=&r"(j) : : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
            __asm__ volatile("cmpl $0x7fffff00, %[m]" JN(X) : [j] "=&r"(j) : [m] "m"(g_m[1]) : "cc", "memory"); \
            h = mix(h, j);                                                                    \
            __asm__ volatile("cmpb %b[b], %b[a]" JN(X) : [j] "=&r"(j) : [a] "r"(a), [b] "r"(b) : "cc", "memory"); \
            h = mix(h, j);                                                                    \
            __asm__ volatile("movl %[m], %k[r]\n\ttestl %k[r], %k[r]" JN(X) : [r] "=&r"(r), [j] "=&r"(j) : [m] "m"(g_m[(i & 8) ? 0 : 1]) : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
            /* loop steps: add/sub, then cmp with a register (both orders) or an immediate */ \
            r = a; __asm__ volatile("addq $3, %[r]\n\tcmpq %[b], %[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            r = a; __asm__ volatile("subl $1, %k[r]\n\tcmpl %k[r], %k[b]" JN(X) : [r] "+r"(r), [j] "=&r"(j) : [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
            r = (i & 8) ? 0xff8 : a; __asm__ volatile("addq $8, %[r]\n\tcmpq $0x1000, %[r]" JN(X) : [r] "+r"(r), [j] "=&r"(j) : : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
            r = a; __asm__ volatile("addl %k[b], %k[r]\n\tcmpl $2000000, %k[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            /* ALU reg, reg + jcc; load + cmp imm + jcc */                                    \
            r = a; __asm__ volatile("subq %[b], %[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            r = a; __asm__ volatile("addl %k[b], %k[r]" JX(X) : [r] "+r"(r), [j] "=&r"(j), [f] "=&r"(f) : [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            r = a; __asm__ volatile("andl %k[b], %k[r]" JN(X) : [r] "+r"(r), [j] "=&r"(j) : [b] "r"(b) : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
            __asm__ volatile("movl %[m], %k[r]\n\tcmpl $0x7fff0000, %k[r]" JX(X) : [r] "=&r"(r), [j] "=&r"(j), [f] "=&r"(f) : [m] "m"(g_m[1]) : "cc", "memory"); \
            h = mix(mix(mix(h, r), j), f & ARITH);                                            \
            __asm__ volatile("movq %[m], %[r]\n\tcmpq $-5, %[r]" JN(X) : [r] "=&r"(r), [j] "=&r"(j) : [m] "m"(g_m[(i & 8) ? 0 : 1]) : "cc", "memory"); \
            h = mix(mix(h, r), j);                                                            \
        }                                                                                     \
        out("fz_" #X, h);                                                                     \
    }
FUSED_CC(o) FUSED_CC(no) FUSED_CC(b) FUSED_CC(ae) FUSED_CC(e) FUSED_CC(ne) FUSED_CC(be) FUSED_CC(a)
FUSED_CC(s) FUSED_CC(ns) FUSED_CC(p) FUSED_CC(np) FUSED_CC(l) FUSED_CC(ge) FUSED_CC(le) FUSED_CC(g)
// (u)comis + jcc: small exact values, equal ones, and quiet NaNs (unordered)
static double dval(int i) {
    union { u64 u; double d; } x;
    if (i % 7 == 0) { x.u = 0x7ff8000000000000ull; return x.d; }
    return (double)((long long)(rnd() % 41) - 20) / 4.0;
}
#define FUSED_COM(X)                                                                          \
    static void fc_##X(void) {                                                                \
        u64 h = 14695981039346656037ull;                                                      \
        for (int i = 0; i < N; i++) {                                                         \
            double d = dval(i), e = (i & 3) ? dval(i + 3) : d;                                \
            float fd = (float)d, fe = (float)e;                                               \
            u64 j, f;                                                                         \
            __asm__ volatile("comisd %[e], %[d]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [d] "x"(d), [e] "x"(e) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("ucomisd %[m], %[d]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [d] "x"(d), [m] "m"(e) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("comiss %[e], %[d]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [d] "x"(fd), [e] "x"(fe) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("ucomiss %[m], %[d]" JX(X) : [j] "=&r"(j), [f] "=&r"(f) : [d] "x"(fd), [m] "m"(fe) : "cc", "memory"); \
            h = mix(mix(h, j), f & ARITH);                                                    \
            __asm__ volatile("ucomisd %[e], %[d]" JN(X) : [j] "=&r"(j) : [d] "x"(d), [e] "x"(e) : "cc", "memory"); \
            h = mix(h, j);                                                                    \
        }                                                                                     \
        out("fc_" #X, h);                                                                     \
    }
FUSED_COM(o) FUSED_COM(no) FUSED_COM(b) FUSED_COM(ae) FUSED_COM(e) FUSED_COM(ne) FUSED_COM(be) FUSED_COM(a)
FUSED_COM(s) FUSED_COM(ns) FUSED_COM(p) FUSED_COM(np) FUSED_COM(l) FUSED_COM(ge) FUSED_COM(le) FUSED_COM(g)
// argument setup + call, push/pop pairs, pop + ret
static void fz_call(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        u64 a = val(), b = val(), r, s, t, z, c, d, e;
        __asm__ volatile("movq %[a], %%rdi\n\tcall 1f\n\tjmp 2f\n1:\n\tmovq %%rdi, %[r]\n\tret\n2:" : [r] "=&r"(r) : [a] "r"(a) : "rdi", "memory");
        __asm__ volatile("leaq 24(%[a]), %%rsi\n\tcall 1f\n\tjmp 2f\n1:\n\tmovq %%rsi, %[r]\n\tret\n2:" : [r] "=&r"(s) : [a] "r"(a) : "rsi", "memory");
        __asm__ volatile("movl $0x89abcdef, %%edx\n\tcall 1f\n\tjmp 2f\n1:\n\tmovq %%rdx, %[r]\n\tret\n2:" : [r] "=&r"(t) : : "rdx", "memory");
        __asm__ volatile("xorl %%ecx, %%ecx\n\tcall 1f\n\tjmp 2f\n1:\n\tmovq %%rcx, %[r]\n\tret\n2:" : [r] "=&r"(z) : : "rcx", "cc", "memory");
        __asm__ volatile("pushq %[a]\n\tpushq %[b]\n\tpopq %[c]\n\tpopq %[d]" : [c] "=&r"(c), [d] "=&r"(d) : [a] "r"(a), [b] "r"(b) : "memory");
        __asm__ volatile("call 1f\n\tjmp 2f\n1:\n\tpushq %[b]\n\tpopq %[e]\n\tret\n2:" : [e] "=&r"(e) : [b] "r"(b) : "memory");
        h = mix(mix(mix(mix(mix(mix(mix(h, r), s), t), z), c), d), e);
    }
    out("fz_call", h);
}
// mov D, S ; op D, imm (3-operand pairs), flags read after and not
static void fz_3op(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        u64 a = val(), fi = flags_in(), f, r;
        __asm__ volatile("movq %[a], %[r]\n\tshrq $12, %[r]" : [r] "=&r"(r) : [a] "r"(a) : "cc");
        h = mix(h, r);
        __asm__ volatile(PRE "movq %[a], %[r]\n\tshlq $25, %[r]" POST : [r] "=&r"(r), [f] "=&r"(f) : [a] "r"(a), [fi] "r"(fi) : "cc", "memory");
        h = mix(mix(h, r), f & 0xc5ull);
        __asm__ volatile(PRE "movl %k[a], %k[r]\n\tsarl $7, %k[r]" POST : [r] "=&r"(r), [f] "=&r"(f) : [a] "r"(a), [fi] "r"(fi) : "cc", "memory");
        h = mix(mix(h, r), f & 0xc5ull);
        __asm__ volatile("movq %[a], %[r]\n\trolq $13, %[r]" : [r] "=&r"(r) : [a] "r"(a) : "cc");
        h = mix(h, r);
        __asm__ volatile("movl %k[a], %k[r]\n\trorl $5, %k[r]" : [r] "=&r"(r) : [a] "r"(a) : "cc");
        h = mix(h, r);
        __asm__ volatile(PRE "movq %[a], %[r]\n\taddq $-77, %[r]" POST : [r] "=&r"(r), [f] "=&r"(f) : [a] "r"(a), [fi] "r"(fi) : "cc", "memory");
        h = mix(mix(h, r), f & ARITH);
        __asm__ volatile(PRE "movq %[a], %[r]\n\tandl $0xf0f0, %k[r]" POST : [r] "=&r"(r), [f] "=&r"(f) : [a] "r"(a), [fi] "r"(fi) : "cc", "memory");
        h = mix(mix(h, r), f & LOGIC);
        __asm__ volatile("movq %[a], %[r]\n\txorq $0x5a5a, %[r]" : [r] "=&r"(r) : [a] "r"(a) : "cc");
        h = mix(h, r);
    }
    out("fz_3op", h);
}
// mov D, S ; x ; op D, imm with an independent register-only x between (the pair fuses across x),
// x writing S or another register; and with the flags read after (no fusion then)
static void fz_3gap(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        u64 a = val(), b = val(), fi = flags_in(), f, r, s = b;
        __asm__ volatile("movq %[a], %[r]\n\taddq $1, %[s]\n\tshrq $12, %[r]" : [r] "=&r"(r), [s] "+r"(s) : [a] "r"(a) : "cc");
        h = mix(mix(h, r), s);
        s = a;
        __asm__ volatile("movq %[s], %[r]\n\trolq $13, %[s]\n\tshrq $7, %[r]" : [r] "=&r"(r), [s] "+r"(s) : : "cc");
        h = mix(mix(h, r), s);
        s = b;
        __asm__ volatile("movl %k[s], %k[r]\n\tnotq %[s]\n\txorl $0x3c3c, %k[r]" : [r] "=&r"(r), [s] "+r"(s) : : "cc");
        h = mix(mix(h, r), s);
        s = b;
        __asm__ volatile(PRE "movl %k[a], %k[r]\n\txorq %[a], %[s]\n\taddl $99, %k[r]" POST
                         : [r] "=&r"(r), [s] "+r"(s), [f] "=&r"(f) : [a] "r"(a), [fi] "r"(fi) : "cc", "memory");
        h = mix(mix(mix(h, r), s), f & ARITH);
    }
    out("fz_3gap", h);
}
// add/sub D, src ; x ; cmp D, ... ; jcc: FXR runs x first when the step is register-only (and fuses
// the step with the compare). With a memory source x must stay after it: build 118 (Crab Game) ran
// mov rbx, -1 before add rcx, [rbx] and read [-1]. x writes the base or the index here.
static void fz_hoist(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        u64 a = val(), fi = flags_in(), j, f, r, s, k = 1, *p = g_m;
        g_m[0] = val(); g_m[1] = val(); g_m[2] = val();
        u64 b = (i & 1) ? a + g_m[0] : val();
        r = a; p = g_m;
        __asm__ volatile("addq (%[p]), %[r]\n\tmovq $-1, %[p]\n\tcmpq %[b], %[r]" JN(e)
                         : [r] "+r"(r), [p] "+r"(p), [j] "=&r"(j) : [b] "r"(b) : "cc", "memory");
        h = mix(mix(mix(h, r), j), (u64)p);
        r = a; p = g_m;
        __asm__ volatile(PRE "addq (%[p]), %[r]\n\tmovq $-1, %[p]\n\tcmpq %[b], %[r]" JX(ne)
                         : [r] "+r"(r), [p] "+r"(p), [j] "=&r"(j), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc", "memory");
        h = mix(mix(mix(mix(h, r), j), (u64)p), f & ARITH);
        r = a; p = g_m; k = 1;
        __asm__ volatile("subl (%[p],%[k],8), %k[r]\n\txorl %k[k], %k[k]\n\tcmpl %k[b], %k[r]" JN(l)
                         : [r] "+r"(r), [k] "+r"(k), [j] "=&r"(j) : [p] "r"(p), [b] "r"(b) : "cc", "memory");
        h = mix(mix(mix(h, r), j), k);
        r = a; s = b;   /* a register-only step: x may run first */
        __asm__ volatile("addq $5, %[r]\n\tmovq %[a], %[s]\n\tcmpq %[b], %[r]" JX(l)
                         : [r] "+r"(r), [s] "+r"(s), [j] "=&r"(j), [f] "=&r"(f) : [a] "r"(a), [b] "r"(b) : "cc", "memory");
        h = mix(mix(mix(mix(h, r), s), j), f & ARITH);
    }
    out("fz_hoist", h);
}
// movaps D, S ; op D, X (3-operand SSE pairs), including X == D (the op reads the copy)
static void fz_x3(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vfloat(0), b = vfloat(0), d = vdouble(0), e = vdouble(1), r;
        __asm__ volatile("movaps %[a], %[r]\n\taddps %[b], %[r]" : [r] "=&x"(r) : [a] "x"(a), [b] "x"(b));
        h = vmix(h, r);
        __asm__ volatile("movaps %[a], %[r]\n\tmulps %[r], %[r]" : [r] "=&x"(r) : [a] "x"(a));
        h = vmix(h, r);
        __asm__ volatile("movapd %[d], %[r]\n\tsubsd %[e], %[r]" : [r] "=&x"(r) : [d] "x"(d), [e] "x"(e));
        h = vmix(h, r);
        __asm__ volatile("movapd %[d], %[r]\n\tdivsd %[e], %[r]" : [r] "=&x"(r) : [d] "x"(d), [e] "x"(e));
        h = vmix(h, r);
        __asm__ volatile("movaps %[a], %[r]\n\txorps %[b], %[r]" : [r] "=&x"(r) : [a] "x"(a), [b] "x"(b));
        h = vmix(h, r);
        __asm__ volatile("movdqa %[a], %[r]\n\tpandn %[b], %[r]" : [r] "=&x"(r) : [a] "x"(a), [b] "x"(b));
        h = vmix(h, r);
    }
    out("fz_x3", h);
}
// loads and stores with [base + index*scale + disp]
static void fz_index(void) {
    static u64 arr[48];
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int q = 0; q < 48; q++) arr[q] = val();
        u64 k = rnd() & 7, v = val(), r1, r2, r3, r4;
        __asm__ volatile("movq 8(%[p],%[k],8), %[r]" : [r] "=r"(r1) : [p] "r"(arr), [k] "r"(k) : "memory");
        __asm__ volatile("movl 4(%[p],%[k],4), %k[r]" : [r] "=r"(r2) : [p] "r"(arr), [k] "r"(k) : "memory");
        __asm__ volatile("movzbl 3(%[p],%[k]), %k[r]" : [r] "=r"(r3) : [p] "r"(arr), [k] "r"(k) : "memory");
        __asm__ volatile("movzbl 7(%[p]), %k[r]" : [r] "=r"(r4) : [p] "r"(arr) : "memory");
        __asm__ volatile("movb %b[v], 5(%[p],%[k],2)\n\tmovl %k[v], 32(%[p],%[k],4)\n\tmovq %[v], 64(%[p],%[k],1)"
                         : : [p] "r"(arr), [k] "r"(k), [v] "r"(v) : "memory");
        h = mix(mix(mix(mix(h, r1), r2), r3), r4);
        for (int q = 0; q < 48; q++) h = mix(h, arr[q]);
    }
    out("fz_index", h);
}
// 16-byte moves, SSE and ALU ops with [base + index] memory operands, without scale or
// displacement (the lean forms) and with them
static void fz_xindex(void) {
    static u64 arr[48] __attribute__((aligned(16)));
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int q = 0; q < 48; q++) arr[q] = val();
        u64 k = (rnd() & 7) * 16, v = val(), r = v;
        v2 a, b;
        __asm__ volatile("movdqu (%[p],%[k]), %[a]\n\tmovdqu 16(%[p],%[k]), %[b]\n\tpaddq %[b], %[a]\n\t"
                         "movdqu %[a], 128(%[p],%[k])\n\tmovdqu %[b], (%[p],%[k])"
                         : [a] "=&x"(a), [b] "=&x"(b) : [p] "r"(arr), [k] "r"(k) : "memory");
        h = vmix(vmix(h, a), b);
        __asm__ volatile("movdqa (%[p],%[k]), %[a]\n\tpaddd (%[p],%[k]), %[a]\n\tpsubq 16(%[p],%[k]), %[a]\n\tmovdqa %[a], 256(%[p])"
                         : [a] "=&x"(a) : [p] "r"(arr), [k] "r"(k) : "memory");
        h = vmix(h, a);
        __asm__ volatile("addq (%[p],%[k]), %[r]\n\txorq 8(%[p],%[k]), %[r]" : [r] "+r"(r) : [p] "r"(arr), [k] "r"(k) : "cc", "memory");
        h = mix(h, r);
        for (int q = 0; q < 48; q++) h = mix(h, arr[q]);
    }
    out("fz_xindex", h);
}

// Direct SSE memory arithmetic must preserve scalar upper lanes and handle all
// address scales and both pinned and memory-backed XMM register classes.
#define XM_CASE(OP, REG, ADDR, INDEX) do { \
    v2 result; \
    __asm__ volatile("movdqu %[input], %%" REG "\n\t" OP " " ADDR ", %%" REG \
                     "\n\tmovdqu %%" REG ", %[result]" \
        : [result] "=m"(result) \
        : [input] "m"(input), [p] "r"(data), [k] "r"((u64)(INDEX)) \
        : REG, "memory"); \
    h = vmix(h, result); \
} while (0)
#define XM_FORMS(OP) do { \
    XM_CASE(OP, "xmm0", "(%[p],%[k])", 16); \
    XM_CASE(OP, "xmm7", "16(%[p],%[k],2)", 8); \
    XM_CASE(OP, "xmm8", "-16(%[p],%[k],4)", 12); \
    XM_CASE(OP, "xmm15", "16(%[p],%[k],8)", 4); \
    XM_CASE(OP, "xmm15", "-16(%[p],%[k])", 48); \
    XM_CASE(OP, "xmm8", "-16(%[p],%[k],2)", 24); \
    XM_CASE(OP, "xmm7", "16(%[p],%[k],4)", 8); \
    XM_CASE(OP, "xmm0", "-16(%[p],%[k],8)", 6); \
    XM_CASE(OP, "xmm3", "16(%[p])", 0); \
} while (0)
static void gpta_xmem(void) {
    v2 data[8] __attribute__((aligned(16)));
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 input = vfloat(0);
        for (int j = 0; j < 8; j++) data[j] = vfloat(1);
        XM_FORMS("addps"); XM_FORMS("subps"); XM_FORMS("mulps");
        XM_FORMS("addss"); XM_FORMS("subss"); XM_FORMS("mulss");
        input = vdouble(0);
        for (int j = 0; j < 8; j++) data[j] = vdouble(1);
        XM_FORMS("addpd"); XM_FORMS("subpd"); XM_FORMS("mulpd");
        XM_FORMS("addsd"); XM_FORMS("subsd"); XM_FORMS("mulsd");
    }
    out("gpta_xmem", h);
}

// Force result-only conditions after different lazy-flag writers. A register
// move separates the writer and branch, exercising the unfused condition path.
#define ZSP_TEST(OP, CC) do { \
    u64 r = val(), b = val(), fi = flags_in(), j, f, tmp; \
    __asm__ volatile(PRE OP "\n\tmovq %[r], %[tmp]" JX(CC) \
        : [r] "+r"(r), [tmp] "=&r"(tmp), [j] "=&r"(j), [f] "=&r"(f) \
        : [b] "r"(b), [fi] "r"(fi) : "cc", "memory"); \
    h = mix(mix(h, j), tmp); \
} while (0)
#define ZSP_ALL(CC) \
    ZSP_TEST("addq %[b], %[r]", CC); ZSP_TEST("subq %[b], %[r]", CC); \
    ZSP_TEST("adcq %[b], %[r]", CC); ZSP_TEST("sbbq %[b], %[r]", CC); \
    ZSP_TEST("incq %[r]", CC); ZSP_TEST("decq %[r]", CC); \
    ZSP_TEST("negq %[r]", CC); ZSP_TEST("xorq %[b], %[r]", CC); \
    ZSP_TEST("shlq $3, %[r]", CC); ZSP_TEST("shrl $7, %k[r]", CC); \
    ZSP_TEST("sarw $2, %w[r]", CC); ZSP_TEST("orb %b[b], %b[r]", CC); \
    ZSP_TEST("", CC)
static void gpta_result_flags(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        ZSP_ALL(e); ZSP_ALL(ne); ZSP_ALL(s); ZSP_ALL(ns); ZSP_ALL(p); ZSP_ALL(np);
    }
    out("gpta_result_flags", h);
}

#define XHIGH_CASE(OP, D, S) do { \
    v2 result; \
    __asm__ volatile("movdqu %[a], %%" D "\n\tmovdqu %[b], %%" S \
        "\n\t" OP " %%" S ", %%" D "\n\tmovdqu %%" D ", %[r]" \
        : [r] "=m"(result) : [a] "m"(a), [b] "m"(b) : D, S, "memory"); \
    h = vmix(h, result); \
} while (0)
#define XHIGH_FORMS(OP) do { \
    XHIGH_CASE(OP, "xmm8", "xmm15"); \
    XHIGH_CASE(OP, "xmm0", "xmm14"); \
    XHIGH_CASE(OP, "xmm13", "xmm7"); \
    XHIGH_CASE(OP, "xmm9", "xmm12"); \
    XHIGH_CASE(OP, "xmm10", "xmm11"); \
} while (0)
static void gpta_xhigh(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vfloat(0), b = vfloat(1);
        XHIGH_FORMS("addps"); XHIGH_FORMS("subss"); XHIGH_FORMS("mulps");
        XHIGH_FORMS("movss"); XHIGH_FORMS("minps"); XHIGH_FORMS("maxss");
        a = vdouble(0); b = vdouble(1);
        XHIGH_FORMS("addpd"); XHIGH_FORMS("subsd"); XHIGH_FORMS("mulpd");
        XHIGH_FORMS("movsd"); XHIGH_FORMS("minpd"); XHIGH_FORMS("maxsd");
        a = vrand(); b = vrand();
        XHIGH_FORMS("paddd"); XHIGH_FORMS("punpckhqdq"); XHIGH_FORMS("pxor");
    }
    out("gpta_xhigh", h);
}

// Carry must survive INC/DEC if any later reader needs it, including across
// a non-carry condition. When carry is dead, the other result flags still count.
#define CF_FLOW_CASE(OP, CC, TAIL) do { \
    u64 r = val(), fi = flags_in(), extra = val(); unsigned char condition; \
    __asm__ volatile(PRE OP "\n\tset" #CC " %[condition]\n\t" TAIL \
        : [r] "+r"(r), [extra] "+r"(extra), [condition] "=&q"(condition) \
        : [fi] "r"(fi) : "cc", "memory"); \
    h = mix(mix(mix(h, r), condition), extra); \
} while (0)
#define CF_FLOW_CONDS(OP, TAIL) \
    CF_FLOW_CASE(OP, e, TAIL); CF_FLOW_CASE(OP, ne, TAIL); \
    CF_FLOW_CASE(OP, s, TAIL); CF_FLOW_CASE(OP, ns, TAIL); \
    CF_FLOW_CASE(OP, o, TAIL); CF_FLOW_CASE(OP, no, TAIL); \
    CF_FLOW_CASE(OP, p, TAIL); CF_FLOW_CASE(OP, np, TAIL); \
    CF_FLOW_CASE(OP, l, TAIL); CF_FLOW_CASE(OP, ge, TAIL); \
    CF_FLOW_CASE(OP, le, TAIL); CF_FLOW_CASE(OP, g, TAIL); \
    CF_FLOW_CASE(OP, b, TAIL); CF_FLOW_CASE(OP, ae, TAIL); \
    CF_FLOW_CASE(OP, be, TAIL); CF_FLOW_CASE(OP, a, TAIL)
#define CF_FLOW_OP(OP) \
    CF_FLOW_CONDS(OP, "addq $7, %[extra]"); \
    CF_FLOW_CONDS(OP, "adcq $7, %[extra]"); \
    CF_FLOW_CONDS(OP, "jnz 1f\n\taddq $7, %[extra]\n\tjmp 2f\n1:\taddq $5, %[extra]\n2:"); \
    CF_FLOW_CONDS(OP, "jnz 1f\n\tadcq $7, %[extra]\n\tjmp 2f\n1:\tadcq $5, %[extra]\n2:")
static void gpta_cf_flow(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        CF_FLOW_OP("incq %[r]"); CF_FLOW_OP("decq %[r]");
        CF_FLOW_OP("incl %k[r]"); CF_FLOW_OP("decl %k[r]");
    }
    out("gpta_cf_flow", h);
}

#define SCALE_MOVE(SCALE, REG) do { \
    __asm__ volatile("movdqu -7(%[p],%[k]," #SCALE "), %%" REG \
        "\n\tmovdqu %%" REG ", 137(%[p],%[k]," #SCALE ")" \
        : : [p] "r"(arr), [k] "r"((u64)17) : REG, "memory"); \
    for (int q = 0; q < 48; q++) h = mix(h, arr[q]); \
} while (0)
#define SCALE_MOVE_ZERO(SCALE, REG) do { \
    __asm__ volatile("movdqu (%[p],%[k]," #SCALE "), %%" REG \
        "\n\tmovdqu %%" REG ", (%[s],%[k]," #SCALE ")" \
        : : [p] "r"((char *)arr + 1), [s] "r"((char *)arr + 145), [k] "r"((u64)17) : REG, "memory"); \
    for (int q = 0; q < 48; q++) h = mix(h, arr[q]); \
} while (0)
static void gpta_scaled_moves(void) {
    static u64 arr[48] __attribute__((aligned(16)));
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int q = 0; q < 48; q++) arr[q] = val();
        SCALE_MOVE(1, "xmm0"); SCALE_MOVE(2, "xmm7");
        SCALE_MOVE(4, "xmm8"); SCALE_MOVE(8, "xmm15");
        SCALE_MOVE_ZERO(1, "xmm0"); SCALE_MOVE_ZERO(2, "xmm7");
        SCALE_MOVE_ZERO(4, "xmm8"); SCALE_MOVE_ZERO(8, "xmm15");
        SCALE_MOVE_ZERO(2, "xmm0"); SCALE_MOVE_ZERO(4, "xmm7");
        SCALE_MOVE_ZERO(8, "xmm8"); SCALE_MOVE_ZERO(2, "xmm15");
        SCALE_MOVE_ZERO(4, "xmm0"); SCALE_MOVE_ZERO(8, "xmm7");
        SCALE_MOVE_ZERO(2, "xmm8"); SCALE_MOVE_ZERO(4, "xmm15");
    }
    out("gpta_scaled_moves", h);
}

#define HIGH_COM_CASE(CC, D, S, OP, LOAD, A, B) do { \
    u64 j, f; \
    __asm__ volatile(LOAD " %[a], %%" D "\n\t" LOAD " %[b], %%" S \
        "\n\t" OP " %%" S ", %%" D JX(CC) \
        : [j] "=&r"(j), [f] "=&r"(f) : [a] "m"(A), [b] "m"(B) : D, S, "cc", "memory"); \
    h = mix(mix(h, j), f & ARITH); \
} while (0)
#define HIGH_COM_FORMS(CC) \
    HIGH_COM_CASE(CC, "xmm7", "xmm15", "ucomisd", "movsd", a, b); \
    HIGH_COM_CASE(CC, "xmm8", "xmm0", "comisd", "movsd", a, b); \
    HIGH_COM_CASE(CC, "xmm14", "xmm12", "ucomisd", "movsd", a, b); \
    HIGH_COM_CASE(CC, "xmm15", "xmm7", "ucomiss", "movss", af, bf); \
    HIGH_COM_CASE(CC, "xmm0", "xmm8", "comiss", "movss", af, bf); \
    HIGH_COM_CASE(CC, "xmm12", "xmm14", "ucomiss", "movss", af, bf)
static void gpta_high_comis(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        double a = dval(i), b = (i & 3) ? dval(i + 2) : a;
        float af = (float)a, bf = (float)b;
        HIGH_COM_FORMS(o); HIGH_COM_FORMS(no); HIGH_COM_FORMS(b); HIGH_COM_FORMS(ae);
        HIGH_COM_FORMS(e); HIGH_COM_FORMS(ne); HIGH_COM_FORMS(be); HIGH_COM_FORMS(a);
        HIGH_COM_FORMS(s); HIGH_COM_FORMS(ns); HIGH_COM_FORMS(p); HIGH_COM_FORMS(np);
        HIGH_COM_FORMS(l); HIGH_COM_FORMS(ge); HIGH_COM_FORMS(le); HIGH_COM_FORMS(g);
    }
    out("gpta_high_comis", h);
}

#define OP3R_CASE(MOVE, OP, TAIL, MASK) do { \
    u64 a = val(), b = val(), fi = flags_in(), r, f; \
    __asm__ volatile(PRE "movq %[a], %%rcx\n\tmovq %[b], %%rdx\n\t" MOVE \
        "\n\t" OP "\n\t" TAIL POST "\n\tmovq %%rax, %[r]" \
        : [r] "=&r"(r), [f] "=&r"(f) : [a] "r"(a), [b] "r"(b), [fi] "r"(fi) \
        : "rax", "rcx", "rdx", "r8", "cc", "memory"); \
    h = mix(mix(h, r), f & (MASK)); \
} while (0)
#define OP3R_FORMS(OP, TAIL, MASK) \
    OP3R_CASE("movq %%rcx, %%rax", OP "q %%rdx, %%rax", TAIL, MASK); \
    OP3R_CASE("movq %%rcx, %%rax", OP "q %%rax, %%rax", TAIL, MASK); \
    OP3R_CASE("movq %%rcx, %%rax", OP "q %%rcx, %%rax", TAIL, MASK); \
    OP3R_CASE("movl %%ecx, %%eax", OP "l %%edx, %%eax", TAIL, MASK); \
    OP3R_CASE("movl %%ecx, %%eax", OP "l %%eax, %%eax", TAIL, MASK); \
    OP3R_CASE("movq %%rcx, %%rax", OP "l %%edx, %%eax", TAIL, MASK); \
    OP3R_CASE("movl %%ecx, %%eax", OP "q %%rdx, %%rax", TAIL, MASK); \
    OP3R_CASE("movq %%rcx, %%rax\n\tincq %%rdx", OP "q %%rdx, %%rax", TAIL, MASK)
#define OP3R_OP(OP, MASK) \
    OP3R_FORMS(OP, "xor %%r8d, %%r8d\n\t", MASK); \
    OP3R_FORMS(OP, "", MASK)
static void gpta_op3r(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        OP3R_OP("add", ARITH); OP3R_OP("sub", ARITH); OP3R_OP("and", ARITH);
        OP3R_OP("or", ARITH); OP3R_OP("xor", ARITH); OP3R_OP("imul", 0x801);
    }
    out("gpta_op3r", h);
}

#define KNOWN_CF_CASE(SOURCE, UNARY) do { \
    u64 a = val(), b = val(), d = val(), fi = flags_in(), ra, rd, f; \
    __asm__ volatile(PRE "movq %[a], %%rax\n\tmovq %[b], %%rcx\n\tmovq %[d], %%rdx\n\t" \
        SOURCE "\n\tmovq %%rax, %%rsi\n\t" UNARY POST \
        "\n\tmovq %%rax, %[ra]\n\tmovq %%rdx, %[rd]" \
        : [ra] "=&r"(ra), [rd] "=&r"(rd), [f] "=&r"(f) \
        : [a] "r"(a), [b] "r"(b), [d] "r"(d), [fi] "r"(fi) \
        : "rax", "rcx", "rdx", "rsi", "cc", "memory"); \
    h = mix(mix(mix(h, ra), rd), f & ARITH); \
} while (0)
#define KNOWN_CF_TARGETS(SOURCE) \
    KNOWN_CF_CASE(SOURCE, "incl %%edx"); KNOWN_CF_CASE(SOURCE, "decl %%edx"); \
    KNOWN_CF_CASE(SOURCE, "incq %%rdx"); KNOWN_CF_CASE(SOURCE, "decq %%rdx")
#define KNOWN_CF_WIDTHS(OP) \
    KNOWN_CF_TARGETS(OP "b %%cl, %%al"); KNOWN_CF_TARGETS(OP "w %%cx, %%ax"); \
    KNOWN_CF_TARGETS(OP "l %%ecx, %%eax"); KNOWN_CF_TARGETS(OP "q %%rcx, %%rax")
static void gpta_known_carry(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        KNOWN_CF_WIDTHS("add"); KNOWN_CF_WIDTHS("adc");
        KNOWN_CF_WIDTHS("sub"); KNOWN_CF_WIDTHS("sbb"); KNOWN_CF_WIDTHS("xor");
        KNOWN_CF_TARGETS("negb %%al"); KNOWN_CF_TARGETS("negw %%ax");
        KNOWN_CF_TARGETS("negl %%eax"); KNOWN_CF_TARGETS("negq %%rax");
        KNOWN_CF_TARGETS("incq %%rax"); KNOWN_CF_TARGETS("decl %%eax");
        KNOWN_CF_TARGETS("imull %%ecx, %%eax"); KNOWN_CF_TARGETS("imulq %%rcx, %%rax");
        KNOWN_CF_TARGETS("addq %%rcx, %%rax\n\tshlq $0, %%rax");
        KNOWN_CF_TARGETS("addq %%rcx, %%rax\n\tmovl $1, %%ecx\n\tshlq %%cl, %%rax");
        KNOWN_CF_TARGETS("addq %%rcx, %%rax\n\tstc");
    }
    out("gpta_known_carry", h);
}

#define NARROW_CASE(CODE, MASK) do { \
    u64 a = val(), b = val(), c = val(), d = val(), fi = flags_in(), f; \
    __asm__ volatile(PRE CODE POST \
        : "+a"(a), "+b"(b), "+c"(c), "+d"(d), [f] "=&r"(f) \
        : [fi] "r"(fi) : "r8", "r9", "cc", "memory"); \
    h = mix(mix(mix(mix(mix(h, a), b), c), d), f & (MASK)); \
} while (0)
#define NARROW_OP(OP, MASK) \
    NARROW_CASE(OP "b %%ah, %%al", MASK); NARROW_CASE(OP "b %%al, %%ah", MASK); \
    NARROW_CASE(OP "b %%ch, %%dh", MASK); NARROW_CASE(OP "b %%bl, %%bh", MASK); \
    NARROW_CASE(OP "b $0xa5, %%ah", MASK); NARROW_CASE(OP "b $0x80, %%cl", MASK); \
    NARROW_CASE(OP "w %%ax, %%dx", MASK); NARROW_CASE(OP "w $0x8001, %%bx", MASK); \
    NARROW_CASE("movq %%rax, %%r8\n\tmovq %%rcx, %%r9\n\t" OP "b %%r8b, %%r9b\n\tmovq %%r9, %%rcx", MASK); \
    NARROW_CASE("movq %%rax, %%r8\n\tmovq %%rcx, %%r9\n\t" OP "w %%r9w, %%r8w\n\tmovq %%r8, %%rax", MASK); \
    NARROW_CASE(OP "b %%ch, %%ah\n\txorl %%edx, %%edx", LOGIC)
static void gpta_narrow(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        NARROW_OP("add", ARITH); NARROW_OP("adc", ARITH);
        NARROW_OP("sub", ARITH); NARROW_OP("sbb", ARITH);
        NARROW_OP("and", LOGIC); NARROW_OP("or", LOGIC); NARROW_OP("xor", LOGIC);
        NARROW_OP("cmp", ARITH); NARROW_OP("test", LOGIC);
        NARROW_OP("mov", ARITH);
    }
    out("gpta_narrow", h);
}

#define FCJ_TRACE_CASE(CC, OP, LOAD, A, B) do { \
    u64 r; \
    __asm__ volatile(LOAD " %[a], %%xmm7\n\t" LOAD " %[b], %%xmm14\n\t" \
        "movl $11, %%ecx\n\txorl %%eax, %%eax\n1:\t" OP " %%xmm14, %%xmm7\n\tj" #CC " 2f\n\t" \
        "addq $3, %%rax\n\tjmp 3f\n2:\taddq $5, %%rax\n3:\tsubl $1, %%ecx\n\tjnz 1b" \
        : "=&a"(r) : [a] "m"(A), [b] "m"(B) : "rcx", "xmm7", "xmm14", "cc", "memory"); \
    h = mix(h, r); \
} while (0)
#define FCJ_TRACE_FORMS(CC) \
    FCJ_TRACE_CASE(CC, "ucomisd", "movsd", a, b); FCJ_TRACE_CASE(CC, "ucomiss", "movss", af, bf)
static void gpta_fcj_trace(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        double a = dval(i), b = (i & 3) ? dval(i + 2) : a;
        float af = (float)a, bf = (float)b;
        FCJ_TRACE_FORMS(o); FCJ_TRACE_FORMS(no); FCJ_TRACE_FORMS(b); FCJ_TRACE_FORMS(ae);
        FCJ_TRACE_FORMS(e); FCJ_TRACE_FORMS(ne); FCJ_TRACE_FORMS(be); FCJ_TRACE_FORMS(a);
        FCJ_TRACE_FORMS(s); FCJ_TRACE_FORMS(ns); FCJ_TRACE_FORMS(p); FCJ_TRACE_FORMS(np);
        FCJ_TRACE_FORMS(l); FCJ_TRACE_FORMS(ge); FCJ_TRACE_FORMS(le); FCJ_TRACE_FORMS(g);
    }
    double one = 1.0, limit = 11.0, result;
    __asm__ volatile("pxor %%xmm14, %%xmm14\n\tmovsd %[one], %%xmm7\n\tmovsd %[lim], %%xmm8\n"
        "1:\taddsd %%xmm7, %%xmm14\n\tucomisd %%xmm8, %%xmm14\n\tjb 1b\n\tmovsd %%xmm14, %[out]\n\txorl %%eax, %%eax"
        : [out] "=m"(result) : [one] "m"(one), [lim] "m"(limit) : "rax", "xmm7", "xmm8", "xmm14", "cc", "memory");
    h = mix(h, (u64)result);
    out("gpta_fcj_trace", h);
}

// Adjacent trace edges for add/sub + immediate compare. The outer counter is
// independent, so every condition terminates even for wraparound and aliases.
#define STEP_TRACE_CASE(CC, OP, SUF, DST, SRC, LIMIT) do { \
    u64 r = (i & 1) ? val() : (u64)i, s = (i & 1) ? val() : 3, score; \
    __asm__ volatile("movl $9, %%ecx\n\txorl %%ebx, %%ebx\n1:\t" \
        OP SUF " " SRC ", %%" DST "\n\tcmp" SUF " $" LIMIT ", %%" DST "\n\tj" #CC " 2f\n\t" \
        "addq $3, %%rbx\n\tjmp 3f\n2:\taddq $5, %%rbx\n3:\tdecl %%ecx\n\tjnz 1b" \
        : "+a"(r), "=&b"(score) : "d"(s) : "rcx", "cc", "memory"); \
    h = mix(mix(h, r), score); \
} while (0)
#define STEP_TRACE_FORMS(CC, OP) \
    STEP_TRACE_CASE(CC, OP, "q", "rax", "$-7", "13"); \
    STEP_TRACE_CASE(CC, OP, "l", "eax", "$7", "-13"); \
    STEP_TRACE_CASE(CC, OP, "q", "rax", "%%rdx", "13"); \
    STEP_TRACE_CASE(CC, OP, "l", "eax", "%%edx", "-13"); \
    STEP_TRACE_CASE(CC, OP, "q", "rax", "%%rax", "-13"); \
    STEP_TRACE_CASE(CC, OP, "l", "eax", "%%eax", "13")
#define STEP_TRACE_COND(CC) STEP_TRACE_FORMS(CC, "add"); STEP_TRACE_FORMS(CC, "sub")
#define STEP_BACKEDGE(OP, SUF, DST, SRC, START, LIMIT, CC) do { \
    u64 r = START, f; \
    __asm__ volatile("1:\t" OP SUF " " SRC ", %%" DST "\n\tcmp" SUF \
        " $" LIMIT ", %%" DST "\n\tj" #CC " 1b\n\tpushfq\n\tpopq %[f]\n\ttestq %%rax, %%rax" \
        : "+a"(r), [f] "=&r"(f) : "d"((u64)3) : "cc", "memory"); \
    h = mix(mix(h, r),f & ARITH); \
} while (0)
static void gpta_step_trace(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        STEP_TRACE_COND(b); STEP_TRACE_COND(ae); STEP_TRACE_COND(e); STEP_TRACE_COND(ne);
        STEP_TRACE_COND(be); STEP_TRACE_COND(a); STEP_TRACE_COND(l); STEP_TRACE_COND(ge);
        STEP_TRACE_COND(le); STEP_TRACE_COND(g);
        // All handlers in these one-block traces have replicas. The last copy
        // must retain its linked backedge instead of stepping past the block.
        STEP_BACKEDGE("add", "q", "rax", "$3", 0, "19", b);
        STEP_BACKEDGE("sub", "l", "eax", "$3", 30, "13", ae);
        STEP_BACKEDGE("add", "q", "rax", "%%rdx", 0, "18", be);
        STEP_BACKEDGE("sub", "l", "eax", "%%edx", 30, "12", a);
        STEP_BACKEDGE("add", "l", "eax", "$1", 0, "17", ne);
        STEP_BACKEDGE("add", "q", "rax", "$1", ~0ull, "0", e);
        STEP_BACKEDGE("add", "q", "rax", "%%rdx", (u64)-20, "7", l);
        STEP_BACKEDGE("sub", "l", "eax", "%%edx", 20, "-7", ge);
        STEP_BACKEDGE("add", "l", "eax", "$3", (u64)-20, "7", le);
        STEP_BACKEDGE("sub", "q", "rax", "$3", 20, "-7", g);
    }
    out("gpta_step_trace", h);
}

#define JRESULT_TRACE_CASE(CC, OP) do { \
    u64 a = val(), r; \
    __asm__ volatile("movl $13, %%ecx\n\txorl %%edx, %%edx\n1:\t" OP "\n\tj" #CC " 2f\n\t" \
        "addq $3, %%rdx\n\tjmp 3f\n2:\taddq $5, %%rdx\n3:\tsubl $1, %%ecx\n\tjnz 1b" \
        : "+a"(a), "=&d"(r) : : "rcx", "cc", "memory"); \
    h = mix(mix(h, a), r); \
} while (0)
#define JRESULT_TRACE_FORMS(CC) \
    JRESULT_TRACE_CASE(CC, "negb %%al"); JRESULT_TRACE_CASE(CC, "negw %%ax"); \
    JRESULT_TRACE_CASE(CC, "negl %%eax"); JRESULT_TRACE_CASE(CC, "negq %%rax")
static void gpta_jresult_trace(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        JRESULT_TRACE_FORMS(e); JRESULT_TRACE_FORMS(ne); JRESULT_TRACE_FORMS(s);
        JRESULT_TRACE_FORMS(ns); JRESULT_TRACE_FORMS(p); JRESULT_TRACE_FORMS(np);
    }
    out("gpta_jresult_trace", h);
}

#define IMMSTORE_SCALE(SCALE) do { \
    u64 fi = flags_in(), f; \
    __asm__ volatile(PRE "movb $0xa5, -3(%[p],%[ix]," #SCALE ")\n\t" \
        "movw $0x8153, 17(%[p],%[ix]," #SCALE ")\n\t" \
        "movl $0x89abcdef, 39(%[p],%[ix]," #SCALE ")\n\t" \
        "movq $-12345, 67(%[p],%[ix]," #SCALE ")" POST \
        : [f] "=&r"(f) : [p] "r"(arr), [ix] "r"((u64)7), [fi] "r"(fi) : "cc", "memory"); \
    for (int j = 0; j < 32; j++) h = mix(h, arr[j]); \
    h = mix(h, f & ARITH); \
} while (0)
#define IMMSTORE_ZERO_CASE(OP, ADDR, IX) do { \
    u64 fi = flags_in(), f; \
    __asm__ volatile(PRE OP " " ADDR POST \
        : [f] "=&r"(f) : [p] "r"(arr + 8), [ix] "r"((u64)(IX)), [fi] "r"(fi) : "cc", "memory"); \
    for (int j = 0; j < 32; j++) h = mix(h, arr[j]); \
    h = mix(h, f & ARITH); \
} while (0)
#define IMMSTORE_ZERO_SCALE(SC, IX) \
    IMMSTORE_ZERO_CASE("movb $0xa5,", "(%[p],%[ix]," #SC ")", IX); \
    IMMSTORE_ZERO_CASE("movw $0x8153,", "(%[p],%[ix]," #SC ")", IX); \
    IMMSTORE_ZERO_CASE("movl $0x89abcdef,", "(%[p],%[ix]," #SC ")", IX); \
    IMMSTORE_ZERO_CASE("movq $-12345,", "(%[p],%[ix]," #SC ")", IX)
static void gpta_immstores(void) {
    u64 h = 14695981039346656037ull, arr[32];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 32; j++) arr[j] = val();
        IMMSTORE_SCALE(1); IMMSTORE_SCALE(2); IMMSTORE_SCALE(4); IMMSTORE_SCALE(8);
        IMMSTORE_ZERO_SCALE(1, 7); IMMSTORE_ZERO_SCALE(2, -7);
        IMMSTORE_ZERO_SCALE(4, 7); IMMSTORE_ZERO_SCALE(8, -7);
        IMMSTORE_ZERO_CASE("movb $0,", "(%[p])", 0);
        IMMSTORE_ZERO_CASE("movw $-1,", "(%[p])", 0);
        IMMSTORE_ZERO_CASE("movl $0,", "(%[p])", 0);
        IMMSTORE_ZERO_CASE("movq $-1,", "(%[p])", 0);
    }
    out("gpta_immstores", h);
}

#define X3HIGH_CASE(OP, D, S, X) do { \
    v2 r; \
    __asm__ volatile("movdqu %[a], %%" S "\n\tmovdqu %[b], %%" X "\n\t" \
        "movdqa %%" S ", %%" D "\n\t" OP " %%" X ", %%" D "\n\tmovdqu %%" D ", %[r]" \
        : [r] "=m"(r) : [a] "m"(a), [b] "m"(b) \
        : "xmm0", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory"); \
    h = vmix(h, r); \
} while (0)
#define X3HIGH_FORMS(OP) \
    X3HIGH_CASE(OP, "xmm0", "xmm8", "xmm15"); X3HIGH_CASE(OP, "xmm15", "xmm8", "xmm9"); \
    X3HIGH_CASE(OP, "xmm8", "xmm14", "xmm7"); X3HIGH_CASE(OP, "xmm7", "xmm15", "xmm8"); \
    X3HIGH_CASE(OP, "xmm13", "xmm13", "xmm12"); X3HIGH_CASE(OP, "xmm14", "xmm10", "xmm14"); \
    X3HIGH_CASE(OP, "xmm9", "xmm11", "xmm11"); \
    X3HIGH_CASE(OP, "xmm10", "xmm8", "xmm15"); X3HIGH_CASE(OP, "xmm11", "xmm7", "xmm0"); \
    X3HIGH_CASE(OP, "xmm12", "xmm14", "xmm9")
static void gpta_x3high(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vfloat(1), b = vfloat(1);
        X3HIGH_FORMS("addps"); X3HIGH_FORMS("subps"); X3HIGH_FORMS("mulps"); X3HIGH_FORMS("divps");
        X3HIGH_FORMS("addss"); X3HIGH_FORMS("subss"); X3HIGH_FORMS("mulss"); X3HIGH_FORMS("divss");
        a = vdouble(1); b = vdouble(1);
        X3HIGH_FORMS("addpd"); X3HIGH_FORMS("subpd"); X3HIGH_FORMS("mulpd"); X3HIGH_FORMS("divpd");
        X3HIGH_FORMS("addsd"); X3HIGH_FORMS("subsd"); X3HIGH_FORMS("mulsd"); X3HIGH_FORMS("divsd");
        a = vrand(); b = vrand();
        X3HIGH_FORMS("pand"); X3HIGH_FORMS("pandn"); X3HIGH_FORMS("por"); X3HIGH_FORMS("pxor");
    }
    out("gpta_x3high", h);
}

// Native x86 is the oracle for every immediate count and the masked boundaries.
// TEST on each side prevents a preceding copy from hiding the single shift,
// and makes its output flags dead without assuming anything about their values.
#define SC_TEST(COUNT, OP, REG) do { \
    u64 r = val(); \
    __asm__ volatile("testq %[r], %[r]\n\t" OP " $" #COUNT ", " REG \
        "\n\ttestq %[r], %[r]" : [r] "+r"(r) : : "cc"); \
    h = mix(h, r); \
} while (0)
#define SC_TEST_COUNT(COUNT) \
    SC_TEST(COUNT, "shll", "%k[r]"); SC_TEST(COUNT, "shlq", "%[r]"); \
    SC_TEST(COUNT, "shrl", "%k[r]"); SC_TEST(COUNT, "shrq", "%[r]"); \
    SC_TEST(COUNT, "sarl", "%k[r]"); SC_TEST(COUNT, "sarq", "%[r]"); \
    SC_TEST(COUNT, "roll", "%k[r]"); SC_TEST(COUNT, "rolq", "%[r]"); \
    SC_TEST(COUNT, "rorl", "%k[r]"); SC_TEST(COUNT, "rorq", "%[r]")
// Copy plus immediate shift/rotate: every count, both result widths and
// both legal copy widths. Native execution supplies the differential oracle.
#define CSH_TEST(COUNT, OP, MOVE, SRC, DST, SHIFT_DST) do { \
    u64 a = val(), r; \
    __asm__ volatile("testq %[a], %[a]\n\t" MOVE " " SRC ", " DST \
        "\n\t" OP " $" #COUNT ", " SHIFT_DST "\n\ttestq %[r], %[r]" \
        : [r] "=&r"(r) : [a] "r"(a) : "cc"); \
    h = mix(mix(h, a), r); \
} while (0)
#define CSH_TEST_OP(COUNT, OP) \
    CSH_TEST(COUNT, OP "l", "movl", "%k[a]", "%k[r]", "%k[r]"); \
    CSH_TEST(COUNT, OP "l", "movq", "%[a]", "%[r]", "%k[r]"); \
    CSH_TEST(COUNT, OP "q", "movq", "%[a]", "%[r]", "%[r]")
#define CSH_TEST_COUNT(COUNT) \
    CSH_TEST_OP(COUNT, "shl"); CSH_TEST_OP(COUNT, "shr"); CSH_TEST_OP(COUNT, "sar"); \
    CSH_TEST_OP(COUNT, "rol"); CSH_TEST_OP(COUNT, "ror")
static void gpta_copy_shift_counts(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        CSH_TEST_COUNT(0); CSH_TEST_COUNT(1); CSH_TEST_COUNT(2); CSH_TEST_COUNT(3); CSH_TEST_COUNT(4); CSH_TEST_COUNT(5); CSH_TEST_COUNT(6); CSH_TEST_COUNT(7);
        CSH_TEST_COUNT(8); CSH_TEST_COUNT(9); CSH_TEST_COUNT(10); CSH_TEST_COUNT(11); CSH_TEST_COUNT(12); CSH_TEST_COUNT(13); CSH_TEST_COUNT(14); CSH_TEST_COUNT(15);
        CSH_TEST_COUNT(16); CSH_TEST_COUNT(17); CSH_TEST_COUNT(18); CSH_TEST_COUNT(19); CSH_TEST_COUNT(20); CSH_TEST_COUNT(21); CSH_TEST_COUNT(22); CSH_TEST_COUNT(23);
        CSH_TEST_COUNT(24); CSH_TEST_COUNT(25); CSH_TEST_COUNT(26); CSH_TEST_COUNT(27); CSH_TEST_COUNT(28); CSH_TEST_COUNT(29); CSH_TEST_COUNT(30); CSH_TEST_COUNT(31);
        CSH_TEST_COUNT(32); CSH_TEST_COUNT(33); CSH_TEST_COUNT(34); CSH_TEST_COUNT(35); CSH_TEST_COUNT(36); CSH_TEST_COUNT(37); CSH_TEST_COUNT(38); CSH_TEST_COUNT(39);
        CSH_TEST_COUNT(40); CSH_TEST_COUNT(41); CSH_TEST_COUNT(42); CSH_TEST_COUNT(43); CSH_TEST_COUNT(44); CSH_TEST_COUNT(45); CSH_TEST_COUNT(46); CSH_TEST_COUNT(47);
        CSH_TEST_COUNT(48); CSH_TEST_COUNT(49); CSH_TEST_COUNT(50); CSH_TEST_COUNT(51); CSH_TEST_COUNT(52); CSH_TEST_COUNT(53); CSH_TEST_COUNT(54); CSH_TEST_COUNT(55);
        CSH_TEST_COUNT(56); CSH_TEST_COUNT(57); CSH_TEST_COUNT(58); CSH_TEST_COUNT(59); CSH_TEST_COUNT(60); CSH_TEST_COUNT(61); CSH_TEST_COUNT(62); CSH_TEST_COUNT(63);
        CSH_TEST_COUNT(64); CSH_TEST_COUNT(127); CSH_TEST_COUNT(255);
    }
    out("gpta_copy_shift_counts", h);
}

static void gpta_shift_counts(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        SC_TEST_COUNT(0); SC_TEST_COUNT(1); SC_TEST_COUNT(2); SC_TEST_COUNT(3); SC_TEST_COUNT(4); SC_TEST_COUNT(5); SC_TEST_COUNT(6); SC_TEST_COUNT(7);
        SC_TEST_COUNT(8); SC_TEST_COUNT(9); SC_TEST_COUNT(10); SC_TEST_COUNT(11); SC_TEST_COUNT(12); SC_TEST_COUNT(13); SC_TEST_COUNT(14); SC_TEST_COUNT(15);
        SC_TEST_COUNT(16); SC_TEST_COUNT(17); SC_TEST_COUNT(18); SC_TEST_COUNT(19); SC_TEST_COUNT(20); SC_TEST_COUNT(21); SC_TEST_COUNT(22); SC_TEST_COUNT(23);
        SC_TEST_COUNT(24); SC_TEST_COUNT(25); SC_TEST_COUNT(26); SC_TEST_COUNT(27); SC_TEST_COUNT(28); SC_TEST_COUNT(29); SC_TEST_COUNT(30); SC_TEST_COUNT(31);
        SC_TEST_COUNT(32); SC_TEST_COUNT(33); SC_TEST_COUNT(34); SC_TEST_COUNT(35); SC_TEST_COUNT(36); SC_TEST_COUNT(37); SC_TEST_COUNT(38); SC_TEST_COUNT(39);
        SC_TEST_COUNT(40); SC_TEST_COUNT(41); SC_TEST_COUNT(42); SC_TEST_COUNT(43); SC_TEST_COUNT(44); SC_TEST_COUNT(45); SC_TEST_COUNT(46); SC_TEST_COUNT(47);
        SC_TEST_COUNT(48); SC_TEST_COUNT(49); SC_TEST_COUNT(50); SC_TEST_COUNT(51); SC_TEST_COUNT(52); SC_TEST_COUNT(53); SC_TEST_COUNT(54); SC_TEST_COUNT(55);
        SC_TEST_COUNT(56); SC_TEST_COUNT(57); SC_TEST_COUNT(58); SC_TEST_COUNT(59); SC_TEST_COUNT(60); SC_TEST_COUNT(61); SC_TEST_COUNT(62); SC_TEST_COUNT(63);
        SC_TEST_COUNT(64); SC_TEST_COUNT(127); SC_TEST_COUNT(255);
    }
    out("gpta_shift_counts", h);
}

// Only one successor reads flags. INC/DEC must preserve the preceding CF even
// when its other output flags are reconstructed after the conditional branch.
#define UNARY_EDGE_CASE(SOURCE, UNARY, CC, EDGE) do { \
    u64 a = val(), b = val(), r = unary_values[i % 10], fi = flags_in(), f; \
    __asm__ volatile(PRE SOURCE "\n\t" UNARY "\n\tj" CC " 1f\n\t" EDGE \
        : [a] "+&r"(a), [r] "+&r"(r), [f] "=&r"(f) \
        : [b] "r"(b), [fi] "r"(fi) : "cc", "memory"); \
    h = mix(mix(mix(h, a), r), f & ARITH); \
} while (0)
#define UNARY_EDGE_FORMS(SOURCE, UNARY, CC) \
    UNARY_EDGE_CASE(SOURCE, UNARY, CC, "pushfq\n\tpopq %[f]\n\tjmp 2f\n\t1: xorq %[f], %[f]\n\t2:"); \
    UNARY_EDGE_CASE(SOURCE, UNARY, CC, "xorq %[f], %[f]\n\tjmp 2f\n\t1: pushfq\n\tpopq %[f]\n\t2:")
#define UNARY_EDGE_CCS(SOURCE, UNARY) \
    UNARY_EDGE_FORMS(SOURCE, UNARY, "e"); UNARY_EDGE_FORMS(SOURCE, UNARY, "ne"); \
    UNARY_EDGE_FORMS(SOURCE, UNARY, "s"); UNARY_EDGE_FORMS(SOURCE, UNARY, "ns")
#define UNARY_EDGE_TARGETS(SOURCE) \
    UNARY_EDGE_CCS(SOURCE, "incl %k[r]"); UNARY_EDGE_CCS(SOURCE, "decl %k[r]"); \
    UNARY_EDGE_CCS(SOURCE, "incq %[r]"); UNARY_EDGE_CCS(SOURCE, "decq %[r]")
#define UNARY_EDGE_WIDTHS(OP) \
    UNARY_EDGE_TARGETS(OP "b %b[b], %b[a]"); UNARY_EDGE_TARGETS(OP "w %w[b], %w[a]"); \
    UNARY_EDGE_TARGETS(OP "l %k[b], %k[a]"); UNARY_EDGE_TARGETS(OP "q %[b], %[a]")
static void gpta_unary_edges(void) {
    static const u64 unary_values[] = { 0, 1, ~0ull, 0x7fffffff, 0x80000000,
        0xffffffff, 0x100000000, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x6c10f87e23a1bd45ull };
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < 40; i++) {
        UNARY_EDGE_WIDTHS("add"); UNARY_EDGE_WIDTHS("adc");
        UNARY_EDGE_WIDTHS("sub"); UNARY_EDGE_WIDTHS("sbb"); UNARY_EDGE_WIDTHS("xor");
        UNARY_EDGE_TARGETS("");
        UNARY_EDGE_TARGETS("negb %b[a]"); UNARY_EDGE_TARGETS("negw %w[a]");
        UNARY_EDGE_TARGETS("negl %k[a]"); UNARY_EDGE_TARGETS("negq %[a]");
        UNARY_EDGE_TARGETS("incq %[a]"); UNARY_EDGE_TARGETS("decl %k[a]");
        UNARY_EDGE_TARGETS("imulq %[b], %[a]"); UNARY_EDGE_TARGETS("imull %k[b], %k[a]");
        UNARY_EDGE_TARGETS("shlb $1, %b[a]"); UNARY_EDGE_TARGETS("shlw $8, %w[a]");
        UNARY_EDGE_TARGETS("shll $31, %k[a]"); UNARY_EDGE_TARGETS("shlq $63, %[a]");
        UNARY_EDGE_TARGETS("shrb $7, %b[a]"); UNARY_EDGE_TARGETS("shrw $1, %w[a]");
        UNARY_EDGE_TARGETS("shrl $31, %k[a]"); UNARY_EDGE_TARGETS("shrq $63, %[a]");
        UNARY_EDGE_TARGETS("sarb $7, %b[a]"); UNARY_EDGE_TARGETS("sarw $1, %w[a]");
        UNARY_EDGE_TARGETS("sarl $31, %k[a]"); UNARY_EDGE_TARGETS("sarq $63, %[a]");
    }
    out("gpta_unary_edges", h);
}

#define AMEM_CASE(OP, SZ, REG, ADDR, TAIL, MASK, INIT) do { \
    u64 r = (INIT), fi = flags_in(), f; \
    __asm__ volatile(PRE OP SZ " " ADDR ", " REG "\n\t" TAIL POST \
        : [r] "+&r"(r), [f] "=&r"(f) \
        : [p] "r"((char *)data + 32), [k] "r"((u64)3), [fi] "r"(fi) : "cc", "memory"); \
    h = mix(mix(h, r), f & (MASK)); \
} while (0)
#define AMEM_ADDR(OP, ADDR, MASK, INIT) \
    AMEM_CASE(OP,"l","%k[r]",ADDR,"",MASK,INIT); \
    AMEM_CASE(OP,"q","%[r]",ADDR,"",MASK,INIT); \
    AMEM_CASE(OP,"l","%k[r]",ADDR,"xorl %k[f], %k[f]\n\t",LOGIC,INIT); \
    AMEM_CASE(OP,"q","%[r]",ADDR,"xorl %k[f], %k[f]\n\t",LOGIC,INIT)
#define AMEM_OP(OP,MASK) \
    AMEM_ADDR(OP,"-7(%[p],%[k],1)",MASK,val()); \
    AMEM_ADDR(OP,"9(%[p],%[k],2)",MASK,val()); \
    AMEM_ADDR(OP,"(%[p],%[k],4)",MASK,val()); \
    AMEM_ADDR(OP,"-7(%[p],%[k],8)",MASK,val()); \
    AMEM_ADDR(OP,"3(%[p])",MASK,val()); \
    AMEM_ADDR(OP,"9(%[p],%[r],2)",MASK,3); \
    AMEM_ADDR(OP,"(%[p],%[r],8)",MASK,3)
static void gpta_alu_memory(void) {
    u64 data[32], h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 32; j++) data[j] = val();
        AMEM_OP("add",ARITH); AMEM_OP("sub",ARITH);
        AMEM_OP("and",LOGIC); AMEM_OP("or",LOGIC); AMEM_OP("xor",LOGIC);
    }
    out("gpta_alu_memory", h);
}

// Preserve every XMM lane across mixed scalar/vector operations and an x87
// fallback. All architectural registers remain live together across the test.
static void gpta_xmm_lane_state(void) {
    v2 regs[16];
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 16; j++) regs[j] = vdouble(1);
        __asm__ volatile(
            "movdqu 0(%[p]), %%xmm0\n\t"
            "movdqu 16(%[p]), %%xmm1\n\t"
            "movdqu 32(%[p]), %%xmm2\n\t"
            "movdqu 48(%[p]), %%xmm3\n\t"
            "movdqu 64(%[p]), %%xmm4\n\t"
            "movdqu 80(%[p]), %%xmm5\n\t"
            "movdqu 96(%[p]), %%xmm6\n\t"
            "movdqu 112(%[p]), %%xmm7\n\t"
            "movdqu 128(%[p]), %%xmm8\n\t"
            "movdqu 144(%[p]), %%xmm9\n\t"
            "movdqu 160(%[p]), %%xmm10\n\t"
            "movdqu 176(%[p]), %%xmm11\n\t"
            "movdqu 192(%[p]), %%xmm12\n\t"
            "movdqu 208(%[p]), %%xmm13\n\t"
            "movdqu 224(%[p]), %%xmm14\n\t"
            "movdqu 240(%[p]), %%xmm15\n\t"
            "addsd %%xmm8, %%xmm0\n\taddsd %%xmm0, %%xmm8\n\t"
            "subsd %%xmm9, %%xmm1\n\tmulsd %%xmm10, %%xmm2\n\t"
            "movsd %%xmm11, %%xmm3\n\tmovss %%xmm12, %%xmm4\n\t"
            "movq %%xmm13, %%xmm5\n\tunpcklpd %%xmm14, %%xmm6\n\t"
            "shufpd $1, %%xmm15, %%xmm7\n\tfnop\n\t"
            "paddq %%xmm0, %%xmm8\n\tpxor %%xmm1, %%xmm9\n\t"
            "pand %%xmm2, %%xmm10\n\tpor %%xmm3, %%xmm11\n\t"
            "movdqu %%xmm4, %%xmm12\n\tpsllq $5, %%xmm13\n\t"
            "pshufd $0x4e, %%xmm14, %%xmm14\n\tpunpckldq %%xmm6, %%xmm15\n\tfnop\n\t"
            "movdqu %%xmm0, 0(%[p])\n\t"
            "movdqu %%xmm1, 16(%[p])\n\t"
            "movdqu %%xmm2, 32(%[p])\n\t"
            "movdqu %%xmm3, 48(%[p])\n\t"
            "movdqu %%xmm4, 64(%[p])\n\t"
            "movdqu %%xmm5, 80(%[p])\n\t"
            "movdqu %%xmm6, 96(%[p])\n\t"
            "movdqu %%xmm7, 112(%[p])\n\t"
            "movdqu %%xmm8, 128(%[p])\n\t"
            "movdqu %%xmm9, 144(%[p])\n\t"
            "movdqu %%xmm10, 160(%[p])\n\t"
            "movdqu %%xmm11, 176(%[p])\n\t"
            "movdqu %%xmm12, 192(%[p])\n\t"
            "movdqu %%xmm13, 208(%[p])\n\t"
            "movdqu %%xmm14, 224(%[p])\n\t"
            "movdqu %%xmm15, 240(%[p])\n\t"
            : : [p] "r"(regs)
            : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
        for (int j = 0; j < 16; j++) h = mix(mix(h, (u64)regs[j][0]), (u64)regs[j][1]);
    }
    out("gpta_xmm_lane_state", h);
}

#define SCALAR_MEM_CASE(X, OP, SCALE, DISP) do { \
    v2 r; u64 f, fi = flags_in(); \
    __asm__ volatile(PRE "movdqu %[seed], %%xmm" #X "\n\t" \
        OP " " #DISP "(%[p],%[ix]," #SCALE "), %%xmm" #X "\n\t" \
        "movdqu %%xmm" #X ", %[r]\n\t" \
        OP " %%xmm" #X ", 96(%[p],%[ix]," #SCALE ")" POST \
        : [r] "=m"(r), [f] "=&r"(f) \
        : [seed] "m"(seed), [p] "r"(arr + 8), [ix] "r"((u64)-3), [fi] "r"(fi) \
        : "xmm" #X, "cc", "memory"); \
    h = mix(vmix(h, r), f & ARITH); \
    for (int q = 0; q < 32; q++) h = mix(h, arr[q]); \
} while (0)
#define SCALAR_MEM_REG(X) \
    SCALAR_MEM_CASE(X, "movss", 1, 0); SCALAR_MEM_CASE(X, "movsd", 1, 0); \
    SCALAR_MEM_CASE(X, "movss", 2, 7); SCALAR_MEM_CASE(X, "movsd", 2, 7); \
    SCALAR_MEM_CASE(X, "movss", 4, -9); SCALAR_MEM_CASE(X, "movsd", 4, -9); \
    SCALAR_MEM_CASE(X, "movss", 8, 11); SCALAR_MEM_CASE(X, "movsd", 8, 11)
static void gpta_scalar_mem(void) {
    u64 h = 14695981039346656037ull, arr[32];
    for (int i = 0; i < N; i++) {
        for (int q = 0; q < 32; q++) arr[q] = val();
        v2 seed = vdouble(1);
        SCALAR_MEM_REG(0); SCALAR_MEM_REG(1); SCALAR_MEM_REG(2); SCALAR_MEM_REG(3);
        SCALAR_MEM_REG(4); SCALAR_MEM_REG(5); SCALAR_MEM_REG(6); SCALAR_MEM_REG(7);
        SCALAR_MEM_REG(8); SCALAR_MEM_REG(9); SCALAR_MEM_REG(10); SCALAR_MEM_REG(11);
        SCALAR_MEM_REG(12); SCALAR_MEM_REG(13); SCALAR_MEM_REG(14); SCALAR_MEM_REG(15);
    }
    out("gpta_scalar_mem", h);
}

#define UNARY_FLOW_CASE(OP, CARRY) do { \
    u64 a = val(), b = val(), d = val(), result; unsigned char flags[5]; \
    __asm__ volatile("movq %[a], %%rax\n\taddq %[b], %%rax\n\t" \
        "movq %[d], %%rcx\n\t" OP "\n\t" \
        "seto %[of]\n\tsetz %[zf]\n\tsets %[sf]\n\tsetp %[pf]\n\t" \
        CARRY "\n\ttestq %%rax, %%rax\n\tmovq %%rcx, %[r]" \
        : [of] "=m"(flags[0]), [zf] "=m"(flags[1]), [sf] "=m"(flags[2]), \
          [pf] "=m"(flags[3]), [cf] "=m"(flags[4]), [r] "=m"(result) \
        : [a] "r"(a), [b] "r"(b), [d] "r"(d) : "rax", "rcx", "cc", "memory"); \
    h = mix(h, result); for (int q = 0; q < 5; q++) h = mix(h, flags[q]); \
} while (0)
#define UNARY_FLOW_FORMS(OP) \
    UNARY_FLOW_CASE(OP, "movb $0, %[cf]"); \
    UNARY_FLOW_CASE(OP, "setc %[cf]")
static void gpta_unary_flag_inputs(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        UNARY_FLOW_FORMS("incb %%cl"); UNARY_FLOW_FORMS("incw %%cx");
        UNARY_FLOW_FORMS("incl %%ecx"); UNARY_FLOW_FORMS("incq %%rcx");
        UNARY_FLOW_FORMS("decb %%cl"); UNARY_FLOW_FORMS("decw %%cx");
        UNARY_FLOW_FORMS("decl %%ecx"); UNARY_FLOW_FORMS("decq %%rcx");
        UNARY_FLOW_FORMS("negb %%cl"); UNARY_FLOW_FORMS("negw %%cx");
        UNARY_FLOW_FORMS("negl %%ecx"); UNARY_FLOW_FORMS("negq %%rcx");
        UNARY_FLOW_FORMS("incq %%rcx\n\tdecq %%rcx");
    }
    out("gpta_unary_flag_inputs", h);
}

// All sixteen vectors survive hot-set changes, explicit-operand fallback,
// implicit FXSAVE/FXRSTOR, and scalar operations that preserve upper lanes.
static void gpta_xmap_transitions(void) {
    v2 regs[16];
    unsigned char saved[512] __attribute__((aligned(16)));
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 16; j++) regs[j] = vrand();
        u64 a = val();
        __asm__ volatile(
            "movdqu 0(%[p]), %%xmm0\n\t"
            "movdqu 16(%[p]), %%xmm1\n\t"
            "movdqu 32(%[p]), %%xmm2\n\t"
            "movdqu 48(%[p]), %%xmm3\n\t"
            "movdqu 64(%[p]), %%xmm4\n\t"
            "movdqu 80(%[p]), %%xmm5\n\t"
            "movdqu 96(%[p]), %%xmm6\n\t"
            "movdqu 112(%[p]), %%xmm7\n\t"
            "movdqu 128(%[p]), %%xmm8\n\t"
            "movdqu 144(%[p]), %%xmm9\n\t"
            "movdqu 160(%[p]), %%xmm10\n\t"
            "movdqu 176(%[p]), %%xmm11\n\t"
            "movdqu 192(%[p]), %%xmm12\n\t"
            "movdqu 208(%[p]), %%xmm13\n\t"
            "movdqu 224(%[p]), %%xmm14\n\t"
            "movdqu 240(%[p]), %%xmm15\n\t"
            "movl $7, %%ecx\n\t1:\n\t"
            "paddq %%xmm9, %%xmm8\n\tpxor %%xmm10, %%xmm11\n\t"
            "psubq %%xmm12, %%xmm13\n\tpor %%xmm14, %%xmm15\n\t"
            "decl %%ecx\n\tjne 1b\n\t"
            "call 8f\n\t"
            "pinsrq $1, %[a], %%xmm13\n\tfnop\n\t"
            "movl $11, %%ecx\n\t2:\n\t"
            "paddq %%xmm1, %%xmm0\n\tpxor %%xmm2, %%xmm3\n\t"
            "psubq %%xmm4, %%xmm5\n\tpor %%xmm6, %%xmm7\n\t"
            "decl %%ecx\n\tjne 2b\n\t"
            "call 8f\n\t"
            "fxsave64 %[saved]\n\t"
            "pxor %%xmm0, %%xmm0\n\t"
            "pxor %%xmm1, %%xmm1\n\t"
            "pxor %%xmm2, %%xmm2\n\t"
            "pxor %%xmm3, %%xmm3\n\t"
            "pxor %%xmm4, %%xmm4\n\t"
            "pxor %%xmm5, %%xmm5\n\t"
            "pxor %%xmm6, %%xmm6\n\t"
            "pxor %%xmm7, %%xmm7\n\t"
            "pxor %%xmm8, %%xmm8\n\t"
            "pxor %%xmm9, %%xmm9\n\t"
            "pxor %%xmm10, %%xmm10\n\t"
            "pxor %%xmm11, %%xmm11\n\t"
            "pxor %%xmm12, %%xmm12\n\t"
            "pxor %%xmm13, %%xmm13\n\t"
            "pxor %%xmm14, %%xmm14\n\t"
            "pxor %%xmm15, %%xmm15\n\t"
            "fxrstor64 %[saved]\n\t"
            "movl $13, %%ecx\n\t3:\n\t"
            "paddq %%xmm15, %%xmm1\n\tpxor %%xmm13, %%xmm3\n\t"
            "psubq %%xmm11, %%xmm5\n\tpor %%xmm9, %%xmm7\n\t"
            "movss %%xmm3, %%xmm13\n\tmovsd %%xmm5, %%xmm15\n\t"
            "decl %%ecx\n\tjne 3b\n\t"
            // Different vector subsets on alternating paths, plus an inner
            // loop, exercise region allocation without discarding any state.
            "movl $9, %%ecx\n\t4:\n\t"
            "paddq %%xmm15, %%xmm8\n\ttestl $1, %%ecx\n\tjz 5f\n\t"
            "pxor %%xmm1, %%xmm2\n\tpaddq %%xmm3, %%xmm4\n\tjmp 6f\n\t"
            "5: pxor %%xmm9, %%xmm10\n\tpsubq %%xmm11, %%xmm12\n\t"
            "6: movl $3, %%edx\n\t7: paddq %%xmm5, %%xmm14\n\t"
            "decl %%edx\n\tjne 7b\n\tdecl %%ecx\n\tjne 4b\n\t"
            // The same map-free direct jump is reached under different maps.
            "jmp 10f\n\t8: jmp 9f\n\t9: pxor %%xmm0, %%xmm15\n\tret\n\t10:\n\t"
            "movdqu %%xmm0, 0(%[p])\n\t"
            "movdqu %%xmm1, 16(%[p])\n\t"
            "movdqu %%xmm2, 32(%[p])\n\t"
            "movdqu %%xmm3, 48(%[p])\n\t"
            "movdqu %%xmm4, 64(%[p])\n\t"
            "movdqu %%xmm5, 80(%[p])\n\t"
            "movdqu %%xmm6, 96(%[p])\n\t"
            "movdqu %%xmm7, 112(%[p])\n\t"
            "movdqu %%xmm8, 128(%[p])\n\t"
            "movdqu %%xmm9, 144(%[p])\n\t"
            "movdqu %%xmm10, 160(%[p])\n\t"
            "movdqu %%xmm11, 176(%[p])\n\t"
            "movdqu %%xmm12, 192(%[p])\n\t"
            "movdqu %%xmm13, 208(%[p])\n\t"
            "movdqu %%xmm14, 224(%[p])\n\t"
            "movdqu %%xmm15, 240(%[p])\n\t"
            : [saved] "=m"(saved)
            : [p] "r"(regs), [a] "r"(a)
            : "rcx", "rdx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
              "memory", "cc");
        for (int j = 0; j < 16; j++) h = vmix(h, regs[j]);
    }
    out("gpta_xmap_transitions", h);
}

// The producer's non-carry flags are overwritten by INC/DEC, while CF
// remains observable. Test register aliases, immediates and a block boundary.
#define CF_ONLY_CASE(PRODUCER, UNARY, EDGE) do { \
    u64 a = val(), b = val(), d = val(), r, q, f; \
    __asm__ volatile("movq %[a], %%rax\n\tmovq %[b], %%rcx\n\tmovq %[d], %%rdx\n\t" \
        PRODUCER "\n\t" EDGE UNARY "\n\tpushfq\n\tpopq %[f]\n\t" \
        "movq %%rax, %[r]\n\tmovq %%rdx, %[q]\n\ttestq %%rax, %%rax" \
        : [r] "=m"(r), [q] "=m"(q), [f] "=&r"(f) \
        : [a] "r"(a), [b] "r"(b), [d] "r"(d) : "rax", "rcx", "rdx", "cc", "memory"); \
    h = mix(mix(mix(h, r), q), f & ARITH); \
} while (0)
#define CF_ONLY_TARGETS(PRODUCER) \
    CF_ONLY_CASE(PRODUCER, "incb %%dl", ""); CF_ONLY_CASE(PRODUCER, "decw %%dx", ""); \
    CF_ONLY_CASE(PRODUCER, "incl %%edx", ""); CF_ONLY_CASE(PRODUCER, "decq %%rdx", ""); \
    CF_ONLY_CASE(PRODUCER, "incq %%rdx", "jmp 1f\n\t1:\n\t")
#define CF_ONLY_FORMS(OP) \
    CF_ONLY_TARGETS(OP "l %%ecx, %%eax"); CF_ONLY_TARGETS(OP "q %%rcx, %%rax"); \
    CF_ONLY_TARGETS(OP "l %%eax, %%eax"); CF_ONLY_TARGETS(OP "q %%rax, %%rax"); \
    CF_ONLY_TARGETS(OP "l $-1, %%eax"); CF_ONLY_TARGETS(OP "q $0x7fffffff, %%rax")
static void gpta_cf_only(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        CF_ONLY_FORMS("add"); CF_ONLY_FORMS("sub"); CF_ONLY_FORMS("and");
        CF_ONLY_FORMS("or"); CF_ONLY_FORMS("xor");
    }
    out("gpta_cf_only", h);
}

// A flag-killing loop entry makes the unary pair's backedge flag-independent.
// Exercise both adjacent-edge choices and final exits from replicated traces.
#define UT_TEST(OP,REG,START,BR) do { \
    u64 a = (START), r, f; \
    __asm__ volatile("movq %[a], %%rcx\n\tstc\n\t1: xorq %%rdx, %%rdx\n\t" \
        OP " " REG "\n\t" BR "\n\tpushfq\n\tpopq %[f]\n\tmovq %%rcx, %[r]\n\t" \
        "testq %%rcx, %%rcx" \
        : [r] "=m"(r), [f] "=&r"(f) : [a] "r"(a) : "rcx", "rdx", "cc", "memory"); \
    h = mix(mix(h,r),f & ARITH); \
} while (0)
#define UT_BOTH(OP,REG,START,CC,INV) \
    UT_TEST(OP,REG,START,"j" CC " 1b"); \
    UT_TEST(OP,REG,START,"j" INV " 2f\n\tjmp 1b\n\t2:")
#define UT_WIDTH(SZ,REG,HIGH,NEG,MAX,MIN) \
    UT_BOTH("inc" SZ,REG,(NEG)+16ull,"e","ne"); \
    UT_BOTH("dec" SZ,REG,(HIGH)|1ull,"e","ne"); \
    UT_BOTH("inc" SZ,REG,(NEG),"ne","e"); \
    UT_BOTH("dec" SZ,REG,(HIGH)|17ull,"ne","e"); \
    UT_BOTH("inc" SZ,REG,(NEG),"s","ns"); \
    UT_BOTH("dec" SZ,REG,(MIN)+18ull,"s","ns"); \
    UT_BOTH("inc" SZ,REG,(MAX)-18ull,"ns","s"); \
    UT_BOTH("dec" SZ,REG,(HIGH)|18ull,"ns","s")
static void gpta_unary_trace(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < 32; i++) {
        u64 upper = val() & 0xffffffff00000000ull;
        UT_WIDTH("l","%%ecx",upper,upper | 0xffffffefull,upper | 0x7fffffffull,upper | 0x80000000ull);
        UT_WIDTH("q","%%rcx",0,~16ull,0x7fffffffffffffffull,0x8000000000000000ull);
    }
    out("gpta_unary_trace",h);
}
typedef void (*test_fn)(void);
#define SX_CASE(OP,SZ,REG,DEST,SRC,COUNT,TAIL) do { \
    u64 a = val(), b = val(), r, q, fi = flags_in(), f; \
    __asm__ volatile(PRE "movq %[a], %%rax\n\tmovq %[b], %%rdx\n\t" \
        OP SZ " %[n], " REG "\n\txor" SZ " " SRC ", " DEST "\n\t" TAIL \
        "pushfq\n\tpopq %[f]\n\tmovq %%rax, %[r]\n\tmovq %%rdx, %[q]\n\ttestq %%rax, %%rax" \
        : [r] "=m"(r), [q] "=m"(q), [f] "=&r"(f) \
        : [a] "r"(a), [b] "r"(b), [fi] "r"(fi), [n] "i"(COUNT) : "rax", "rdx", "cc", "memory"); \
    h = mix(mix(mix(h,r),q),f & LOGIC); \
} while (0)
#define SX_FORMS(OP,COUNT) \
    SX_CASE(OP,"l","%%eax","%%eax","%%edx",COUNT,""); \
    SX_CASE(OP,"q","%%rax","%%rax","%%rdx",COUNT,""); \
    SX_CASE(OP,"l","%%eax","%%edx","%%eax",COUNT,""); \
    SX_CASE(OP,"q","%%rax","%%rdx","%%rax",COUNT,""); \
    SX_CASE(OP,"l","%%eax","%%eax","%%eax",COUNT,""); \
    SX_CASE(OP,"q","%%rax","%%rax","%%rax",COUNT,""); \
    SX_CASE(OP,"l","%%eax","%%edx","%%eax",COUNT,"xorl %k[f], %k[f]\n\t"); \
    SX_CASE(OP,"q","%%rax","%%rax","%%rdx",COUNT,"xorl %k[f], %k[f]\n\t")
#define SX_COUNT(COUNT) SX_FORMS("shl",COUNT); SX_FORMS("shr",COUNT); \
    SX_FORMS("sar",COUNT); SX_FORMS("rol",COUNT); SX_FORMS("ror",COUNT)
static void gpta_shift_xor(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        SX_COUNT(0); SX_COUNT(1); SX_COUNT(2); SX_COUNT(3); SX_COUNT(4); SX_COUNT(5); SX_COUNT(6); SX_COUNT(7);
        SX_COUNT(8); SX_COUNT(9); SX_COUNT(10); SX_COUNT(11); SX_COUNT(12); SX_COUNT(13); SX_COUNT(14); SX_COUNT(15);
        SX_COUNT(16); SX_COUNT(17); SX_COUNT(18); SX_COUNT(19); SX_COUNT(20); SX_COUNT(21); SX_COUNT(22); SX_COUNT(23);
        SX_COUNT(24); SX_COUNT(25); SX_COUNT(26); SX_COUNT(27); SX_COUNT(28); SX_COUNT(29); SX_COUNT(30); SX_COUNT(31);
        SX_COUNT(32); SX_COUNT(33); SX_COUNT(34); SX_COUNT(35); SX_COUNT(36); SX_COUNT(37); SX_COUNT(38); SX_COUNT(39);
        SX_COUNT(40); SX_COUNT(41); SX_COUNT(42); SX_COUNT(43); SX_COUNT(44); SX_COUNT(45); SX_COUNT(46); SX_COUNT(47);
        SX_COUNT(48); SX_COUNT(49); SX_COUNT(50); SX_COUNT(51); SX_COUNT(52); SX_COUNT(53); SX_COUNT(54); SX_COUNT(55);
        SX_COUNT(56); SX_COUNT(57); SX_COUNT(58); SX_COUNT(59); SX_COUNT(60); SX_COUNT(61); SX_COUNT(62); SX_COUNT(63);
        SX_COUNT(64); SX_COUNT(127); SX_COUNT(255);
    }
    out("gpta_shift_xor",h);
}
#define AF_COMPARE_0(CMP,D,C) CMP " %%xmm" #C ", %%xmm" #D "\n\t"
#define AF_COMPARE_1(CMP,D,C) CMP " %%xmm" #D ", %%xmm" #C "\n\t"
#define AF_COPY_0(D) ""
#define AF_COPY_1(D) "movapd %%xmm" #D ", %%xmm3\n\tmovapd %%xmm3, %%xmm" #D "\n\t"
#define AF_CASE(OP,CMP,D,S,C,CC) do { \
    AF_CASE_ORDER(OP,CMP,D,S,C,CC,0,0); AF_CASE_ORDER(OP,CMP,D,S,C,CC,1,0); \
    AF_CASE_ORDER(OP,CMP,D,S,C,CC,0,1); AF_CASE_ORDER(OP,CMP,D,S,C,CC,1,1); \
} while (0)
#define AF_CASE_ORDER(OP,CMP,D,S,C,CC,ORDER,COPY) do { \
    v2 r; u64 score, f; \
    __asm__ volatile("movdqu %[a], %%xmm" #D "\n\t" \
        "movdqu %[b], %%xmm" #S "\n\tmovdqu %[c], %%xmm" #C "\n\t" \
        AF_COPY_##COPY(D) \
        OP " %%xmm" #S ", %%xmm" #D "\n\t" \
        AF_COMPARE_##ORDER(CMP,D,C) "j" CC " 1f\n\t" \
        "movq $3, %[score]\n\tjmp 2f\n\t1: movq $5, %[score]\n\t2:\n\t" \
        "pushfq\n\tpopq %[f]\n\tmovdqu %%xmm" #D ", %[r]\n\ttestq %[f], %[f]" \
        : [r] "=m"(r), [score] "=m"(score), [f] "=&r"(f) \
        : [a] "m"(a), [b] "m"(b), [c] "m"(c) \
        : "xmm" #D, "xmm" #S, "xmm" #C, "xmm3", "cc", "memory"); \
    h = mix(mix(vmix(h,r),score),f & ARITH); \
} while (0)
#define AF_REGS(OP,CMP,CC) \
    AF_CASE(OP,CMP,0,1,2,CC); AF_CASE(OP,CMP,7,14,13,CC); \
    AF_CASE(OP,CMP,12,13,14,CC)
#define AF_CCS(OP,CMP) \
    AF_REGS(OP,CMP,"o"); AF_REGS(OP,CMP,"no"); AF_REGS(OP,CMP,"b"); AF_REGS(OP,CMP,"ae"); \
    AF_REGS(OP,CMP,"e"); AF_REGS(OP,CMP,"ne"); AF_REGS(OP,CMP,"be"); AF_REGS(OP,CMP,"a"); \
    AF_REGS(OP,CMP,"s"); AF_REGS(OP,CMP,"ns"); AF_REGS(OP,CMP,"p"); AF_REGS(OP,CMP,"np"); \
    AF_REGS(OP,CMP,"l"); AF_REGS(OP,CMP,"ge"); AF_REGS(OP,CMP,"le"); AF_REGS(OP,CMP,"g")
#define AF_ALIAS_CASE(OP,CMP,D,S,C) do { \
    AF_ALIAS_ORDER(OP,CMP,D,S,C,0); AF_ALIAS_ORDER(OP,CMP,D,S,C,1); \
} while (0)
#define AF_ALIAS_ORDER(OP,CMP,D,S,C,ORDER) do { \
    v2 r; u64 score, f; \
    __asm__ volatile("movdqu %[a], %%xmm" #D "\n\t" \
        "movdqu %[b], %%xmm" #S "\n\t" \
        OP " %%xmm" #S ", %%xmm" #D "\n\t" \
        AF_COMPARE_##ORDER(CMP,D,C) "jbe 1f\n\t" \
        "movq $3, %[score]\n\tjmp 2f\n\t1: movq $5, %[score]\n\t2:\n\t" \
        "pushfq\n\tpopq %[f]\n\tmovdqu %%xmm" #D ", %[r]\n\ttestq %[f], %[f]" \
        : [r] "=m"(r), [score] "=m"(score), [f] "=&r"(f) \
        : [a] "m"(a), [b] "m"(b) : "xmm" #D, "xmm" #S, "xmm" #C, "cc", "memory"); \
    h = mix(mix(vmix(h,r),score),f & ARITH); \
} while (0)
#define AF_OP(OP,CMP) \
    AF_CCS(OP,CMP); AF_ALIAS_CASE(OP,CMP,8,8,8); \
    AF_ALIAS_CASE(OP,CMP,13,14,13); AF_ALIAS_CASE(OP,CMP,7,14,14)
static void gpta_arith_float_branch(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vfloat(1), b = vfloat(1);
        union { float f[4]; v2 v; } cf; cf.v = vrand(); cf.f[0] = (float)dval(i);
        v2 c = cf.v;
        AF_OP("addss","ucomiss"); AF_OP("subss","ucomiss");
        AF_OP("mulss","ucomiss"); AF_OP("divss","ucomiss");
        a = vdouble(1); b = vdouble(1);
        union { double d[2]; v2 v; } cd; cd.v = vrand(); cd.d[0] = dval(i);
        c = cd.v;
        AF_OP("addsd","ucomisd"); AF_OP("subsd","ucomisd");
        AF_OP("mulsd","ucomisd"); AF_OP("divsd","ucomisd");
    }
    out("gpta_arith_float_branch",h);
}
#define AF_TRACE_CASE(OP,CMP,D,S,C,CC,ORDER) do { \
    v2 r; u64 score; \
    __asm__ volatile("movdqu %[a], %%xmm" #D "\n\t" \
        "movdqu %[b], %%xmm" #S "\n\tmovdqu %[c], %%xmm" #C "\n\t" \
        "movl $17, %%ecx\n\txorl %%eax, %%eax\n\t1:\n\t" \
        OP " %%xmm" #S ", %%xmm" #D "\n\t" \
        AF_COMPARE_##ORDER(CMP,D,C) "j" CC " 2f\n\t" \
        "addq $3, %%rax\n\tjmp 3f\n\t2: addq $5, %%rax\n\t" \
        "3: subl $1, %%ecx\n\tjnz 1b\n\tmovdqu %%xmm" #D ", %[r]" \
        : [r] "=m"(r), "=&a"(score) : [a] "m"(a), [b] "m"(b), [c] "m"(c) \
        : "rcx", "xmm" #D, "xmm" #S, "xmm" #C, "cc", "memory"); \
    h = mix(vmix(h,r),score); \
} while (0)
#define AF_TRACE_REGS(OP,CMP,CC) \
    AF_TRACE_CASE(OP,CMP,0,1,2,CC,0); AF_TRACE_CASE(OP,CMP,0,1,2,CC,1); \
    AF_TRACE_CASE(OP,CMP,14,13,7,CC,0); AF_TRACE_CASE(OP,CMP,14,13,7,CC,1)
#define AF_TRACE_CCS(OP,CMP) \
    AF_TRACE_REGS(OP,CMP,"o"); AF_TRACE_REGS(OP,CMP,"no"); \
    AF_TRACE_REGS(OP,CMP,"b"); AF_TRACE_REGS(OP,CMP,"ae"); \
    AF_TRACE_REGS(OP,CMP,"e"); AF_TRACE_REGS(OP,CMP,"ne"); \
    AF_TRACE_REGS(OP,CMP,"be"); AF_TRACE_REGS(OP,CMP,"a"); \
    AF_TRACE_REGS(OP,CMP,"s"); AF_TRACE_REGS(OP,CMP,"ns"); \
    AF_TRACE_REGS(OP,CMP,"p"); AF_TRACE_REGS(OP,CMP,"np"); \
    AF_TRACE_REGS(OP,CMP,"l"); AF_TRACE_REGS(OP,CMP,"ge"); \
    AF_TRACE_REGS(OP,CMP,"le"); AF_TRACE_REGS(OP,CMP,"g")
static void gpta_arith_float_trace(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        v2 a = vfloat(1), b = vfloat(1);
        union { float f[4]; v2 v; } cf; cf.v = vrand(); cf.f[0] = (float)dval(i);
        v2 c = cf.v;
        AF_TRACE_CCS("addss","ucomiss");
        a = vdouble(1); b = vdouble(1);
        union { double d[2]; v2 v; } cd; cd.v = vrand(); cd.d[0] = dval(i);
        c = cd.v;
        AF_TRACE_CCS("addsd","ucomisd");
    }
    // Taken adjacency, including the final non-adjacent replica backedge;
    // the exit consumes the original (reversed-operand) comparison flags.
    double one = 1.0, limit = 11.0, result; u64 f;
    __asm__ volatile("pxor %%xmm7, %%xmm7\n\tmovsd %[one], %%xmm8\n\tmovsd %[lim], %%xmm14\n\t"
        "1: addsd %%xmm8, %%xmm7\n\tucomisd %%xmm7, %%xmm14\n\tja 1b\n\t"
        "pushfq\n\tpopq %[f]\n\tmovsd %%xmm7, %[r]\n\txorl %%eax, %%eax"
        : [r] "=m"(result), [f] "=&r"(f) : [one] "m"(one), [lim] "m"(limit)
        : "rax", "xmm7", "xmm8", "xmm14", "cc", "memory");
    h = mix(mix(h,(u64)result),f & ARITH);
    float onef = 1.0f, limitf = 11.0f, resultf;
    __asm__ volatile("pxor %%xmm14, %%xmm14\n\tmovss %[one], %%xmm1\n\tmovss %[lim], %%xmm0\n\t"
        "1: addss %%xmm1, %%xmm14\n\tucomiss %%xmm14, %%xmm0\n\tja 1b\n\t"
        "pushfq\n\tpopq %[f]\n\tmovss %%xmm14, %[r]\n\txorl %%eax, %%eax"
        : [r] "=m"(resultf), [f] "=&r"(f) : [one] "m"(onef), [lim] "m"(limitf)
        : "rax", "xmm0", "xmm1", "xmm14", "cc", "memory");
    h = mix(mix(h,(u64)resultf),f & ARITH);
    out("gpta_arith_float_trace",h);
}
#define MT_CASE(MSZ,MSRC,MDST,TSZ,TDST,CC,SETUP) do { \
    u64 a = val(), seed = val(), r, score, fi = flags_in(), f; \
    __asm__ volatile(PRE "movq %[a], %%rcx\n\tmovq %[seed], %%rax\n\t" SETUP \
        "mov" MSZ " " MSRC ", " MDST "\n\t" \
        "test" TSZ " " TDST ", " TDST "\n\tj" CC " 1f\n\t" \
        "movq $3, %%rdx\n\tjmp 2f\n\t1: movq $5, %%rdx\n\t2:\n\t" \
        "pushfq\n\tpopq %[f]\n\tmovq %%rax, %[r]\n\tmovq %%rdx, %[score]\n\t" \
        "testq %%rax, %%rax" \
        : [r] "=m"(r), [score] "=m"(score), [f] "=&r"(f) \
        : [a] "r"(a), [seed] "r"(seed), [fi] "r"(fi) : "rax", "rcx", "rdx", "cc", "memory"); \
    h = mix(mix(mix(h,r),score),f & LOGIC); \
} while (0)
#define MT_FORMS(CC) \
    MT_CASE("l","%%ecx","%%eax","l","%%eax",CC,""); \
    MT_CASE("l","%%ecx","%%eax","q","%%rax",CC,""); \
    MT_CASE("q","%%rcx","%%rax","l","%%eax",CC,""); \
    MT_CASE("q","%%rcx","%%rax","q","%%rax",CC,""); \
    MT_CASE("l","%%eax","%%eax","l","%%eax",CC,"movq %%rcx, %%rax\n\t"); \
    MT_CASE("l","%%eax","%%eax","q","%%rax",CC,"movq %%rcx, %%rax\n\t"); \
    MT_CASE("q","%%rax","%%rax","l","%%eax",CC,"movq %%rcx, %%rax\n\t"); \
    MT_CASE("q","%%rax","%%rax","q","%%rax",CC,"movq %%rcx, %%rax\n\t")
static void gpta_mov_test_branch(void) {
    u64 h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        MT_FORMS("o"); MT_FORMS("no"); MT_FORMS("b"); MT_FORMS("ae");
        MT_FORMS("e"); MT_FORMS("ne"); MT_FORMS("be"); MT_FORMS("a");
        MT_FORMS("s"); MT_FORMS("ns"); MT_FORMS("p"); MT_FORMS("np");
        MT_FORMS("l"); MT_FORMS("ge"); MT_FORMS("le"); MT_FORMS("g");
    }
    out("gpta_mov_test_branch",h);
}
#define EA_PAIR_CASE(OP1,DST1,OP2,DST2,DISP1,DISP2,SCALE) do { \
    u64 a, b, fi = flags_in(), f; v2 x, y; \
    __asm__ volatile(PRE "movdqu %[seed], %%xmm0\n\tmovdqu %[seed], %%xmm14\n\t" \
        "movq $0x1011, %%rax\n\tmovq $0x2011, %%rdx\n\t" \
        OP1 " " DISP1 "(%[p],%[k]," #SCALE "), " DST1 "\n\t" \
        OP2 " " DISP2 "(%[p],%[k]," #SCALE "), " DST2 "\n\t" \
        "movq %%rax, %[a]\n\tmovq %%rdx, %[b]\n\t" \
        "movdqu %%xmm0, %[x]\n\tmovdqu %%xmm14, %[y]" POST \
        : [a] "=m"(a), [b] "=m"(b), [x] "=m"(x), [y] "=m"(y), [f] "=&r"(f) \
        : [p] "r"(&data[8]), [k] "r"((u64)3), [seed] "m"(seed), [fi] "r"(fi) \
        : "rax", "rdx", "xmm0", "xmm14", "cc", "memory"); \
    h = mix(vmix(vmix(mix(mix(h,a),b),x),y),f & ARITH); \
} while (0)
#define EA_PAIR_ADDRS(OP1,D1,OP2,D2) \
    EA_PAIR_CASE(OP1,D1,OP2,D2,"0","0",1); \
    EA_PAIR_CASE(OP1,D1,OP2,D2,"13","-7",2); \
    EA_PAIR_CASE(OP1,D1,OP2,D2,"0","16",4); \
    EA_PAIR_CASE(OP1,D1,OP2,D2,"-9","11",8)
static void gpta_ea_pairs(void) {
    u64 data[64], h = 14695981039346656037ull;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 64; j++) data[j] = val();
        v2 seed = vrand();
        EA_PAIR_ADDRS("movq","%%rax","movq","%%rdx");
        EA_PAIR_ADDRS("movl","%%eax","movl","%%edx");
        EA_PAIR_ADDRS("movzbl","%%eax","movzbl","%%edx");
        EA_PAIR_ADDRS("movq","%%rax","movl","%%eax");
        EA_PAIR_ADDRS("movups","%%xmm0","movups","%%xmm14");
        EA_PAIR_ADDRS("movss","%%xmm0","movsd","%%xmm14");
        EA_PAIR_ADDRS("movsd","%%xmm14","movss","%%xmm14");
        EA_PAIR_ADDRS("movq","%%rax","movups","%%xmm14");
        EA_PAIR_ADDRS("movss","%%xmm0","movzbl","%%edx");
        // The first destination changes the base or index: reuse must reject
        // these cases and calculate the second address from updated GPRs.
        data[8] = (u64)(unsigned long)&data[32];
        data[9] = 3;
        u64 a, b;
        __asm__ volatile("movq %[p], %%rbx\n\tmovq (%%rbx), %%rbx\n\t"
            "movq 8(%%rbx), %%rax\n\tmovq %%rax, %[a]\n\t"
            "movq $1, %%rdx\n\tmovq (%[p],%%rdx,8), %%rdx\n\t"
            "movq 8(%[p],%%rdx,8), %%rax\n\tmovq %%rax, %[b]"
            : [a] "=m"(a), [b] "=m"(b) : [p] "r"(&data[8]) : "rax", "rbx", "rdx", "memory");
        h = mix(mix(h,a),b);
    }
    out("gpta_ea_pairs",h);
}
// Load/arithmetic pairs preserve scalar zeroed upper lanes, source aliases,
// all architectural XMM operands, and the incoming arithmetic flags.
#define XL_CASE(OP,LOAD,D,S,ADDR,P) XL_CASE_I(OP,LOAD,D,S,ADDR,P,0)
#define XL_CASE_I(OP,LOAD,D,S,ADDR,P,IX) do { \
    v2 r, q; u64 fi = flags_in(), f; \
    __asm__ volatile(PRE "movdqu %[seed], %%xmm" #D "\n\t" \
        "movdqu %[b], %%xmm" #S "\n\t" \
        LOAD " " ADDR ", %%xmm" #D "\n\t" \
        OP " %%xmm" #S ", %%xmm" #D "\n\t" \
        "movdqu %%xmm" #D ", %[r]\n\tmovdqu %%xmm" #S ", %[q]" POST \
        : [r] "=m"(r), [q] "=m"(q), [f] "=&r"(f) \
        : [seed] "m"(seed), [b] "m"(b), [p] "r"(P), [idx] "r"((u64)(IX)), [fi] "r"(fi) \
        : "xmm" #D, "xmm" #S, "memory", "cc"); \
    h = mix(vmix(vmix(h,r),q),f & ARITH); \
} while (0)
#define XL_DST_CASES(OP,LOAD,D,S) \
    XL_CASE(OP,LOAD,D,S,"16(%[p])",&mem[0]); \
    XL_CASE(OP,LOAD,D,D,"-16(%[p])",&mem[2]); \
    XL_CASE_I(OP,LOAD,D,S,"(%[p],%[idx])",&mem[0],16); \
    XL_CASE_I(OP,LOAD,D,D,"(%[p],%[idx])",&mem[0],16); \
    XL_CASE_I(OP,LOAD,D,S,"8(%[p],%[idx],2)",&mem[0],4); \
    XL_CASE_I(OP,LOAD,D,D,"8(%[p],%[idx],2)",&mem[0],4); \
    XL_CASE_I(OP,LOAD,D,S,"-16(%[p],%[idx],4)",&mem[0],8); \
    XL_CASE_I(OP,LOAD,D,D,"-16(%[p],%[idx],4)",&mem[0],8); \
    XL_CASE_I(OP,LOAD,D,S,"32(%[p],%[idx],8)",&mem[2],-6); \
    XL_CASE_I(OP,LOAD,D,D,"32(%[p],%[idx],8)",&mem[2],-6); \
    XL_CASE_I(OP,LOAD,D,S,"(%[p],%[idx])",(u64)&mem[1]/2,(u64)&mem[1]/2)
#define XL_REGS(OP,LOAD) \
    XL_DST_CASES(OP,LOAD,0,8); XL_DST_CASES(OP,LOAD,1,9); \
    XL_DST_CASES(OP,LOAD,2,10); XL_DST_CASES(OP,LOAD,3,11); \
    XL_DST_CASES(OP,LOAD,4,12); XL_DST_CASES(OP,LOAD,5,13); \
    XL_DST_CASES(OP,LOAD,6,14); XL_DST_CASES(OP,LOAD,7,15); \
    XL_DST_CASES(OP,LOAD,8,0); XL_DST_CASES(OP,LOAD,9,1); \
    XL_DST_CASES(OP,LOAD,10,2); XL_DST_CASES(OP,LOAD,11,3); \
    XL_DST_CASES(OP,LOAD,12,4); XL_DST_CASES(OP,LOAD,13,5); \
    XL_DST_CASES(OP,LOAD,14,6); XL_DST_CASES(OP,LOAD,15,7)
static void gpta_xload_pairs(void) {
    u64 h = 14695981039346656037ull;
    v2 mem[3];
    for (int i = 0; i < N; i++) {
        v2 seed = vrand(), b = vfloat(1); mem[1] = vfloat(1);
        XL_REGS("addps","movups"); XL_REGS("subps","movups"); XL_REGS("mulps","movups");
        XL_REGS("addss","movss"); XL_REGS("subss","movss"); XL_REGS("mulss","movss");
        b = vdouble(1); mem[1] = vdouble(1);
        XL_REGS("addpd","movupd"); XL_REGS("subpd","movupd"); XL_REGS("mulpd","movupd");
        XL_REGS("addsd","movsd"); XL_REGS("subsd","movsd"); XL_REGS("mulsd","movsd");
    }
    out("gpta_xload_pairs",h);
}
#define LP_CASE(D,E,A1,A2,P,IX) do { \
    v2 r, q; u64 fi = flags_in(), f; \
    __asm__ volatile(PRE "movdqu %[seed], %%xmm" #D "\n\t" \
        "movdqu %[seed], %%xmm" #E "\n\t" \
        "movdqu " A1 ", %%xmm" #D "\n\tmovdqu " A2 ", %%xmm" #E "\n\t" \
        "movdqu %%xmm" #D ", %[r]\n\tmovdqu %%xmm" #E ", %[q]" POST \
        : [r] "=m"(r), [q] "=m"(q), [f] "=&r"(f) \
        : [seed] "m"(seed), [p] "r"(P), [idx] "r"((u64)(IX)), [fi] "r"(fi) \
        : "xmm" #D, "xmm" #E, "memory", "cc"); \
    h = mix(vmix(vmix(h,r),q),f & ARITH); \
} while (0)
#define LP_SCALE_CASE(D,E,SC,IX) \
    LP_CASE(D,E,"(%[p],%[idx]," #SC ")","16(%[p],%[idx]," #SC ")",&mem[0],IX); \
    LP_CASE(D,E,"-16(%[p],%[idx]," #SC ")","(%[p],%[idx]," #SC ")",&mem[3],-(IX))
#define LP_REG_CASE(D,E) \
    LP_SCALE_CASE(D,E,1,16); LP_SCALE_CASE(D,E,2,8); \
    LP_SCALE_CASE(D,E,4,4); LP_SCALE_CASE(D,E,8,2); \
    LP_CASE(D,E,"16(%[p],%[idx],4)","(%[p],%[idx],4)",&mem[0],4); \
    LP_CASE(D,E,"(%[p],%[idx],2)","(%[p],%[idx],2)",&mem[0],8); \
    LP_CASE(D,E,"-16(%[p],%[idx],8)","16(%[p],%[idx],8)",&mem[1],2); \
    LP_CASE(D,E,"16(%[p])","-16(%[p])",&mem[1],0); \
    LP_CASE(D,E,"(%[p],%[idx])","16(%[p],%[idx])",(u64)&mem[1]/2,(u64)&mem[1]/2)
#define LP_DEST_CASE(D,E) LP_REG_CASE(D,E); LP_REG_CASE(D,D)
static void gpta_direct_load_pairs(void) {
    u64 h = 14695981039346656037ull;
    v2 mem[4];
    for (int i = 0; i < N; i++) {
        v2 seed = vrand();
        for (int k = 0; k < 4; k++) mem[k] = vrand();
        LP_DEST_CASE(0,8); LP_DEST_CASE(1,9); LP_DEST_CASE(2,10); LP_DEST_CASE(3,11);
        LP_DEST_CASE(4,12); LP_DEST_CASE(5,13); LP_DEST_CASE(6,14); LP_DEST_CASE(7,15);
        LP_DEST_CASE(8,0); LP_DEST_CASE(9,1); LP_DEST_CASE(10,2); LP_DEST_CASE(11,3);
        LP_DEST_CASE(12,4); LP_DEST_CASE(13,5); LP_DEST_CASE(14,6); LP_DEST_CASE(15,7);
    }
    out("gpta_direct_load_pairs",h);
}
#define ALU_LIST(OP) OP##_q, OP##_l, OP##_w, OP##_b, OP##_mr_q, OP##_rm_l, OP##_mr_b,
static const test_fn kTests[] = {
    ALU_LIST(add) ALU_LIST(adc) ALU_LIST(sub) ALU_LIST(sbb) ALU_LIST(cmp) ALU_LIST(and) ALU_LIST(or) ALU_LIST(xor)
    test_q, test_l, test_b, add_imm8_q, sub_imm32_l, and_imm_q, cmp_imm8_w, adc_imm_b, sbb_imm_q, a32_add_q, a32_imul_l,
    inc_q, inc_l, inc_w, inc_b, dec_q, dec_l, dec_b, neg_q, neg_l, neg_w, neg_b, not_q, not_l, not_b,
    bswap_q, bswap_l, shl1_q, sar1_l, shr1_b, rol1_q, ror1_w, rcl1_l, rcr1_b,
    shl_imm_q, shr_imm_l, sar_imm_w, rol_imm_b, ror_imm_q,
    shl_q, shl_l, shl_w, shl_b, shr_q, shr_l, shr_w, shr_b, sar_q, sar_l, sar_w, sar_b,
    rol_q, rol_l, rol_w, rol_b, ror_q, ror_l, ror_w, ror_b, rcl_q, rcl_l, rcl_w, rcl_b, rcr_q, rcr_l, rcr_w, rcr_b,
    shld_q, shld_l, shld_w, shrd_q, shrd_l, shrd_w,
    imul2_q, imul2_l, imul2_w, imul3_q, imul3_l, imul3_w,
    movsbq, movswq, movslq, movsbl, movsbw, movzbl, movzwl, movzbw, movl_zext, movw_keep, movb_keep,
    lea_q, lea_l, lea_w, lea_a32_l, lea_a32_q, lea_a32_w, rex2_add_q, rex2_mov_l, xchg_q, xchg_b, xadd_q, xadd_w, bt_q, bts_l, btr_q, btc_w, bt_imm_q, bts_imm_l,
    bsf_q, bsr_q,
    cc_o, cc_no, cc_b, cc_ae, cc_e, cc_ne, cc_be, cc_a, cc_s, cc_ns, cc_p, cc_np, cc_l, cc_ge, cc_le, cc_g,
    muldiv, misc,
    x_paddsb, x_paddsw, x_paddusb, x_paddusw, x_psubsb, x_psubsw, x_psubusb, x_psubusw, x_pavgb, x_pavgw,
    x_pmulhw, x_pmulhuw, x_pmaddwd, x_pmullw, x_pmuludq, x_paddb, x_paddq, x_psubd, x_pcmpgtb, x_pcmpeqw,
    x_pcmpgtd, x_pminub, x_pmaxsw, x_psadbw, x_pand, x_pandn, x_por, x_pxor, x_punpcklbw, x_punpckhwd,
    x_punpckldq, x_punpckhqdq, x_packsswb, x_packuswb, x_packssdw, x_pshufd, x_pshuflw, x_pshufhw, x_shufps,
    x_shufpd, x_psllw_i, x_psrad_i, x_psrlq_i, x_pslldq_i, x_psrldq_i,
    x_psllw_x, x_pslld_x, x_psllq_x, x_psrlw_x, x_psrld_x, x_psrlq_x, x_psraw_x, x_psrad_x,
    x_addps, x_subps, x_mulps, x_divps, x_minps, x_maxps, x_cmpps_lt, x_cmpps_neq, x_cvtps2dq, x_cvttps2dq,
    x_cvtps2pd, x_addss, x_divss, x_cvtss2sd, x_addpd, x_mulpd, x_divpd, x_minpd, x_maxsd, x_subsd,
    x_cvtpd2dq, x_cvttpd2dq, x_cvtpd2ps, x_cvtsd2ss, x_cmpsd_le, x_cvtdq2ps, x_cvtdq2pd,
    x_movsd_rr, x_movss_rr, x_movhlps, x_movlhps, x_movq_rr, x_unpcklps, x_pinsrw, x_togpr,
    fz_o, fz_no, fz_b, fz_ae, fz_e, fz_ne, fz_be, fz_a, fz_s, fz_ns, fz_p, fz_np, fz_l, fz_ge, fz_le, fz_g,
    fc_o, fc_no, fc_b, fc_ae, fc_e, fc_ne, fc_be, fc_a, fc_s, fc_ns, fc_p, fc_np, fc_l, fc_ge, fc_le, fc_g,
    fz_call, fz_index, fz_xindex, fz_3op, fz_3gap, fz_x3, fz_hoist, gpta_xmem, gpta_result_flags, gpta_xhigh, gpta_cf_flow,
    gpta_scaled_moves, gpta_high_comis, gpta_op3r, gpta_known_carry, gpta_narrow, gpta_fcj_trace, gpta_jresult_trace, gpta_immstores, gpta_x3high, gpta_step_trace, gpta_shift_counts, gpta_copy_shift_counts, gpta_unary_edges, gpta_alu_memory, gpta_xmm_lane_state, gpta_scalar_mem, gpta_unary_flag_inputs, gpta_xmap_transitions, gpta_cf_only, gpta_unary_trace, gpta_xload_pairs, gpta_ea_pairs, gpta_mov_test_branch, gpta_arith_float_branch, gpta_shift_xor, gpta_arith_float_trace, gpta_direct_load_pairs,
};

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (unsigned i = 0; i < sizeof kTests / sizeof kTests[0]; i++) kTests[i]();
    g_puts("difftest forms ");
    g_putu(sizeof kTests / sizeof kTests[0]);
    g_puts("\n");
    return 0;
}
