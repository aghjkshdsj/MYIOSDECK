// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic (CI only, not part of FXR): what one threaded dispatch costs on this CPU. A ring of
// uops, each `ldr p, [u, #80]!; br p` into one of NH distinct preserve_none handlers that do one
// add, like FXR's handlers. Run under perf stat; the CI step divides by the dispatch count.
//   cc -O2 [-DALIGN=n] [-DFAT] dispatch_probe.c && ./a.out <ind|chain|mix|st|ld|cp> <ring> <handlers>
// ALIGN: each handler aligned to n bytes (spreads them in memory as FXR's are spread).
// FAT: FXR's full handler signature (23 integer and 8 vector arguments).
// SPREAD: padding between handlers (see below).
// chain: each uop reaches the next through a link pointer (FXR's block chaining: two dependent loads).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none)
#define CC __attribute__((preserve_none))
#else
#define CC
#endif
#ifndef ALIGN
#define ALIGN 4
#endif
typedef uint64_t V __attribute__((vector_size(16)));
typedef struct U U;
#ifdef FAT
#define PARAMS U *u, uint64_t n, uint64_t a, uint64_t b, uint64_t c3, uint64_t c4, uint64_t c5, uint64_t c6, \
    uint64_t c7, uint64_t c8, uint64_t c9, uint64_t c10, uint64_t c11, uint64_t c12, uint64_t c13, uint64_t c14, \
    uint64_t c15, uint64_t c16, uint64_t c17, uint64_t c18, uint64_t c19, uint64_t c20, uint64_t c21, \
    V x0, V x1, V x2, V x3, V x4, V x5, V x6, V x7
#define ARGS n, a, b, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21, \
    x0, x1, x2, x3, x4, x5, x6, x7
#else
#define PARAMS U *u, uint64_t n, uint64_t a, uint64_t b
#define ARGS n, a, b
#endif
typedef CC void (*H)(PARAMS);
struct U { H p; U *link; uint64_t pad[8]; };   // 80 bytes, as FXR's uops

#define HD static CC __attribute__((noinline, aligned(ALIGN))) void
#define DONE() do { printf("%llu\n", (unsigned long long)a); return; } while (0)
// SPREAD=n: n bytes (plus a varying 64-byte multiple, so L1I sets do not alias) of padding after
// each handler pair, spreading the hot handlers over megabytes of text as FXR's are
#ifdef SPREAD
#define STR_(X) #X
#define STR(X) STR_(X)
#define PAD(N) static __attribute__((used, noinline)) void pad##N(void) { __asm__ volatile(".skip " STR(SPREAD) " + 64 * ((" #N " * 7) % 32)"); }
#else
#define PAD(N)
#endif
#define DEF_I(N)                                                                           \
    HD hi##N(PARAMS) { a += N; if (__builtin_expect(!--n, 0)) DONE();                      \
        U *x = u + 1; __attribute__((musttail)) return x->p(x, ARGS); }                    \
    HD hc##N(PARAMS) { a += N; if (__builtin_expect(!--n, 0)) DONE();                      \
        U *x = u->link; __attribute__((musttail)) return x->p(x, ARGS); }                  \
    PAD(N)
DEF_I(0) DEF_I(1) DEF_I(2) DEF_I(3) DEF_I(4) DEF_I(5) DEF_I(6) DEF_I(7)
DEF_I(8) DEF_I(9) DEF_I(10) DEF_I(11) DEF_I(12) DEF_I(13) DEF_I(14) DEF_I(15)
DEF_I(16) DEF_I(17) DEF_I(18) DEF_I(19) DEF_I(20) DEF_I(21) DEF_I(22) DEF_I(23)
DEF_I(24) DEF_I(25) DEF_I(26) DEF_I(27) DEF_I(28) DEF_I(29) DEF_I(30) DEF_I(31)
#define L32(P) { P##0, P##1, P##2, P##3, P##4, P##5, P##6, P##7, P##8, P##9, P##10, P##11, P##12, P##13, P##14, \
    P##15, P##16, P##17, P##18, P##19, P##20, P##21, P##22, P##23, P##24, P##25, P##26, P##27, P##28, P##29, P##30, P##31 }
