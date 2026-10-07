// SPDX-License-Identifier: GPL-3.0-or-later
// x86-64 atomic and bit-test instructions (what Windows code uses for Interlocked*, SRW locks,
// critical sections and refcounts): every result and the arithmetic flags are printed, and
// engine/fxi/ci-test.sh requires FXI's output to equal the native run's byte for byte.
// Built with -mno-red-zone (pushfq inside the asm would clobber the red zone).

#include "guest_rt.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define FLAGS_MASK 0x8c5ull   // CF PF ZF SF OF (AF is undefined after and/or/xor, CPUs differ)
#define GET_FLAGS "\n\tpushfq\n\tpopq %[f]"

static u64 g_hash = 1469598103934665603ull;

static void report(const char *name, u64 a, u64 b, u64 flags) {
    g_puts(name); g_puts(": "); g_putu(a); g_puts(" "); g_putu(b);
    g_puts(" flags "); g_putu(flags & FLAGS_MASK); g_puts("\n");
    u64 v[3] = { a, b, flags & FLAGS_MASK };
    for (int i = 0; i < 3; i++) { g_hash ^= v[i]; g_hash *= 1099511628211ull; }
}

static u64 mem[4] __attribute__((aligned(16)));
static u8 raw[64] __attribute__((aligned(16)));

