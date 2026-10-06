// SPDX-License-Identifier: GPL-3.0-or-later
// x86-64 benchmark runner: `bench <kernel> [scale]`.
// Runs one warm-up pass (so FEX has compiled the hot blocks), then times a
// full pass with the guest's own clock. Prints one machine-readable line:
//   RESULT kernel=<name> ns=<time> sum=<checksum>

#include "guest_rt.h"
#include "bench_kernels.h"

int guest_main(int argc, char **argv) {
    if (argc < 2) {
        g_puts("usage: bench <kernel> [scale]\n");
        return 2;
    }
    for (unsigned i = 0; i < BK_KERNEL_COUNT; i++) {
        const bk_kernel *k = &bk_kernels[i];
        if (!g_streq(argv[1], k->name)) continue;
        unsigned scale = argc > 2 ? g_atou(argv[2]) : k->scale;
        if (!scale) scale = k->scale;

        k->fn(scale / 8 ? scale / 8 : 1); /* warm-up: JIT compile */
        unsigned long long t0 = g_now_ns();
        bk_u64 sum = k->fn(scale);
        unsigned long long t1 = g_now_ns();

        g_puts("RESULT kernel=");
        g_puts(k->name);
        g_puts(" ns=");
        g_putu(t1 - t0);
        g_puts(" sum=");
        g_putu(sum);
        g_puts("\n");
        return 0;
    }
    g_puts("unknown kernel\n");
    return 3;
}
