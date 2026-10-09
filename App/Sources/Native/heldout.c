// SPDX-License-Identifier: GPL-3.0-or-later
// The held-out benchmark's kernels, native: engine/guest/heldout.c (frozen) compiled as it is in
// CI's native build (HELDOUT_HOSTED, -O2 -ffp-contract=off), with its command-line main renamed
// so the app calls the kernels directly.

#include "heldout.h"

#include <mach/mach_time.h>

#define HELDOUT_HOSTED 1
#define main mid_heldout_cli_main
#include "../../../engine/guest/heldout.c"
#undef main

#if __has_include("guest_heldout.h")
#include "guest_heldout.h"
#define HAVE_HELDOUT_GUEST 1
#endif

int mid_heldout_count(void) { return (int)HK_COUNT; }
const char *mid_heldout_name(int i) { return hk_kernels[i].name; }
unsigned mid_heldout_default_scale(int i) { return hk_kernels[i].scale; }

uint64_t mid_heldout_native(int i, unsigned scale, uint64_t *sum) {
    const hk_kernel *k = &hk_kernels[i];
    if (!scale) scale = k->scale;
    k->fn(scale / 8 ? scale / 8 : 1);   // the same warm-up as the guest
    uint64_t t0 = mach_absolute_time();
    hu64 s = k->fn(scale);
    uint64_t t1 = mach_absolute_time();
    if (sum) *sum = s;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (t1 - t0) * tb.numer / tb.denom;
}

const uint8_t *mid_guest_heldout(size_t *len) {
#ifdef HAVE_HELDOUT_GUEST
    *len = guest_heldout_elf_len;
    return guest_heldout_elf;
#else
    *len = 0;
    return 0;
#endif
}