static void t_cmpxchg(void) {
    u64 f, rax;
    // 64-bit: success, then failure
    mem[0] = 5; rax = 5;
    __asm__ volatile("lock cmpxchgq %[s], (%[p])" GET_FLAGS : "+a"(rax), [f] "=&r"(f) : [s] "r"(9ull), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg64 ok", mem[0], rax, f);
    rax = 7;
    __asm__ volatile("lock cmpxchgq %[s], (%[p])" GET_FLAGS : "+a"(rax), [f] "=&r"(f) : [s] "r"(1ull), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg64 fail", mem[0], rax, f);
    // 32-bit failure zero-extends RAX
    mem[0] = 0xffffffff00000003ull; rax = 0x1234567800000004ull;
    __asm__ volatile("lock cmpxchgl %[s], (%[p])" GET_FLAGS : "+a"(rax), [f] "=&r"(f) : [s] "r"(1u), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg32 fail", mem[0], rax, f);
    rax = 3;
    __asm__ volatile("lock cmpxchgl %[s], (%[p])" GET_FLAGS : "+a"(rax), [f] "=&r"(f) : [s] "r"(0x80000000u), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg32 ok", mem[0], rax, f);
    // 16 and 8 bit
    mem[0] = 0x1111222233334444ull; rax = 0x4444;
    __asm__ volatile("lock cmpxchgw %w[s], (%[p])" GET_FLAGS : "+a"(rax), [f] "=&r"(f) : [s] "r"(0xbeefu), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg16 ok", mem[0], rax, f);
    rax = 0x10;
    __asm__ volatile("lock cmpxchgb %b[s], (%[p])" GET_FLAGS : "+a"(rax), [f] "=&r"(f) : [s] "q"(0x77u), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg8 fail", mem[0], rax, f);
    // register form, no lock
    u64 d = 42; rax = 42;
    __asm__ volatile("cmpxchgq %[s], %[d]" GET_FLAGS : "+a"(rax), [d] "+r"(d), [f] "=&r"(f) : [s] "r"(99ull) : "cc");
    report("cmpxchg reg", d, rax, f);
}

static void t_xadd_xchg(void) {
    u64 f, v;
    mem[0] = 0x7fffffffffffffffull; v = 1;
    __asm__ volatile("lock xaddq %[v], (%[p])" GET_FLAGS : [v] "+r"(v), [f] "=&r"(f) : [p] "r"(mem) : "memory", "cc");
    report("xadd64", mem[0], v, f);
    mem[0] = 0xfffffffeull; v = 3;
    __asm__ volatile("lock xaddl %k[v], (%[p])" GET_FLAGS : [v] "+r"(v), [f] "=&r"(f) : [p] "r"(mem) : "memory", "cc");
    report("xadd32", mem[0], v, f);
    mem[0] = 0xffull; v = 0x101;
    __asm__ volatile("lock xaddb %b[v], (%[p])" GET_FLAGS : [v] "+q"(v), [f] "=&r"(f) : [p] "r"(mem) : "memory", "cc");
    report("xadd8", mem[0], v, f);
    u64 d = 10; v = 20;
    __asm__ volatile("xaddq %[v], %[d]" GET_FLAGS : [v] "+r"(v), [d] "+r"(d), [f] "=&r"(f) : : "cc");
    report("xadd reg", d, v, f);
    mem[0] = 111; v = 222;
    __asm__ volatile("xchgq %[v], (%[p])" : [v] "+r"(v) : [p] "r"(mem) : "memory");
    report("xchg64", mem[0], v, 0);
    mem[0] = 0xaaaaaaaabbbbbbbbull; v = 0x55;
    __asm__ volatile("xchgb %b[v], (%[p])" : [v] "+q"(v) : [p] "r"(mem) : "memory");
    report("xchg8", mem[0], v, 0);
}

#define LOCK_ALU(NAME, INSN, START, SRC)                                                       \
    do {                                                                                       \
        u64 f; mem[0] = (START);                                                               \
        __asm__ volatile("stc\n\t" INSN " %[s], (%[p])" GET_FLAGS : [f] "=&r"(f) : [s] "r"((u64)(SRC)), [p] "r"(mem) : "memory", "cc"); \
        report(NAME, mem[0], 0, f);                                                            \
    } while (0)
#define LOCK_ALU_I(NAME, INSN, START)                                                          \
    do {                                                                                       \
        u64 f; mem[0] = (START);                                                               \
        __asm__ volatile("clc\n\t" INSN GET_FLAGS : [f] "=&r"(f) : [p] "r"(mem) : "memory", "cc"); \
        report(NAME, mem[0], 0, f);                                                            \
    } while (0)

static void t_lock_alu(void) {
    LOCK_ALU("lock add64", "lock addq", 0xffffffffffffffffull, 1);
    LOCK_ALU("lock sub64", "lock subq", 5, 7);
    LOCK_ALU("lock and64", "lock andq", 0xf0f0, 0x0ff0);
    LOCK_ALU("lock or64", "lock orq", 0x8000000000000000ull, 1);
    LOCK_ALU("lock xor64", "lock xorq", 0x1234, 0x1234);
    LOCK_ALU("lock adc64", "lock adcq", 10, 20);
    LOCK_ALU("lock sbb64", "lock sbbq", 10, 20);
    LOCK_ALU_I("lock add32 imm", "lock addl $0x7fffffff, (%[p])", 1);
    LOCK_ALU_I("lock sub8 imm", "lock subb $1, (%[p])", 0x100);
    LOCK_ALU_I("lock or16 imm", "lock orw $0x8000, (%[p])", 0x12340001);
    LOCK_ALU_I("lock inc64", "lock incq (%[p])", 0x7fffffffffffffffull);
    LOCK_ALU_I("lock dec32", "lock decl (%[p])", 0x100000000ull);
    LOCK_ALU_I("lock neg64", "lock negq (%[p])", 5);
    LOCK_ALU_I("lock not16", "lock notw (%[p])", 0xff00ff00ull);
    // misaligned (x86 "split lock"): FXI's fallback path
    u64 f;
    for (int i = 0; i < 16; i++) raw[i] = (u8)i;
    __asm__ volatile("lock addl %[s], 3(%[p])" GET_FLAGS : [f] "=&r"(f) : [s] "r"(0x01010101u), [p] "r"(raw) : "memory", "cc");
    u64 lo, hi; __builtin_memcpy(&lo, raw, 8); __builtin_memcpy(&hi, raw + 8, 8);
    report("lock add32 misaligned", lo, hi, f);
}

static void t_cmpxchg8b16b(void) {
    u64 f, a, d;
    mem[0] = 0x0000000200000001ull;
    a = 1; d = 2;
    __asm__ volatile("lock cmpxchg8b (%[p])" GET_FLAGS : "+a"(a), "+d"(d), [f] "=&r"(f) : "b"(0xaull), "c"(0xbull), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg8b ok", mem[0], a | d << 32, f);
    a = 1; d = 2;
    __asm__ volatile("lock cmpxchg8b (%[p])" GET_FLAGS : "+a"(a), "+d"(d), [f] "=&r"(f) : "b"(0xcull), "c"(0xdull), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg8b fail", mem[0], a | d << 32, f);
    mem[0] = 111; mem[1] = 222;
    a = 111; d = 222;
    __asm__ volatile("lock cmpxchg16b (%[p])" GET_FLAGS : "+a"(a), "+d"(d), [f] "=&r"(f) : "b"(333ull), "c"(444ull), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg16b ok", mem[0], mem[1], f);
    report("cmpxchg16b ok rdx:rax", a, d, 0);
    a = 1; d = 2;
    __asm__ volatile("lock cmpxchg16b (%[p])" GET_FLAGS : "+a"(a), "+d"(d), [f] "=&r"(f) : "b"(5ull), "c"(6ull), [p] "r"(mem) : "memory", "cc");
    report("cmpxchg16b fail", a, d, f);
}

static void t_bitops(void) {
    u64 f, cf;
    mem[0] = 0; mem[1] = 0; mem[2] = 0x8000000000000000ull;
    __asm__ volatile("lock btsq $63, (%[p])\n\tsetc %b[c]" : [c] "=&q"(cf) : [p] "r"(mem) : "memory", "cc");
    report("lock bts imm", mem[0], cf & 1, 0);
    __asm__ volatile("lock btsq %[n], (%[p])\n\tsetc %b[c]" : [c] "=&q"(cf) : [n] "r"(70ull), [p] "r"(mem) : "memory", "cc");
    report("lock bts reg +70", mem[1], cf & 1, 0);
    __asm__ volatile("lock btrq %[n], (%[p])\n\tsetc %b[c]" : [c] "=&q"(cf) : [n] "r"(-1ll), [p] "r"(mem + 3) : "memory", "cc");
    report("lock btr reg -1", mem[2], cf & 1, 0);
    __asm__ volatile("lock btcl $5, (%[p])\n\tsetc %b[c]" : [c] "=&q"(cf) : [p] "r"(mem + 1) : "memory", "cc");
    report("lock btc imm", mem[1], cf & 1, 0);
    __asm__ volatile("btq %[n], (%[p])\n\tsetc %b[c]" : [c] "=&q"(cf) : [n] "r"(64 + 6ull), [p] "r"(mem) : "memory", "cc");
    report("bt mem reg", mem[1], cf & 1, 0);
    u64 r = 0x10;
    __asm__ volatile("btsq $3, %[r]\n\tbtrq $4, %[r]\n\tbtcq %[n], %[r]\n\tsetc %b[c]" : [r] "+r"(r), [c] "=&q"(cf) : [n] "r"(64 + 3ull) : "cc");
    report("bts/btr/btc reg", r, cf & 1, 0);
    u32 w = 1;
    __asm__ volatile("btl $0, %[w]" GET_FLAGS : [f] "=&r"(f) : [w] "r"(w) : "cc");
    report("bt reg imm", w, (f & 1), 0);
}

// Double-precision shifts (shld/shrd): not atomics, but the same "real Windows code" set.
static void t_shxd(void) {
    u64 f, r, m;
    r = 0x8000000000000001ull;
    __asm__ volatile("shldq $4, %[s], %[r]" GET_FLAGS : [r] "+r"(r), [f] "=&r"(f) : [s] "r"(0xf000000000000000ull) : "cc");
    report("shld64 imm", r, 0, f & ~0x800ull);   // OF only defined for a count of 1
    r = 0x12345678;
    __asm__ volatile("shrdl %%cl, %k[s], %k[r]" GET_FLAGS : [r] "+r"(r), [f] "=&r"(f) : [s] "r"(0xabcdu), "c"(8) : "cc");
    report("shrd32 cl", r, 0, f & ~0x800ull);
    r = 0x8000000000000000ull;
    __asm__ volatile("shldq $1, %[s], %[r]" GET_FLAGS : [r] "+r"(r), [f] "=&r"(f) : [s] "r"(0ull) : "cc");
    report("shld64 1", r, 0, f);
    m = 0x00000000ffff0000ull;
    __asm__ volatile("shrdq $16, %[s], (%[p])" GET_FLAGS : [f] "=&r"(f) : [s] "r"(0x1111ull), [p] "r"(&m) : "memory", "cc");
    report("shrd64 mem", m, 0, f & ~0x800ull);
}

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    t_cmpxchg();
    t_xadd_xchg();
    t_lock_alu();
    t_cmpxchg8b16b();
    t_bitops();
    t_shxd();
    g_puts("atomics hash ");
    g_putu(g_hash);
    g_puts("\n");
    return 0;
}
