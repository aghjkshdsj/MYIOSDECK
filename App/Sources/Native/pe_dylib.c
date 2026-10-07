// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT Windows code, milestone A spike. See pe_dylib.h and docs/NO_JIT_WINDOWS.md.
//
// engine/pedylib/pe2dylib.py lays a PE image out as mapped (every section at its RVA,
// sections 16 KB aligned) inside __TEXT of a dylib, which the IPA step signs. dyld maps
// it like any library: the whole image is then signed, readable and executable. Here:
//   1. every page of a non-code section is replaced by anonymous read-write memory at
//      the same address with the same bytes (code pages are never touched, and stay
//      signed and executable: no JIT involved at any point);
//   2. base relocations are applied (only data pages may need them);
//   3. imports are resolved against the app (myiosdeck.dll);
//   4. for ARM64EC, the dispatch pointers in the CHPE metadata are filled the way
//      Wine's ntdll does before its emulator is up (arm64x_check_call_early & co.);
//   5. read-only sections go back to read-only.

#include "pe_dylib.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bench.h"
#include "jit_core.h"

#define PE_PAGE 0x4000u
#define SCN_CNT_CODE 0x00000020u
#define SCN_MEM_EXECUTE 0x20000000u
#define SCN_MEM_WRITE 0x80000000u

typedef struct {
    uint8_t *base;
    size_t size;
    uint64_t image_base;
    uint32_t nsec;
    const uint8_t *sec;   // first IMAGE_SECTION_HEADER
    const uint8_t *dirs;  // IMAGE_DATA_DIRECTORY[16]
    uint32_t ndirs;
    uint16_t machine;
} pe_image;

static uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void pe_dir(const pe_image *pe, unsigned i, uint32_t *rva, uint32_t *size) {
    *rva = *size = 0;
    if (i < pe->ndirs) { *rva = rd32(pe->dirs + 8 * i); *size = rd32(pe->dirs + 8 * i + 4); }
}

static bool sec_is_code(const uint8_t *s) { return (rd32(s + 36) & (SCN_MEM_EXECUTE | SCN_CNT_CODE)) != 0; }
static uint32_t sec_va(const uint8_t *s) { return rd32(s + 12); }
static uint32_t sec_span(const uint8_t *s) {
    uint32_t v = rd32(s + 8), r = rd32(s + 16);
    return v > r ? v : r;
}

// Page kinds over the image: 0 = headers/unused, 1 = code, 2 = data, 3 = both (unloadable).
static uint8_t *page_kinds(const pe_image *pe, size_t *npages) {
    size_t n = (pe->size + PE_PAGE - 1) / PE_PAGE;
    uint8_t *k = calloc(n, 1);
    for (uint32_t i = 0; i < pe->nsec; i++) {
        const uint8_t *s = pe->sec + 40 * i;
        uint32_t va = sec_va(s), span = sec_span(s);
        if (!span) continue;
        for (size_t p = va / PE_PAGE; p <= (va + span - 1) / PE_PAGE && p < n; p++) k[p] |= sec_is_code(s) ? 1 : 2;
    }
    *npages = n;
    return k;
}

// --- what the app exports to the DLL (engine/pedylib/spike/myiosdeck.def) ----------------

static int g_host_log_calls;
static void host_log(const char *line) {
    g_host_log_calls++;
    mid_log("[pe-dylib] DLL says: %s", line);
}
static int host_twice(int x) { return 2 * x; }

static const struct { const char *name; void *fn; } g_host_exports[] = {
    { "host_log", (void *)host_log }, { "host_twice", (void *)host_twice },
    { "memset", (void *)memset },     { "memcpy", (void *)memcpy },
    { "memmove", (void *)memmove },
};

