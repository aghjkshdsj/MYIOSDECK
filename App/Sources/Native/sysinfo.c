// SPDX-License-Identifier: GPL-3.0-or-later

#include "sysinfo.h"

#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>

int mid_sysctl_flag(const char *name) {
    int v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return -1;
    return v;
}

static bool feat(const char *name) { return mid_sysctl_flag(name) == 1; }

static int64_t num(const char *name) {
    int64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return 0;
    if (len == sizeof(int32_t)) return (int64_t)(int32_t)v;
    return v;
}

static void str(const char *name, char *dst, size_t cap) {
    size_t len = cap;
    if (sysctlbyname(name, dst, &len, NULL, 0) != 0) dst[0] = 0;
    dst[cap - 1] = 0;
}

void mid_sysinfo_read(mid_sysinfo *o) {
    memset(o, 0, sizeof *o);
    str("hw.machine", o->machine, sizeof o->machine);
    str("machdep.cpu.brand_string", o->cpu_brand, sizeof o->cpu_brand);
    o->ncpu = (int)num("hw.ncpu");
    o->perf_cores = (int)num("hw.perflevel0.physicalcpu");
    o->eff_cores = (int)num("hw.perflevel1.physicalcpu");
    o->cacheline = (int)num("hw.cachelinesize");
    o->l1d_perf = (uint64_t)num("hw.perflevel0.l1dcachesize");
    o->l2_perf = (uint64_t)num("hw.perflevel0.l2cachesize");
    o->memsize = (uint64_t)num("hw.memsize");

    o->lse = feat("hw.optional.arm.FEAT_LSE");
    o->lse2 = feat("hw.optional.arm.FEAT_LSE2");
    o->rcpc = feat("hw.optional.arm.FEAT_LRCPC");
    o->rcpc2 = feat("hw.optional.arm.FEAT_LRCPC2");
    o->flagm = feat("hw.optional.arm.FEAT_FlagM");
    o->flagm2 = feat("hw.optional.arm.FEAT_FlagM2");
    o->frintts = feat("hw.optional.arm.FEAT_FRINTTS");
    o->aes = feat("hw.optional.arm.FEAT_AES");
    o->pmull = feat("hw.optional.arm.FEAT_PMULL");
    o->sha1 = feat("hw.optional.arm.FEAT_SHA1");
    o->sha256 = feat("hw.optional.arm.FEAT_SHA256");
    o->crc32 = feat("hw.optional.armv8_crc32");
    o->fcma = feat("hw.optional.arm.FEAT_FCMA");
    o->rpres = feat("hw.optional.arm.FEAT_RPRES");
    o->afp = feat("hw.optional.arm.FEAT_AFP");
    o->ecv = feat("hw.optional.arm.FEAT_ECV");
    o->wfxt = feat("hw.optional.arm.FEAT_WFxT");
    o->cssc = feat("hw.optional.arm.FEAT_CSSC");
    o->mops = feat("hw.optional.arm.FEAT_MOPS");
    o->rng = feat("hw.optional.arm.FEAT_RNG");
    o->sme = feat("hw.optional.arm.FEAT_SME");
    o->bf16 = feat("hw.optional.arm.FEAT_BF16");
    o->i8mm = feat("hw.optional.arm.FEAT_I8MM");
    o->dotprod = feat("hw.optional.arm.FEAT_DotProd");
    o->fp16 = feat("hw.optional.arm.FEAT_FP16");

    /* Older kernels only publish the pre-FEAT_ names for a few of these. */
    if (!o->lse) o->lse = feat("hw.optional.armv8_1_atomics");
    if (!o->fcma) o->fcma = feat("hw.optional.armv8_3_compnum");
}
