// SPDX-License-Identifier: GPL-3.0-or-later
// Performance Lab: the same kernels run natively (ARM64) and through FEX (x86-64).

#ifndef MYIOSDECK_BENCH_H
#define MYIOSDECK_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mid_bench_count(void);
const char *mid_bench_name(int i);
const char *mid_bench_what(int i);
unsigned mid_bench_default_scale(int i);
/// Runs kernel i natively with the same warm-up + timed pass as the guest.
/// Returns elapsed nanoseconds of the timed pass; *sum gets the checksum.
uint64_t mid_bench_native(int i, unsigned scale, uint64_t *sum);

/// Embedded x86-64 guest programs (built by CI from engine/guest).
const uint8_t *mid_guest_hello(size_t *len);
const uint8_t *mid_guest_bench(size_t *len);
/// The same benchmark built for baseline x86-64 (SSE2): what the interpreter runs.
const uint8_t *mid_guest_bench_sse2(size_t *len);

#ifdef __cplusplus
}
#endif

#endif