// --- ARM64EC dispatch (what ntdll installs before its x64 emulator exists) ---------------
// __os_arm64x_check_call: target in x9, returns the address to call in x11.
// __os_arm64x_check_icall(_cfg): target in x11, returned unchanged: all code is native.
// Entering x64 code is not possible in this spike (no emulator): FXI takes that role later.
void mid_ec_check_call(void);
void mid_ec_check_icall(void);
__asm__(".text\n"
        ".p2align 2\n"
        ".globl _mid_ec_check_call\n"
        "_mid_ec_check_call:\n"
        "    mov x11, x9\n"
        "    ret\n"
        ".globl _mid_ec_check_icall\n"
        "_mid_ec_check_icall:\n"
        "    ret\n");

static void mid_ec_enter_x64(void) {
    mid_log("[pe-dylib] FAIL: the DLL tried to enter x64 code; this spike has no x64 CPU");
    abort();
}

// --- loader -------------------------------------------------------------------------------

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

static bool pe_load(const char *path, pe_image *pe, char *err, size_t errlen) {
    uint8_t *kinds = NULL;
    memset(pe, 0, sizeof *pe);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) FAIL("dlopen: %s", dlerror());
    uint8_t *img = dlsym(h, "myiosdeck_pe_image"), *end = dlsym(h, "myiosdeck_pe_image_end");
    if (!img || !end) FAIL("no myiosdeck_pe_image symbol in the dylib");
    if (img[0] != 'M' || img[1] != 'Z') FAIL("no MZ header at %p", (void *)img);
    const uint8_t *nt = img + rd32(img + 0x3C);
    if (memcmp(nt, "PE\0\0", 4)) FAIL("no PE header");
    const uint8_t *opt = nt + 24;
    if (rd16(opt) != 0x20B) FAIL("not PE32+");
    pe->base = img;
    pe->machine = rd16(nt + 4);
    pe->nsec = rd16(nt + 6);
    pe->sec = opt + rd16(nt + 20);
    pe->image_base = rd64(opt + 24);
    pe->size = rd32(opt + 56);
    pe->ndirs = rd32(opt + 108) < 16 ? rd32(opt + 108) : 16;
    pe->dirs = opt + 112;
    if (pe->size > (size_t)(end - img)) FAIL("SizeOfImage 0x%zx > dylib payload 0x%zx", pe->size, (size_t)(end - img));
    if ((uintptr_t)img % PE_PAGE) FAIL("image at %p is not 16 KB aligned", (void *)img);
    mid_log("[pe-dylib] dlopen ok: image %p+0x%zx machine 0x%x, %u sections, preferred base 0x%llx", (void *)img,
            pe->size, pe->machine, pe->nsec, (unsigned long long)pe->image_base);

    // 1. Data pages -> anonymous RW copies (code pages stay as dyld mapped them).
    size_t npages;
    kinds = page_kinds(pe, &npages);
    size_t code_pages = 0, data_pages = 0;
    for (size_t p = 0; p < npages; p++) {
        if (kinds[p] == 3) FAIL("page +0x%zx holds code and data (rebuild with 16 KB section alignment)", p * PE_PAGE);
        code_pages += kinds[p] == 1;
    }
    for (size_t p = 0; p < npages;) {
        if (kinds[p] != 2) { p++; continue; }
        size_t q = p;
        while (q < npages && kinds[q] == 2) q++;
        uint8_t *at = img + p * PE_PAGE;
        size_t len = (q - p) * PE_PAGE;
        void *copy = malloc(len);
        memcpy(copy, at, len);
        void *r = mmap(at, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r != at) { int e = errno; free(copy); FAIL("mmap RW over data pages %p+0x%zx: %s", (void *)at, len, strerror(e)); }
        memcpy(at, copy, len);
        free(copy);
        data_pages += q - p;
        p = q;
    }
    mid_log("[pe-dylib] %zu code pages stay signed RX in place, %zu data pages now RW", code_pages, data_pages);

    // 2. Base relocations.
    int64_t delta = (int64_t)((uintptr_t)img - pe->image_base);
    uint32_t rrva, rsize, nrel = 0;
    pe_dir(pe, 5, &rrva, &rsize);
    for (uint32_t off = 0; off + 8 <= rsize;) {
        const uint8_t *blk = img + rrva + off;
        uint32_t page = rd32(blk), bsize = rd32(blk + 4);
        if (bsize < 8) break;
        for (uint32_t i = 0; i < (bsize - 8) / 2; i++) {
            uint16_t e = rd16(blk + 8 + 2 * i);
            uint32_t t = e >> 12, rva = page + (e & 0xFFF);
            if (!t) continue;
            if (t != 10) FAIL("relocation type %u at +0x%x", t, rva);
            if (rva / PE_PAGE >= npages || kinds[rva / PE_PAGE] != 2) FAIL("relocation in a code page at +0x%x", rva);
            uint64_t v = rd64(img + rva) + (uint64_t)delta;
            memcpy(img + rva, &v, 8);
            nrel++;
        }
        off += bsize;
    }
    mid_log("[pe-dylib] %u relocations applied (delta %+lld)", nrel, (long long)delta);

    // 3. Imports.
    uint32_t irva, isize, nimp = 0;
    pe_dir(pe, 1, &irva, &isize);
    for (const uint8_t *d = img + irva; isize && rd32(d + 12); d += 20) {
        const char *dll = (const char *)img + rd32(d + 12);
        uint32_t ilt = rd32(d) ? rd32(d) : rd32(d + 16), iat = rd32(d + 16);
        for (uint32_t i = 0;; i++) {
            uint64_t v = rd64(img + ilt + 8 * i);
            if (!v) break;
            if (v >> 63) FAIL("import by ordinal %s!#%u", dll, (unsigned)(v & 0xFFFF));
            const char *name = (const char *)img + (uint32_t)v + 2;
            void *fn = NULL;
            for (size_t k = 0; k < sizeof g_host_exports / sizeof g_host_exports[0]; k++)
                if (!strcmp(g_host_exports[k].name, name)) fn = g_host_exports[k].fn;
            if (!fn) FAIL("unresolved import %s!%s", dll, name);
            if (kinds[(iat + 8 * i) / PE_PAGE] != 2) FAIL("IAT in a code page");
            memcpy(img + iat + 8 * i, &fn, 8);
            nimp++;
        }
    }
    mid_log("[pe-dylib] %u imports resolved against the app", nimp);

    // 4. ARM64EC: CHPE metadata dispatch pointers (indices into IMAGE_ARM64EC_METADATA).
    uint32_t lrva, lsize;
    pe_dir(pe, 10, &lrva, &lsize);
    uint64_t meta_va = (lrva && lsize >= 0xD0 && rd32(img + lrva) >= 0xD0) ? rd64(img + lrva + 0xC8) : 0;
    if (meta_va) {
        const uint8_t *meta = (const uint8_t *)(uintptr_t)meta_va; // already relocated
        if (meta < img || meta >= img + pe->size) FAIL("CHPE metadata %p outside the image", (void *)meta);
        static const struct { int idx; const char *what; } slots[] = {
            { 5, "dispatch_call_no_redirect" }, { 6, "dispatch_ret" }, { 7, "check_call" },
            { 8, "check_icall" }, { 9, "check_icall_cfg" }, { 18, "dispatch_fptr" },
        };
        int set = 0;
        for (size_t k = 0; k < sizeof slots / sizeof slots[0]; k++) {
            uint32_t rva = rd32(meta + 4 * slots[k].idx);
            if (!rva) continue;
            if (kinds[rva / PE_PAGE] != 2) FAIL("EC pointer %s in a code page", slots[k].what);
            void *fn = slots[k].idx == 7 ? (void *)mid_ec_check_call
                     : (slots[k].idx == 8 || slots[k].idx == 9) ? (void *)mid_ec_check_icall
                     : (void *)mid_ec_enter_x64;
            memcpy(img + rva, &fn, 8);
            set++;
        }
        mid_log("[pe-dylib] ARM64EC: CHPE metadata v%u at +0x%lx, %d dispatch pointers set, %u code ranges",
                rd32(meta), (unsigned long)(meta - img), set, rd32(meta + 8));
    } else if (pe->machine == 0x8664) {
        mid_log("[pe-dylib] note: x64/ARM64EC machine but no CHPE metadata");
    }

    // 5. Read-only sections back to read-only.
    for (uint32_t i = 0; i < pe->nsec; i++) {
        const uint8_t *s = pe->sec + 40 * i;
        if (sec_is_code(s) || (rd32(s + 36) & SCN_MEM_WRITE) || !sec_span(s)) continue;
        size_t len = (sec_span(s) + PE_PAGE - 1) & ~(size_t)(PE_PAGE - 1);
        mprotect(img + sec_va(s), len, PROT_READ);
    }
    free(kinds);
    return true;
