// SPDX-License-Identifier: GPL-3.0-or-later
// Host CPU description read from the kernel (sysctl), used to tune FEX's
// code generator for the exact SoC instead of a hardcoded guess.

#ifndef MYIOSDECK_SYSINFO_H
#define MYIOSDECK_SYSINFO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char machine[32];        // e.g. "iPhone16,2" (iPhone 15 Pro Max)
    char cpu_brand[64];      // machdep.cpu.brand_string, e.g. "Apple A17 Pro"
    int ncpu;
    int perf_cores;          // hw.perflevel0.physicalcpu
    int eff_cores;           // hw.perflevel1.physicalcpu
    int cacheline;           // hw.cachelinesize
    uint64_t l1d_perf;       // hw.perflevel0.l1dcachesize
    uint64_t l2_perf;        // hw.perflevel0.l2cachesize
    uint64_t memsize;        // hw.memsize

    // ARMv8.x features FEX's backend can use (hw.optional.arm.FEAT_*)
    bool lse, lse2, rcpc, rcpc2, flagm, flagm2, frintts, aes, pmull, sha1, sha256, crc32;
    bool fcma, rpres, afp, ecv, wfxt, cssc, mops, rng, sme, bf16, i8mm, dotprod, fp16;
} mid_sysinfo;

void mid_sysinfo_read(mid_sysinfo *out);

/// Value of one hw.optional.* sysctl, or -1 if the kernel does not know it.
int mid_sysctl_flag(const char *name);

#ifdef __cplusplus
}
#endif

#endif
