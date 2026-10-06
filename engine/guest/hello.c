// SPDX-License-Identifier: GPL-3.0-or-later
// x86-64 "hello": proves translation works and shows what CPU the guest sees
// (FEX's emulated CPUID).

#include "guest_rt.h"

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

int guest_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_puts("Hello from x86-64 code, translated to ARM64 by FEX, running on iPhone!\n");

    unsigned r[4];
    char vendor[13];
    cpuid(0, 0, r);
    ((unsigned *)vendor)[0] = r[1];
    ((unsigned *)vendor)[1] = r[3];
    ((unsigned *)vendor)[2] = r[2];
    vendor[12] = 0;
    g_puts("Guest CPU vendor: ");
    g_puts(vendor);
    g_puts("\n");

    char brand[49];
    for (unsigned i = 0; i < 3; i++) {
        cpuid(0x80000002u + i, 0, r);
        for (int j = 0; j < 4; j++) ((unsigned *)brand)[i * 4 + j] = r[j];
    }
    brand[48] = 0;
    g_puts("Guest CPU brand:  ");
    g_puts(brand);
    g_puts("\n");

    cpuid(1, 0, r);
    g_puts("SSE2 ");
    g_puts((r[3] >> 26) & 1 ? "yes" : "no");
    g_puts(" | SSE4.2 ");
    g_puts((r[2] >> 20) & 1 ? "yes" : "no");
    g_puts(" | AVX ");
    g_puts((r[2] >> 28) & 1 ? "yes" : "no");
    g_puts("\n");

    unsigned long long t0 = g_now_ns();
    volatile unsigned long long x = 0;
    for (unsigned i = 0; i < 10000000u; i++) x += i ^ (x >> 3);
    unsigned long long t1 = g_now_ns();
    g_puts("10M-iteration loop: ");
    g_putu((t1 - t0) / 1000);
    g_puts(" us\n");
    return 0;
}
