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

typedef void (*test_fn)(void);
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
    lea_q, lea_l, lea_w, xchg_q, xchg_b, xadd_q, xadd_w, bt_q, bts_l, btr_q, btc_w, bt_imm_q, bts_imm_l,
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
    fz_call, fz_index,
};

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (unsigned i = 0; i < sizeof kTests / sizeof kTests[0]; i++) kTests[i]();
    g_puts("difftest forms ");
    g_putu(sizeof kTests / sizeof kTests[0]);
    g_puts("\n");
    return 0;
}
