// SPDX-License-Identifier: GPL-3.0-or-later
// Native driver for the benchmark kernels (the ARM64 baseline): same source, same warm-up
// and timing as engine/guest/bench.c, prints the same RESULT line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../guest/bench_kernels.h"
static unsigned long long now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: native <kernel> [scale]\n"); return 2; }
    for (unsigned i = 0; i < BK_KERNEL_COUNT; i++) {
        const bk_kernel *k = &bk_kernels[i];
        if (strcmp(argv[1], k->name)) continue;
        unsigned scale = argc > 2 ? (unsigned)atoi(argv[2]) : k->scale;
        if (!scale) scale = k->scale;
        k->fn(scale / 8 ? scale / 8 : 1);
        unsigned long long t0 = now_ns();
        bk_u64 sum = k->fn(scale);
        unsigned long long t1 = now_ns();
        printf("RESULT kernel=%s ns=%llu sum=%llu\n", k->name, t1 - t0, (unsigned long long)sum);
        return 0;
    }
    fprintf(stderr, "unknown kernel\n");
    return 3;
}