fail:
    free(kinds);
    return false;
}

static void *pe_export(const pe_image *pe, const char *name) {
    uint32_t rva, size;
    pe_dir(pe, 0, &rva, &size);
    if (!size) return NULL;
    const uint8_t *e = pe->base + rva;
    uint32_t nnames = rd32(e + 24), funcs = rd32(e + 28), names = rd32(e + 32), ords = rd32(e + 36);
    for (uint32_t i = 0; i < nnames; i++) {
        if (strcmp((const char *)pe->base + rd32(pe->base + names + 4 * i), name)) continue;
        uint32_t f = rd32(pe->base + funcs + 4 * rd16(pe->base + ords + 2 * i));
        if (f >= rva && f < rva + size) return NULL; // forwarders unsupported
        uint8_t *p = pe->base + f;
        // ARM64EC exports point at an x64 "fast-forward" thunk in .hexpthk
        // (mov rax,rsp; mov [rax+0x20],rbx; push rbp; pop rbp; jmp <native>), so x64
        // callers and hooks see x64 code. A native caller follows the jmp, as Wine's
        // arm64x_check_call does (.Lffwd_seq in signal_arm64ec.c).
        static const uint8_t ffwd[10] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9 };
        if (!memcmp(p, ffwd, sizeof ffwd)) {
            uint8_t *native = p + 14 + (int32_t)rd32(p + 10);
            mid_log("[pe-dylib] export %s: x64 fast-forward thunk +0x%x -> native +0x%lx", name, f,
                    (unsigned long)(native - pe->base));
            p = native;
        }
        return p;
    }
    return NULL;
}