// st / ld / cp: each handler also stores, loads, or copies (load, add, store) 16 bytes at a cursor
// (b) sweeping 1 MB in 16-byte steps, like FXR running a copy loop (the arrays 1 MB apart).
static uint8_t *g_src, *g_dst;
#define MEMMASK ((1u << 20) - 1)
#define DEF_M(N)                                                                           \
    HD hst##N(PARAMS) { a += N; *(volatile V *)(g_dst + (b & MEMMASK)) = (V){ a, a }; b += 16;   \
        if (__builtin_expect(!--n, 0)) DONE(); U *x = u + 1; __attribute__((musttail)) return x->p(x, ARGS); } \
    HD hld##N(PARAMS) { a ^= (*(volatile V *)(g_src + (b & MEMMASK)))[0] + N; b += 16;        \
        if (__builtin_expect(!--n, 0)) DONE(); U *x = u + 1; __attribute__((musttail)) return x->p(x, ARGS); } \
    HD hcp##N(PARAMS) { V v = *(volatile V *)(g_src + (b & MEMMASK)); *(volatile V *)(g_dst + (b & MEMMASK)) = v + (V){ N, N }; \
        b += 16; if (__builtin_expect(!--n, 0)) DONE(); U *x = u + 1; __attribute__((musttail)) return x->p(x, ARGS); }
DEF_M(0) DEF_M(1) DEF_M(2) DEF_M(3) DEF_M(4) DEF_M(5) DEF_M(6) DEF_M(7)
DEF_M(8) DEF_M(9) DEF_M(10) DEF_M(11) DEF_M(12) DEF_M(13) DEF_M(14) DEF_M(15)
DEF_M(16) DEF_M(17) DEF_M(18) DEF_M(19) DEF_M(20) DEF_M(21) DEF_M(22) DEF_M(23)
DEF_M(24) DEF_M(25) DEF_M(26) DEF_M(27) DEF_M(28) DEF_M(29) DEF_M(30) DEF_M(31)
static H hs[32] = L32(hi), hcs[32] = L32(hc), hsts[32] = L32(hst), hlds[32] = L32(hld), hcps[32] = L32(hcp);
HD hwrap(PARAMS) { a ^= 1; if (__builtin_expect(!--n, 0)) DONE();
    U *x = u->link; __attribute__((musttail)) return x->p(x, ARGS); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "ind";
    int ring = argc > 2 ? atoi(argv[2]) : 16, nh = argc > 3 ? atoi(argv[3]) : 16, chain = !strcmp(mode, "chain"), mix = !strcmp(mode, "mix");
    uint64_t n = 200000000;
    H *set = chain ? hcs : !strcmp(mode, "st") ? hsts : !strcmp(mode, "ld") ? hlds : !strcmp(mode, "cp") ? hcps : hs;
    g_src = aligned_alloc(64, 2u << 20);
    g_dst = g_src + (1u << 20);
    memset(g_src, 1, 2u << 20);
    U *r = calloc((size_t)ring + 1, sizeof(U));
    for (int i = 0; i < ring; i++) {   // mix: handlers in a scrambled order, so one handler has several successors
        int h = mix ? (int)(((unsigned)i * 2654435761u >> 7) % (unsigned)nh) : i % nh;
        r[i].p = set[h]; r[i].link = &r[i + 1];
    }
    r[ring].p = hwrap; r[ring].link = r;
#ifdef FAT
    V z = { 0, 0 };
    r[0].p(r, n, 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, z, z, z, z, z, z, z, z);
#else
    r[0].p(r, n, 0, 0);
#endif
    return 0;
}
