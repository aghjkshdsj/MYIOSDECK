// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic (CI only, not part of FXR): what one threaded dispatch costs on this CPU. A ring of
// uops, each `ldr p, [u, #80]!; br p` into one of NH distinct preserve_none handlers that do one
// add, like FXR's handlers. Run under perf stat; the CI step divides by the dispatch count.
//   cc -O2 [-DALIGN=n] [-DFAT] dispatch_probe.c && ./a.out <ind|chain> <ring> <handlers>
// ALIGN: each handler aligned to n bytes (spreads them in memory as FXR's are spread).
// FAT: FXR's full handler signature (23 integer and 8 vector arguments).
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
#define DEF_I(N)                                                                           \
    HD hi##N(PARAMS) { a += N; if (__builtin_expect(!--n, 0)) DONE();                      \
        U *x = u + 1; __attribute__((musttail)) return x->p(x, ARGS); }                    \
    HD hc##N(PARAMS) { a += N; if (__builtin_expect(!--n, 0)) DONE();                      \
        U *x = u->link; __attribute__((musttail)) return x->p(x, ARGS); }
DEF_I(0) DEF_I(1) DEF_I(2) DEF_I(3) DEF_I(4) DEF_I(5) DEF_I(6) DEF_I(7)
DEF_I(8) DEF_I(9) DEF_I(10) DEF_I(11) DEF_I(12) DEF_I(13) DEF_I(14) DEF_I(15)
DEF_I(16) DEF_I(17) DEF_I(18) DEF_I(19) DEF_I(20) DEF_I(21) DEF_I(22) DEF_I(23)
DEF_I(24) DEF_I(25) DEF_I(26) DEF_I(27) DEF_I(28) DEF_I(29) DEF_I(30) DEF_I(31)
#define L32(P) { P##0, P##1, P##2, P##3, P##4, P##5, P##6, P##7, P##8, P##9, P##10, P##11, P##12, P##13, P##14, \
    P##15, P##16, P##17, P##18, P##19, P##20, P##21, P##22, P##23, P##24, P##25, P##26, P##27, P##28, P##29, P##30, P##31 }
static H hs[32] = L32(hi), hcs[32] = L32(hc);
HD hwrap(PARAMS) { a ^= 1; if (__builtin_expect(!--n, 0)) DONE();
    U *x = u->link; __attribute__((musttail)) return x->p(x, ARGS); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "ind";
    int ring = argc > 2 ? atoi(argv[2]) : 16, nh = argc > 3 ? atoi(argv[3]) : 16, chain = !strcmp(mode, "chain");
    uint64_t n = 200000000;
    U *r = calloc((size_t)ring + 1, sizeof(U));
    for (int i = 0; i < ring; i++) { r[i].p = (chain ? hcs : hs)[i % nh]; r[i].link = &r[i + 1]; }
    r[ring].p = hwrap; r[ring].link = r;
#ifdef FAT
    V z = { 0, 0 };
    r[0].p(r, n, 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, z, z, z, z, z, z, z, z);
#else
    r[0].p(r, n, 0, 0);
#endif
    return 0;
}