// --- the spike ----------------------------------------------------------------------------

static int host_callback(int x) { return 3 * x; }

static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

static bool run_one(const char *path, const char *name, char *line, size_t linelen) {
    char err[256] = "";
    pe_image pe;
    mid_log("[pe-dylib] ---- %s ----", name);
    if (!pe_load(path, &pe, err, sizeof err)) {
        mid_log("[pe-dylib] %s: LOAD FAILED: %s", name, err);
        snprintf(line, linelen, "%s: load failed: %s", name, err);
        return false;
    }
    int (*add)(int, int) = pe_export(&pe, "spike_add");
    int (*counter)(void) = pe_export(&pe, "spike_counter");
    int (*words)(void) = pe_export(&pe, "spike_words_len");
    int (*imp)(int) = pe_export(&pe, "spike_import");
    int (*cb)(int (*)(int), int) = pe_export(&pe, "spike_callback");
    int (*kcount)(void) = pe_export(&pe, "spike_kernel_count");
    uint64_t (*kernel)(int, unsigned) = pe_export(&pe, "spike_kernel");
    if (!add || !counter || !words || !imp || !cb || !kcount || !kernel) {
        mid_log("[pe-dylib] %s: missing exports", name);
        snprintf(line, linelen, "%s: missing exports", name);
        return false;
    }
    int fails = 0, r;
#define CHECK(what, expr, want)                                                                   \
    do {                                                                                          \
        mid_log("[pe-dylib] calling %s", what);                                                   \
        r = (expr);                                                                               \
        mid_log("[pe-dylib] %s = %d (want %d) %s", what, r, (want), r == (want) ? "ok" : "WRONG"); \
        fails += r != (want);                                                                     \
    } while (0)
    CHECK("spike_add(2, 3): code", add(2, 3), 5);
    CHECK("spike_counter(): .data write", counter(), 42);
    CHECK("spike_counter() again", counter(), 43);
    CHECK("spike_words_len(): relocated .rdata pointers", words(), 16);
    int logs = g_host_log_calls;
    CHECK("spike_import(21): import via IAT", imp(21), 42);
    if (g_host_log_calls != logs + 1) { mid_log("[pe-dylib] host_log was not called"); fails++; }
    CHECK("spike_callback(cb, 10): indirect call", cb(host_callback, 10), 31);

    // Benchmark kernels: inside the DLL vs. the app's native build (same source, same flags).
    int n = kcount() < mid_bench_count() ? kcount() : mid_bench_count();
    double eff_sum = 0;
    for (int i = 0; i < n; i++) {
        unsigned scale = mid_bench_default_scale(i);
        uint64_t nsum = 0, nat = mid_bench_native(i, scale, &nsum);
        kernel(i, scale / 8 ? scale / 8 : 1); // same warm-up
        uint64_t t0 = now_ns();
        uint64_t psum = kernel(i, scale);
        uint64_t dt = now_ns() - t0;
        double eff = dt ? 100.0 * (double)nat / (double)dt : 0;
        eff_sum += eff;
        mid_log("[pe-dylib] bench %-8s native %6.1f ms  DLL %6.1f ms  %5.1f%%  checksum %s", mid_bench_name(i),
                nat / 1e6, dt / 1e6, eff, psum == nsum ? "match" : "DIFFERS");
        fails += psum != nsum;
    }
    double avg = n ? eff_sum / n : 0;
    mid_log("[pe-dylib] %s: %s, DLL code at %.0f%% of native", name, fails ? "FAILED" : "PASSED", avg);
    snprintf(line, linelen, "%s: %s, %.0f%% of native", name, fails ? "failed (see log)" : "passed", avg);
    return fails == 0;
}

