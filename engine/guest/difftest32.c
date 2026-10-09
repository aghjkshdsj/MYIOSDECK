// SPDX-License-Identifier: GPL-3.0-or-later
// Differential instruction test for i386 code (FXI32, docs/NO_JIT_WOW64.md stage 2), the
// counterpart of difftest.c: the integer forms 32-bit compilers emit in 8/16/32 bits, register
// and memory, and the encodings only i386 has (inc/dec 40-4F, pushad/popad, push/pop of
// segment registers, mov eax <-> [moffs32], enter-less frames with leave), each on random and
// edge inputs with random flags, one hash per form. engine/fxi/ci-test.sh requires FXI32's
// output to equal the native run's (an i686 static guest on the x86-64 runner) line for line.
// Flags a CPU leaves undefined are masked per form.

#include "guest_rt.h"

typedef unsigned int u32;
typedef unsigned long long u64;

#define N 300
#define ARITH 0x8d5u     // CF PF AF ZF SF OF
#define LOGIC 0x8c5u     // AF undefined after and/or/xor/test
#define CFOF 0x801u      // mul/imul: only CF and OF defined

static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (u32)(g_rng >> 16); }
static const u32 kEdge[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000,
    0xffffffff, 0xfffffffe, 0x55555555, 0xaaaaaaaa, 0x01234567, 0x89abcdef,
};
#define NEDGE (sizeof kEdge / sizeof kEdge[0])
static u32 val(void) {
    u32 r = rnd();
    switch (r % 6) {
    case 0: case 1: return kEdge[(r >> 8) % NEDGE];
    case 2: return (r >> 8) & 0xff;
    default: return rnd();
    }
}
static u32 flags_in(void) { return rnd() & ARITH; }

static u64 mix(u64 h, u64 v) { h ^= v; h *= 1099511628211ull; return h ^ (h >> 29); }
static void out(const char *name, u64 h) { g_puts(name); g_puts(" "); g_putu(h); g_puts("\n"); }

#define PRE "pushl %[fi]\n\tpopfl\n\t"
#define POST "\n\tpushfl\n\tpopl %[f]"
#define H0 14695981039346656037ull

