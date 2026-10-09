// SPDX-License-Identifier: GPL-3.0-or-later
// Performance Lab: the held-out benchmark (engine/guest/heldout.c, frozen), run natively and
// through the no-JIT interpreters. No engine is tuned for these kernels; the standard kernels
// (bench.h) are what an engine could end up tuned for.

#ifndef MYIOSDECK_HELDOUT_H
#define MYIOSDECK_HELDOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mid_heldout_count(void);
const char *mid_heldout_name(int i);
unsigned mid_heldout_default_scale(int i);
/// Runs held-out kernel i natively with the same warm-up + timed pass as the guest.
/// Returns elapsed nanoseconds of the timed pass; *sum gets the checksum.
uint64_t mid_heldout_native(int i, unsigned scale, uint64_t *sum);
/// The held-out benchmark built for baseline x86-64 (embedded by CI; NULL, *len = 0 if missing).
const uint8_t *mid_guest_heldout(size_t *len);

#ifdef __cplusplus
}
#endif

#endif