bool mid_pe_dylib_spike(const char *dir, char *summary, size_t summary_len) {
    summary[0] = 0;
    uint32_t cs = mid_cs_flags();
    mid_log("[pe-dylib] no-JIT spike: Windows DLLs from signed dylibs. cs_flags=0x%x, JIT %s", cs,
            mid_jit_debugged() ? "ON (debugger attached at some point): run again with JIT off for the real proof"
                               : "OFF (no debugger this run): this is the no-JIT proof");
    DIR *d = opendir(dir);
    if (!d) {
        mid_log("[pe-dylib] no PE folder in the app (%s): this build has no spike DLLs", dir);
        snprintf(summary, summary_len, "This build has no spike DLLs.");
        return false;
    }
    char names[8][128];
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 8) {
        size_t l = strlen(e->d_name);
        if (l > 6 && !strcmp(e->d_name + l - 6, ".dylib")) snprintf(names[count++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    // aarch64 before arm64ec: the plain ARM64 image needs less of the loader.
    for (int i = 0; i + 1 < count; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[128];
                memcpy(t, names[i], sizeof t); memcpy(names[i], names[j], sizeof t); memcpy(names[j], t, sizeof t);
            }
    bool ok = count > 0;
    for (int i = 0; i < count; i++) {
        char path[1024], line[256];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        ok &= run_one(path, names[i], line, sizeof line);
        size_t used = strlen(summary);
        snprintf(summary + used, summary_len - used, "%s%s", used ? "\n" : "", line);
    }
    mid_log("[pe-dylib] spike %s (JIT was %s)", ok ? "PASSED" : "FAILED", mid_jit_debugged() ? "on" : "off");
    return ok;
}
