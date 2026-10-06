// SPDX-License-Identifier: GPL-3.0-or-later

#include "bench.h"

#include <mach/mach_time.h>

#include "bench_kernels.h"

#if __has_include("guest_hello.h") && __has_include("guest_bench.h")
#include "guest_bench.h"
#include "guest_hello.h"
#define HAVE_GUESTS 1
#endif
#if __has_include("guest_bench_sse2.h")
#include "guest_bench_sse2.h"
#define HAVE_SSE2_GUEST 1
#endif

int mid_bench_count(void) { return (int)BK_KERNEL_COUNT; }
const char *mid_bench_name(int i) { return bk_kernels[i].name; }
const char *mid_bench_what(int i) { return bk_kernels[i].what; }
unsigned mid_bench_default_scale(int i) { return bk_kernels[i].scale; }

uint64_t mid_bench_native(int i, unsigned scale, uint64_t *sum) {
    const bk_kernel *k = &bk_kernels[i];
    if (!scale) scale = k->scale;
    k->fn(scale / 8 ? scale / 8 : 1); /* same warm-up as the guest */
    uint64_t t0 = mach_absolute_time();
    bk_u64 s = k->fn(scale);
    uint64_t t1 = mach_absolute_time();
    if (sum) *sum = s;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (t1 - t0) * tb.numer / tb.denom;
}

#ifdef HAVE_GUESTS
const uint8_t *mid_guest_hello(size_t *len) {
    *len = guest_hello_elf_len;
    return guest_hello_elf;
}
const uint8_t *mid_guest_bench(size_t *len) {
    *len = guest_bench_elf_len;
    return guest_bench_elf;
}
#ifdef HAVE_SSE2_GUEST
const uint8_t *mid_guest_bench_sse2(size_t *len) {
    *len = guest_bench_sse2_elf_len;
    return guest_bench_sse2_elf;
}
#else
const uint8_t *mid_guest_bench_sse2(size_t *len) { return mid_guest_bench(len); }
#endif
#else
const uint8_t *mid_guest_bench_sse2(size_t *len) {
    *len = 0;
    return 0;
}
const uint8_t *mid_guest_hello(size_t *len) {
    *len = 0;
    return 0;
}
const uint8_t *mid_guest_bench(size_t *len) {
    *len = 0;
    return 0;
}
#endif
