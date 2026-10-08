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

typedef void (*test_fn)(void);
#define ALU_LIST(OP) OP##_q, OP##_l, OP##_w, OP##_b, OP##_mr_q, OP##_rm_l, OP##_mr_b,
static const test_fn kTests[] = {
    ALU_LIST(add) ALU_LIST(adc) ALU_LIST(sub) ALU_LIST(sbb) ALU_LIST(cmp) ALU_LIST(and) ALU_LIST(or) ALU_LIST(xor)
    test_q, test_l, test_b, add_imm8_q, sub_imm32_l, and_imm_q, cmp_imm8_w, adc_imm_b, sbb_imm_q,
    inc_q, inc_l, inc_w, inc_b, dec_q, dec_l, dec_b, neg_q, neg_l, neg_w, neg_b, not_q, not_l, not_b,
    bswap_q, bswap_l, shl1_q, sar1_l, shr1_b, rol1_q, ror1_w, rcl1_l, rcr1_b,
    shl_imm_q, shr_imm_l, sar_imm_w, rol_imm_b, ror_imm_q,
    shl_q, shl_l, shl_w, shl_b, shr_q, shr_l, shr_w, shr_b, sar_q, sar_l, sar_w, sar_b,
    rol_q, rol_l, rol_w, rol_b, ror_q, ror_l, ror_w, ror_b, rcl_q, rcl_l, rcl_w, rcl_b, rcr_q, rcr_l, rcr_w, rcr_b,
    shld_q, shld_l, shld_w, shrd_q, shrd_l, shrd_w,
    imul2_q, imul2_l, imul2_w, imul3_q, imul3_l, imul3_w,
    movsbq, movswq, movslq, movsbl, movsbw, movzbl, movzwl, movzbw, movl_zext, movw_keep, movb_keep,
    lea_q, lea_l, lea_w, xchg_q, xchg_b, xadd_q, xadd_w, bt_q, bts_l, btr_q, btc_w, bt_imm_q, bts_imm_l,
    bsf_q, bsr_q,
    cc_o, cc_no, cc_b, cc_ae, cc_e, cc_ne, cc_be, cc_a, cc_s, cc_ns, cc_p, cc_np, cc_l, cc_ge, cc_le, cc_g,
    muldiv, misc,
};

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (unsigned i = 0; i < sizeof kTests / sizeof kTests[0]; i++) kTests[i]();
    g_puts("difftest forms ");
    g_putu(sizeof kTests / sizeof kTests[0]);
    g_puts("\n");
    return 0;
}
