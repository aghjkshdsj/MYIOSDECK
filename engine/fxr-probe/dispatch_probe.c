// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic (CI only, not part of FXR): what one threaded dispatch costs on this CPU. A ring of
// uops, each `ldr p, [u, #size]!; br p` into one of NH distinct preserve_none handlers that do one
// add, like FXR's handlers. Reports cycles per dispatch (from the wall clock and an assumed GHz is
// not needed: run it under perf stat) for several ring sizes and handler counts, and the same
// handlers chained by direct tail calls (no indirect branch) for comparison.
//   cc -O2 dispatch_probe.c && perf stat ./a.out <mode> <ring> <handlers>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none)
#define CC __attribute__((preserve_none))
#else
#define CC
#endif
typedef struct U U;
typedef CC void (*H)(U *u, uint64_t n, uint64_t a, uint64_t b);
struct U { H p; uint64_t pad[9]; };   // 80 bytes, as FXR's uops

#define H8(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)

// indirect: like FXR (the next uop's handler pointer, one load with writeback, one branch)
#define DEF_I(N) static CC void hi##N(U *u, uint64_t n, uint64_t a, uint64_t b) { \
    a += N; if (__builtin_expect(!--n, 0)) { printf("%llu\n", (unsigned long long)a); return; } \
    U *x = u + 1; __attribute__((musttail)) return x->p(x, n, a, b); }
#define DEF_I8(M) DEF_I(M)
H8(DEF_I8)
DEF_I(8) DEF_I(9) DEF_I(10) DEF_I(11) DEF_I(12) DEF_I(13) DEF_I(14) DEF_I(15)
DEF_I(16) DEF_I(17) DEF_I(18) DEF_I(19) DEF_I(20) DEF_I(21) DEF_I(22) DEF_I(23)
DEF_I(24) DEF_I(25) DEF_I(26) DEF_I(27) DEF_I(28) DEF_I(29) DEF_I(30) DEF_I(31)
static H hs[32] = { hi0, hi1, hi2, hi3, hi4, hi5, hi6, hi7, hi8, hi9, hi10, hi11, hi12, hi13, hi14, hi15,
                    hi16, hi17, hi18, hi19, hi20, hi21, hi22, hi23, hi24, hi25, hi26, hi27, hi28, hi29, hi30, hi31 };
// ring wrap: the last uop jumps back to the first
static CC void hwrap(U *u, uint64_t n, uint64_t a, uint64_t b) {
    a ^= 1; if (__builtin_expect(!--n, 0)) { printf("%llu\n", (unsigned long long)a); return; }
    U *x = (U *)(uintptr_t)u->pad[0]; __attribute__((musttail)) return x->p(x, n, a, b); }
// direct: 32 handlers each tail-calling the next by name (a direct branch)
#define DEF_D(N, NX) static CC void hd##N(U *u, uint64_t n, uint64_t a, uint64_t b) { \
    a += N; if (__builtin_expect(!--n, 0)) { printf("%llu\n", (unsigned long long)a); return; } \
    __attribute__((musttail)) return hd##NX(u, n, a, b); }
static CC void hd0(U *u, uint64_t n, uint64_t a, uint64_t b);
DEF_D(31, 0) DEF_D(30, 31) DEF_D(29, 30) DEF_D(28, 29) DEF_D(27, 28) DEF_D(26, 27) DEF_D(25, 26) DEF_D(24, 25)
DEF_D(23, 24) DEF_D(22, 23) DEF_D(21, 22) DEF_D(20, 21) DEF_D(19, 20) DEF_D(18, 19) DEF_D(17, 18) DEF_D(16, 17)
DEF_D(15, 16) DEF_D(14, 15) DEF_D(13, 14) DEF_D(12, 13) DEF_D(11, 12) DEF_D(10, 11) DEF_D(9, 10) DEF_D(8, 9)
DEF_D(7, 8) DEF_D(6, 7) DEF_D(5, 6) DEF_D(4, 5) DEF_D(3, 4) DEF_D(2, 3) DEF_D(1, 2)
static CC void hd0(U *u, uint64_t n, uint64_t a, uint64_t b) {
    a += 0; if (__builtin_expect(!--n, 0)) { printf("%llu\n", (unsigned long long)a); return; }
    __attribute__((musttail)) return hd1(u, n, a, b); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "ind";
    int ring = argc > 2 ? atoi(argv[2]) : 16, nh = argc > 3 ? atoi(argv[3]) : 16;
    uint64_t n = 200000000;
    if (!strcmp(mode, "dir")) { hd0(0, n, 0, 0); return 0; }
    U *r = calloc((size_t)ring + 1, sizeof(U));
    for (int i = 0; i < ring; i++) r[i].p = hs[i % nh];
    r[ring].p = hwrap; r[ring].pad[0] = (uint64_t)(uintptr_t)r;
    r[0].p(r, n, 0, 0);
    return 0;
}