// r = r OP b (reg, reg); 8-bit forms need a/b/c/d registers ("q")
#define RR(NAME, INSN, MASK)                                                                  \
    static void NAME(void) {                                                                  \
        u64 h = H0;                                                                           \
        for (int i = 0; i < N; i++) {                                                         \
            u32 r = val(), b = val(), fi = flags_in(), f;                                     \
            __asm__ volatile(PRE INSN POST : [r] "+q"(r), [f] "=&r"(f) : [b] "q"(b), [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(h, r), f & (MASK));                                                   \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// [m] = [m] OP r and r = r OP [m]
#define RM(NAME, INSN, MASK)                                                                  \
    static void NAME(void) {                                                                  \
        u64 h = H0;                                                                           \
        for (int i = 0; i < N; i++) {                                                         \
            u32 r = val(), m = val(), fi = flags_in(), f;                                     \
            __asm__ volatile(PRE INSN POST : [r] "+q"(r), [m] "+m"(m), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(mix(h, r), m), f & (MASK));                                           \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// r = OP r
#define R1(NAME, INSN, MASK)                                                                  \
    static void NAME(void) {                                                                  \
        u64 h = H0;                                                                           \
        for (int i = 0; i < N; i++) {                                                         \
            u32 r = val(), fi = flags_in(), f;                                                \
            __asm__ volatile(PRE INSN POST : [r] "+q"(r), [f] "=&r"(f) : [fi] "r"(fi) : "cc", "memory"); \
            h = mix(mix(h, r), f & (MASK));                                                   \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
// Shift/rotate by CL (count in ECX)
#define SH(NAME, INSN, CNT, MASKEXPR)                                                         \
    static void NAME(void) {                                                                  \
        u64 h = H0;                                                                           \
        for (int i = 0; i < N; i++) {                                                         \
            u32 r = val(), b = val(), fi = flags_in(), f, cnt = (CNT), c = cnt & 31;          \
            __asm__ volatile(PRE INSN POST : [r] "+a"(r), [f] "=&r"(f) : [b] "d"(b), [fi] "r"(fi), "c"(cnt) : "cc", "memory"); \
            h = mix(mix(h, r), f & (MASKEXPR));                                               \
        }                                                                                     \
        out(#NAME, h);                                                                        \
    }
#define SHIFT_MASK (c == 0 ? ARITH : (0xc5u | (c == 1 ? 0x800u : 0)))
#define ROT_MASK (c == 0 ? ARITH : (0xd5u | (c == 1 ? 0x800u : 0)))

#define ALU3(OP, MASK)                                                                        \
    RR(OP##_l, #OP "l %[b], %[r]", MASK) RR(OP##_w, #OP "w %w[b], %w[r]", MASK)               \
    RR(OP##_b, #OP "b %b[b], %b[r]", MASK) RM(OP##_mr_l, #OP "l %[r], %[m]", MASK)            \
    RM(OP##_rm_l, #OP "l %[m], %[r]", MASK) RM(OP##_mr_b, #OP "b %b[r], %[m]", MASK)          \
    RM(OP##_rm_w, #OP "w %[m], %w[r]", MASK)
ALU3(add, ARITH) ALU3(adc, ARITH) ALU3(sub, ARITH) ALU3(sbb, ARITH) ALU3(cmp, ARITH)
ALU3(and, LOGIC) ALU3(or, LOGIC) ALU3(xor, LOGIC)
RR(test_l, "testl %[b], %[r]", LOGIC) RR(test_w, "testw %w[b], %w[r]", LOGIC) RR(test_b, "testb %b[b], %b[r]", LOGIC)
RR(add_imm8_l, "addl $-5, %[r]", ARITH) RR(sub_imm32_l, "subl $0x12345678, %[r]", ARITH)
RR(and_imm_l, "andl $0x7fffff00, %[r]", LOGIC) RR(cmp_imm8_w, "cmpw $-1, %w[r]", ARITH)
RR(adc_imm_b, "adcb $0x7f, %b[r]", ARITH) RR(sbb_imm_l, "sbbl $1, %[r]", ARITH)
RM(add_mi_l, "addl $0x11111111, %[m]", ARITH) RM(cmp_mi8_l, "cmpl $7, %[m]", ARITH) RM(or_mi_b, "orb $0x81, %[m]", LOGIC)

// inc/dec r32 are the one-byte 40-4F encodings in i386
R1(inc_l, "incl %[r]", ARITH) R1(inc_w, "incw %w[r]", ARITH) R1(inc_b, "incb %b[r]", ARITH)
R1(dec_l, "decl %[r]", ARITH) R1(dec_w, "decw %w[r]", ARITH) R1(dec_b, "decb %b[r]", ARITH)
RM(inc_m_l, "incl %[m]", ARITH) RM(dec_m_w, "decw %[m]", ARITH)
R1(neg_l, "negl %[r]", ARITH) R1(neg_w, "negw %w[r]", ARITH) R1(neg_b, "negb %b[r]", ARITH)
R1(not_l, "notl %[r]", ARITH) R1(not_b, "notb %b[r]", ARITH)
R1(bswap_l, "bswapl %[r]", ARITH)
R1(shl1_l, "shll $1, %[r]", 0xcd5u & ~0x10u) R1(sar1_l, "sarl $1, %[r]", 0xcd5u & ~0x10u)
R1(shr1_b, "shrb $1, %b[r]", 0xcd5u & ~0x10u) R1(rol1_l, "roll $1, %[r]", 0x8d5u) R1(ror1_w, "rorw $1, %w[r]", 0x8d5u)
R1(rcl1_l, "rcll $1, %[r]", 0x8d5u) R1(rcr1_b, "rcrb $1, %b[r]", 0x8d5u)
R1(shl_imm_l, "shll $13, %[r]", 0xc5u) R1(shr_imm_l, "shrl $31, %[r]", 0xc5u) R1(sar_imm_w, "sarw $7, %w[r]", 0xc5u)
R1(rol_imm_b, "rolb $3, %b[r]", 0xd5u) R1(ror_imm_l, "rorl $20, %[r]", 0xd5u)

SH(shl_l, "shll %%cl, %[r]", rnd() & 0x3f, SHIFT_MASK) SH(shl_w, "shlw %%cl, %w[r]", rnd() % 16, SHIFT_MASK)
SH(shl_b, "shlb %%cl, %b[r]", rnd() % 8, SHIFT_MASK)
SH(shr_l, "shrl %%cl, %[r]", rnd() & 0x3f, SHIFT_MASK) SH(shr_w, "shrw %%cl, %w[r]", rnd() % 16, SHIFT_MASK)
SH(shr_b, "shrb %%cl, %b[r]", rnd() % 8, SHIFT_MASK)
SH(sar_l, "sarl %%cl, %[r]", rnd() & 0x3f, SHIFT_MASK) SH(sar_w, "sarw %%cl, %w[r]", rnd() % 16, SHIFT_MASK)
SH(sar_b, "sarb %%cl, %b[r]", rnd() % 8, SHIFT_MASK)
SH(rol_l, "roll %%cl, %[r]", rnd() & 0x3f, ROT_MASK) SH(rol_w, "rolw %%cl, %w[r]", rnd() & 0x3f, ROT_MASK)
SH(ror_l, "rorl %%cl, %[r]", rnd() & 0x3f, ROT_MASK) SH(ror_b, "rorb %%cl, %b[r]", rnd() & 0x3f, ROT_MASK)
SH(rcl_l, "rcll %%cl, %[r]", rnd() & 0x3f, ROT_MASK) SH(rcr_l, "rcrl %%cl, %[r]", rnd() & 0x3f, ROT_MASK)
SH(shld_l, "shldl %%cl, %[b], %[r]", rnd() & 0x3f, SHIFT_MASK) SH(shld_w, "shldw %%cl, %w[b], %w[r]", rnd() % 16, SHIFT_MASK)
SH(shrd_l, "shrdl %%cl, %[b], %[r]", rnd() & 0x3f, SHIFT_MASK) SH(shrd_w, "shrdw %%cl, %w[b], %w[r]", rnd() % 16, SHIFT_MASK)

RR(imul2_l, "imull %[b], %[r]", CFOF) RR(imul2_w, "imulw %w[b], %w[r]", CFOF)
RR(imul3_l, "imull $-3, %[b], %[r]", CFOF) RR(imul3_w, "imulw $1000, %w[b], %w[r]", CFOF)
RR(movsbl, "movsbl %b[b], %[r]", ARITH) RR(movswl, "movswl %w[b], %[r]", ARITH)
RR(movzbl, "movzbl %b[b], %[r]", ARITH) RR(movzwl, "movzwl %w[b], %[r]", ARITH)
RR(movsbw, "movsbw %b[b], %w[r]", ARITH) RR(movw_keep, "movw %w[b], %w[r]", ARITH) RR(movb_keep, "movb %b[b], %b[r]", ARITH)
RR(lea_l, "leal 0x1234(%[r],%[b],4), %[r]", ARITH) RR(lea_w, "leaw -8(%[r],%[b],2), %w[r]", ARITH)
RR(lea_abs, "leal 0x7ffffff0(%[b]), %[r]", ARITH)
RR(xchg_l, "xchgl %[b], %[r]", ARITH) RR(xchg_b, "xchgb %b[b], %b[r]", ARITH) RR(xadd_l, "xaddl %[b], %[r]", ARITH)
RR(bt_l, "btl %[b], %[r]", 0x1u) RR(bts_l, "btsl %[b], %[r]", 0x1u) RR(btr_l, "btrl %[b], %[r]", 0x1u) RR(btc_w, "btcw %w[b], %w[r]", 0x1u)
R1(bt_imm_l, "btl $17, %[r]", 0x1u) R1(bts_imm_l, "btsl $31, %[r]", 0x1u)
RR(bsf_l, "orl $1, %[b]\n\tbsfl %[b], %[r]", 0x40u) RR(bsr_l, "orl $1, %[b]\n\tbsrl %[b], %[r]", 0x40u)
RR(cmov_l, "cmpl %[b], %[r]\n\tcmovll %[b], %[r]", ARITH) RR(cmov_w, "testw %w[b], %w[r]\n\tcmovsw %w[b], %w[r]", LOGIC)
R1(cwde, "movl %[r], %%eax\n\tcwtl\n\tmovl %%eax, %[r]", ARITH)

// setcc after a compare, for every condition
#define CC(NAME, J)                                                                           \
    static void cc_##NAME(void) {                                                             \
        u64 h = H0;                                                                           \
        for (int i = 0; i < N; i++) {                                                         \
            u32 a = val(), b = val(), r = 0;                                                  \
            __asm__ volatile("cmpl %[b], %[a]\n\tset" #J " %b[r]" : [r] "+q"(r) : [a] "r"(a), [b] "r"(b) : "cc"); \
            h = mix(h, r);                                                                    \
        }                                                                                     \
        out("cc_" #NAME, h);                                                                  \
    }
CC(o, o) CC(no, no) CC(b, b) CC(ae, ae) CC(e, e) CC(ne, ne) CC(be, be) CC(a, a)
CC(s, s) CC(ns, ns) CC(p, p) CC(np, np) CC(l, l) CC(ge, ge) CC(le, le) CC(g, g)

// One-operand mul/div (EDX:EAX), 8/16/32 bits; divisors kept nonzero, quotients in range.
static void muldiv(void) {
    u64 h = H0;
    for (int i = 0; i < N; i++) {
        u32 a = val(), b = val() | 1, d = 0, f, fi = flags_in();
        u32 eax = a, edx = 0;
        __asm__ volatile(PRE "mull %[b]" POST : "+a"(eax), "+d"(edx), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, eax), edx), f & CFOF);
        eax = a; edx = 0;
        __asm__ volatile(PRE "imull %[b]" POST : "+a"(eax), "+d"(edx), [f] "=&r"(f) : [b] "r"(b), [fi] "r"(fi) : "cc");
        h = mix(mix(mix(h, eax), edx), f & CFOF);
        eax = a; edx = a % b;   // edx < b: the quotient fits
        __asm__ volatile("divl %[b]" : "+a"(eax), "+d"(edx) : [b] "r"(b) : "cc");
        h = mix(mix(h, eax), edx);
        eax = a; edx = (a & 0x80000000u) ? 0xffffffffu : 0;
        if (b != 0xffffffffu) {
            __asm__ volatile("idivl %[b]" : "+a"(eax), "+d"(edx) : [b] "r"(b) : "cc");
            h = mix(mix(h, eax), edx);
        }
        eax = a & 0xffff; edx = 0;
        __asm__ volatile("mulw %w[b]" : "+a"(eax), "+d"(edx) : [b] "r"(b) : "cc");
        h = mix(mix(h, eax), edx);
        eax = a & 0x7f7f; d = (b & 0x7f) | 0x80;   // AH < divisor: the quotient fits in AL
        __asm__ volatile("divb %b[d]" : "+a"(eax) : [d] "q"(d) : "cc");
        h = mix(h, eax);
        eax = a; edx = b;
        __asm__ volatile("cdq" : "+a"(eax), "=d"(edx));
        h = mix(mix(h, eax), edx);
        eax = a;
        __asm__ volatile("cbw\n\tcwd" : "+a"(eax), "=d"(edx));
        h = mix(mix(h, eax), edx & 0xffff);
    }
    out("muldiv", h);
}

// Stack: push/pop of registers, immediates, memory; pushad/popad; pushfd/popfd; leave; the
// segment register pushes (selectors 0x23/0x2b, the same on Linux and WoW64).
volatile u32 g_scratch __attribute__((used));   // written by absolute address (popal restores every register)
static void stack_forms(void) {
    u64 h = H0;
    for (int i = 0; i < N; i++) {
        u32 a = val(), b = val(), m = val(), r1, r2, r3, r4;
        __asm__ volatile("pushl %[a]\n\tpushl $-7\n\tpushl $0x12345678\n\tpushl %[m]\n\t"
                         "popl %[r1]\n\tpopl %[r2]\n\tpopl %[r3]\n\tpopl %[r4]"
                         : [r1] "=&r"(r1), [r2] "=&r"(r2), [r3] "=&r"(r3), [r4] "=&r"(r4) : [a] "r"(a), [m] "m"(m) : "memory");
        h = mix(mix(mix(mix(h, r1), r2), r3), r4);
        u32 sa = a, sb = b;
        __asm__ volatile("pushal\n\tmovl $1, %%eax\n\tmovl $2, %%ecx\n\tmovl $3, %%edx\n\tmovl $5, %%esi\n\tmovl $6, %%edi\n\tpopal"
                         : "+a"(sa), "+c"(sb) : : "edx", "esi", "edi", "memory");
        h = mix(mix(h, sa), sb);
        u32 s0;
        __asm__ volatile("movl %%esp, %[s0]\n\tpushal\n\tmovl 12(%%esp), %%eax\n\tmovl %%eax, g_scratch\n\tpopal"
                         : [s0] "=&r"(s0) : : "memory");
        h = mix(h, s0 - g_scratch);   // the ESP slot pushad stored: ESP before the push
        u32 f;
        __asm__ volatile("pushl %[b]\n\tpopfl\n\tpushfl\n\tpopl %[f]" : [f] "=r"(f) : [b] "r"(b & 0x8d5u) : "cc");
        h = mix(h, f & 0x8d5u);
        u32 pm = 0;
        __asm__ volatile("pushl %[a]\n\tpopl %[pm]" : [pm] "=m"(pm) : [a] "r"(a) : "memory");
        h = mix(h, pm);
        u32 l;
        __asm__ volatile("pushl %%ebp\n\tmovl %%esp, %%ebp\n\tsubl $64, %%esp\n\tmovl %[a], -4(%%ebp)\n\t"
                         "movl -4(%%ebp), %[l]\n\tleave" : [l] "=&d"(l) : [a] "c"(a) : "memory");   // never in ebp
        h = mix(h, l);
        u32 sel;
        __asm__ volatile("pushl %%cs\n\tpopl %[s]" : [s] "=r"(sel)); h = mix(h, sel & 0xffff);
        __asm__ volatile("pushl %%ds\n\tpopl %[s]" : [s] "=r"(sel)); h = mix(h, sel & 0xffff);
        __asm__ volatile("pushl %%es\n\tpopl %%es" ::: "memory");
        __asm__ volatile("movl %%ss, %[s]" : [s] "=r"(sel)); h = mix(h, sel & 0xffff);
    }
    out("stack", h);
}

// mov eax/al <-> [moffs32] (a1/a3/a0/a2): what 32-bit compilers emit for globals.
volatile u32 g_moffs __attribute__((used));
volatile unsigned char g_moffs8 __attribute__((used));
static void moffs(void) {
    u64 h = H0;
    for (int i = 0; i < N; i++) {
        u32 a = val(), r;
        __asm__ volatile("movl %[a], %%eax\n\tmovl %%eax, g_moffs\n\taddl $3, %%eax\n\tmovl g_moffs, %%eax"
                         : "=a"(r) : [a] "r"(a) : "memory");
        h = mix(h, r);
        __asm__ volatile("movb %b[a], %%al\n\tmovb %%al, g_moffs8\n\tmovl $0, %%eax\n\tmovb g_moffs8, %%al"
                         : "=a"(r) : [a] "q"(a) : "memory");
        h = mix(h, r);
    }
    out("moffs", h);
}

// String instructions: rep movs/stos (byte, dword), cmps/scas with repe/repne, lods, std/cld.
static unsigned char sbuf[512], dbuf[512];
static void strings(void) {
    u64 h = H0;
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < 512; k++) { sbuf[k] = (unsigned char)rnd(); dbuf[k] = (unsigned char)rnd(); }
        u32 n = rnd() % 100, o = rnd() % 64;
        unsigned char *s = sbuf + o, *d = dbuf + (rnd() % 64);
        u32 cnt = n;
        __asm__ volatile("cld\n\trep movsb" : "+S"(s), "+D"(d), "+c"(cnt) : : "memory");
        h = mix(mix(h, (u32)(s - sbuf)), (u32)(d - dbuf));
        cnt = n / 4; d = dbuf + 100;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(cnt) : "a"(val()) : "memory");
        h = mix(h, (u32)(d - dbuf));
        s = sbuf; d = dbuf; cnt = 64; u32 f;
        __asm__ volatile("repe cmpsb\n\tpushfl\n\tpopl %[f]" : "+S"(s), "+D"(d), "+c"(cnt), [f] "=r"(f) : : "memory", "cc");
        h = mix(mix(mix(h, cnt), (u32)(s - sbuf)), f & ARITH);
        d = dbuf; cnt = 200;
        __asm__ volatile("repne scasb\n\tpushfl\n\tpopl %[f]" : "+D"(d), "+c"(cnt), [f] "=r"(f) : "a"(sbuf[7]) : "memory", "cc");
        h = mix(mix(h, cnt), f & ARITH);
        s = sbuf + 400; u32 v;
        __asm__ volatile("std\n\tlodsl\n\tlodsl\n\tcld" : "+S"(s), "=a"(v) : : "memory");
        h = mix(mix(h, v), (u32)(s - sbuf));
        s = sbuf + 300; d = dbuf + 310; cnt = 9;
        __asm__ volatile("std\n\trep movsl\n\tcld" : "+S"(s), "+D"(d), "+c"(cnt) : : "memory");
        h = mix(mix(h, (u32)(s - sbuf)), (u32)(d - dbuf));
        for (int k = 0; k < 512; k++) h = mix(h, dbuf[k]);
    }
    out("strings", h);
}

// Control flow: indirect calls through registers and memory, a jump table, ret imm16.
static u32 add3(u32 a) { return a + 3; }
static u32 xor5(u32 a) { return a ^ 5; }
static u32 (*volatile g_fns[2])(u32) = { add3, xor5 };
static void control(void) {
    u64 h = H0;
    for (int i = 0; i < N; i++) {
        u32 a = val(), r;
        r = g_fns[a & 1](a);
        h = mix(h, r);
        __asm__ volatile("pushl %[a]\n\tcall 1f\n\tjmp 2f\n1:\tmovl 4(%%esp), %[r]\n\taddl $1, %[r]\n\tret $4\n2:"
                         : [r] "=&r"(r) : [a] "r"(a) : "memory");
        h = mix(h, r);
        switch (a % 7) {
        case 0: r = a * 3; break; case 1: r = a ^ 0x55; break; case 2: r = a + 99; break;
        case 3: r = a >> 3; break; case 4: r = ~a; break; case 5: r = a | 0x100; break; default: r = 0;
        }
        h = mix(h, r);
    }
    out("control", h);
}

// SSE2 through 32-bit addressing: integer and double forms MSVC's /arch:SSE2 code uses.
typedef long long v2 __attribute__((vector_size(16)));
static v2 xbuf[8];
static void sse(void) {
    u64 h = H0;
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < 8; k++) xbuf[k] = (v2){ (long long)val() << 32 | val(), (long long)val() << 32 | val() };
        v2 a, b;
        u32 k = (rnd() & 3) * 16;
        __asm__ volatile("movdqu (%[p],%[k]), %[a]\n\tmovdqa 64(%[p]), %[b]\n\tpaddd %[b], %[a]\n\tpxor 16(%[p],%[k]), %[a]\n\t"
                         "movdqu %[a], 96(%[p])" : [a] "=&x"(a), [b] "=&x"(b) : [p] "r"(xbuf), [k] "r"(k) : "memory");
        h = mix(mix(h, (u64)a[0]), (u64)a[1]);
        double x = (double)(int)val() / 7.0, y;
        int t;
        __asm__ volatile("cvtsi2sdl %[i], %[y]\n\taddsd %[x], %[y]\n\tmulsd %[x], %[y]\n\tcvttsd2si %[y], %[t]"
                         : [y] "=&x"(y), [t] "=r"(t) : [i] "r"(val() & 0xffff), [x] "x"(x));
        h = mix(h, (u32)t);
        u32 d;
        __asm__ volatile("movd %[v], %[a]\n\tpshufd $0, %[a], %[a]\n\tpsrlq $7, %[a]\n\tmovd %[a], %[d]"
                         : [a] "=&x"(a), [d] "=r"(d) : [v] "r"(val()));
        h = mix(h, d);
        for (int q = 0; q < 8; q++) h = mix(mix(h, (u64)xbuf[q][0]), (u64)xbuf[q][1]);
    }
    out("sse", h);
}

typedef void (*test_fn)(void);
#define ALU_LIST(OP) OP##_l, OP##_w, OP##_b, OP##_mr_l, OP##_rm_l, OP##_mr_b, OP##_rm_w,
static const test_fn kTests[] = {
    ALU_LIST(add) ALU_LIST(adc) ALU_LIST(sub) ALU_LIST(sbb) ALU_LIST(cmp) ALU_LIST(and) ALU_LIST(or) ALU_LIST(xor)
    test_l, test_w, test_b, add_imm8_l, sub_imm32_l, and_imm_l, cmp_imm8_w, adc_imm_b, sbb_imm_l, add_mi_l, cmp_mi8_l, or_mi_b,
    inc_l, inc_w, inc_b, dec_l, dec_w, dec_b, inc_m_l, dec_m_w, neg_l, neg_w, neg_b, not_l, not_b, bswap_l,
    shl1_l, sar1_l, shr1_b, rol1_l, ror1_w, rcl1_l, rcr1_b, shl_imm_l, shr_imm_l, sar_imm_w, rol_imm_b, ror_imm_l,
    shl_l, shl_w, shl_b, shr_l, shr_w, shr_b, sar_l, sar_w, sar_b, rol_l, rol_w, ror_l, ror_b, rcl_l, rcr_l,
    shld_l, shld_w, shrd_l, shrd_w,
    imul2_l, imul2_w, imul3_l, imul3_w, movsbl, movswl, movzbl, movzwl, movsbw, movw_keep, movb_keep,
    lea_l, lea_w, lea_abs, xchg_l, xchg_b, xadd_l, bt_l, bts_l, btr_l, btc_w, bt_imm_l, bts_imm_l, bsf_l, bsr_l,
    cmov_l, cmov_w, cwde,
    cc_o, cc_no, cc_b, cc_ae, cc_e, cc_ne, cc_be, cc_a, cc_s, cc_ns, cc_p, cc_np, cc_l, cc_ge, cc_le, cc_g,
    muldiv, stack_forms, moffs, strings, control, sse,
};

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (unsigned i = 0; i < sizeof kTests / sizeof kTests[0]; i++) kTests[i]();
    g_puts("difftest32 forms ");
    g_putu(sizeof kTests / sizeof kTests[0]);
    g_puts("\n");
    return 0;
}
