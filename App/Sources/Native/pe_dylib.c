// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT Windows code, milestone A spike. See pe_dylib.h and docs/NO_JIT_WINDOWS.md.
//
// engine/pedylib/pe2dylib.py lays a PE image out as mapped (every section at its RVA)
// across two segments of a dylib, which the IPA step signs: x18 trampolines, then
// headers + code in __TEXT (signed, r-x); the rest of the image in __DATA (rw,
// copy-on-write) directly behind it, then the TEB slot offset word. dyld maps it like
// any library. Here:
//   1. check that layout (every code page below the split). Pages dyld mapped cannot
//      be replaced: iOS fails mmap(MAP_FIXED) over them with EPERM (build 42), so
//      everything written below must lie in __DATA;
//   2. base relocations are applied;
//   3. imports are resolved against the app (myiosdeck.dll);
//   4. for ARM64EC, the dispatch pointers in the CHPE metadata are filled the way
//      Wine's ntdll does before its emulator is up (arm64x_check_call_early & co.);
//   5. read-only sections go back to read-only.

#include "pe_dylib.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
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

// Page kinds over the image: 0 = headers/unused, 1 = code, 2 = data, 3 = both.
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

// --- TEB ------------------------------------------------------------------------------------
// Windows code finds its TEB in x18, which iOS does not preserve. pe2dylib.py rewrote every
// x18 use into a trampoline that loads the TEB from the thread's TSD array
// (TPIDRRO_EL0 & ~7) at the byte offset in the dylib's myiosdeck_teb_offset word. On
// Darwin a pthread key IS its TSD slot index, so the offset is key * 8. The spike gives each
// thread that calls in a zeroed fake TEB with Self (+0x30) set; Wine will own this later.

static pthread_key_t g_teb_key;
static pthread_once_t g_teb_once = PTHREAD_ONCE_INIT;
static void teb_key_init(void) { pthread_key_create(&g_teb_key, NULL); }

static uint64_t teb_tsd_offset(void) {
    pthread_once(&g_teb_once, teb_key_init);
    return (uint64_t)g_teb_key * 8;
}

static uint8_t *teb_current(void) {
    pthread_once(&g_teb_once, teb_key_init);
    uint8_t *teb = pthread_getspecific(g_teb_key);
    if (!teb) {
        teb = calloc(1, 0x2000);
        memcpy(teb + 0x30, &teb, 8); // NT_TIB.Self
        pthread_setspecific(g_teb_key, teb);
    }
    return teb;
}

// What a rewritten x18 site computes: *(TPIDRRO_EL0 & ~7 + offset).
static void *teb_via_tsd(uint64_t offset) {
    uint64_t tsd;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(tsd));
    return *(void **)((tsd & ~7ull) + offset);
}

// --- loader -------------------------------------------------------------------------------

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

static bool pe_load(const char *path, pe_image *pe, char *err, size_t errlen) {
    uint8_t *kinds = NULL;
    memset(pe, 0, sizeof *pe);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) FAIL("dlopen: %s", dlerror());
    uint8_t *img = dlsym(h, "myiosdeck_pe_image"), *end = dlsym(h, "myiosdeck_pe_image_end");
    uint8_t *data = dlsym(h, "myiosdeck_pe_data");
    if (!img || !end || !data) FAIL("no myiosdeck_pe_image/_data symbols in the dylib");
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

    // 1. Layout: code pages in __TEXT below `data`, data pages in __DATA from `data` on.
    size_t npages;
    kinds = page_kinds(pe, &npages);
    size_t split = (size_t)(data - img) / PE_PAGE, code_pages = 0, data_pages = 0;
    if ((size_t)(data - img) % PE_PAGE) FAIL("__DATA payload at +0x%zx is not page aligned", (size_t)(data - img));
    for (size_t p = 0; p < npages; p++) {
        if ((kinds[p] & 1) && p >= split) FAIL("code page +0x%zx is in __DATA (not executable)", p * PE_PAGE);
        code_pages += (kinds[p] & 1) != 0;
        data_pages += p >= split;
    }
    mid_log("[pe-dylib] %zu code pages signed RX in __TEXT, %zu data pages RW in __DATA from +0x%zx", code_pages,
            data_pages, split * PE_PAGE);
    uint8_t *tramps = dlsym(h, "myiosdeck_x18_tramps");
    uint64_t *teb_off = dlsym(h, "myiosdeck_teb_offset");
    if (!tramps || !teb_off) FAIL("no x18 trampoline / TEB offset symbols in the dylib");
    *teb_off = teb_tsd_offset();
    mid_log("[pe-dylib] x18: %zu KB of build-time trampolines; TEB at TSD offset 0x%llx", (size_t)(img - tramps) >> 10,
            (unsigned long long)*teb_off);

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
            if (rva / PE_PAGE >= npages || rva / PE_PAGE < split) FAIL("relocation in __TEXT at +0x%x", rva);
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
            if ((iat + 8 * i) / PE_PAGE < split) FAIL("IAT in __TEXT");
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
            if (rva / PE_PAGE < split) FAIL("EC pointer %s in __TEXT", slots[k].what);
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

    // 5. __DATA pages that hold no writable section go back to read-only.
    for (uint32_t i = 0; i < pe->nsec; i++) {
        const uint8_t *s = pe->sec + 40 * i;
        if (!(rd32(s + 36) & SCN_MEM_WRITE) || !sec_span(s)) continue;
        for (size_t p = sec_va(s) / PE_PAGE; p <= (sec_va(s) + sec_span(s) - 1) / PE_PAGE && p < npages; p++)
            kinds[p] |= 4;
    }
    size_t ro = 0;
    for (size_t p = split; p < npages; p++)
        if (kinds[p] && !(kinds[p] & 4) && !mprotect(img + p * PE_PAGE, PE_PAGE, PROT_READ)) ro++;
    mid_log("[pe-dylib] %zu read-only data pages protected", ro);
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

#define PCHECK(what, got, want)                                                                        \
    do {                                                                                               \
        const void *g_ = (got), *w_ = (want);                                                          \
        mid_log("[pe-dylib] %s = %p (want %p) %s", what, g_, w_, g_ == w_ ? "ok" : "WRONG");           \
        ok &= g_ == w_;                                                                                \
    } while (0)

// The spike's x18 functions, each rewritten at build time into a TEB trampoline.
static bool check_x18(const pe_image *pe) {
    void *(*self)(void) = pe_export(pe, "spike_teb_self");
    void *(*mov)(void) = pe_export(pe, "spike_teb_mov");
    void *(*add)(void) = pe_export(pe, "spike_teb_add");
    void *(*index)(unsigned long long) = pe_export(pe, "spike_teb_index");
    void (*store)(unsigned) = pe_export(pe, "spike_teb_store");
    if (!self || !mov || !add || !index || !store) {
        mid_log("[pe-dylib] x18: spike_teb_* exports missing");
        return false;
    }
    uint8_t *teb = teb_current();
    bool ok = true;
    PCHECK("TSD slot sanity (no DLL code)", teb_via_tsd(teb_tsd_offset()), teb);
    PCHECK("x18 ldr [x18,#0x30] (TEB->Self)", self(), teb);
    PCHECK("x18 mov x0, x18", mov(), teb);
    PCHECK("x18 add x0, x18, #0x68", add(), teb + 0x68);
    PCHECK("x18 ldr [x18, x0, lsl #3] (index 6 = Self)", index(6), teb);
    store(0x1234);
    uint32_t v;
    memcpy(&v, teb + 0x68, 4);
    mid_log("[pe-dylib] x18 str w0, [x18,#0x68] wrote 0x%x (want 0x1234) %s", v, v == 0x1234 ? "ok" : "WRONG");
    ok &= v == 0x1234;
    return ok;
}

// Wine's real ntdll.dll (ARM64EC, Madeira's build), its x18 uses rewritten at build time.
// Only self-contained functions: nothing here needs ntdll's own initialisation.
static bool run_ntdll(const pe_image *pe, const char *name, char *line, size_t linelen) {
    uint32_t (*crc)(uint32_t, const void *, int) = pe_export(pe, "RtlComputeCrc32");
    void (*seterr)(uint32_t) = pe_export(pe, "RtlSetLastWin32Error");
    uint32_t (*geterr)(void) = pe_export(pe, "RtlGetLastWin32Error");
    size_t (*cmp)(const void *, const void *, size_t) = pe_export(pe, "RtlCompareMemory");
    void *(*curteb)(void) = pe_export(pe, "NtCurrentTeb");
    if (!crc || !seterr || !geterr || !cmp) {
        mid_log("[pe-dylib] %s: missing exports", name);
        snprintf(line, linelen, "%s: missing exports", name);
        return false;
    }
    uint8_t *teb = teb_current();
    bool ok = true;
    PCHECK("TSD slot sanity (no DLL code)", teb_via_tsd(teb_tsd_offset()), teb);
    mid_log("[pe-dylib] calling RtlComputeCrc32(0, \"123456789\", 9)");
    uint32_t c = crc(0, "123456789", 9);
    mid_log("[pe-dylib] RtlComputeCrc32 = 0x%08x (want 0xcbf43926) %s", c, c == 0xcbf43926u ? "ok" : "WRONG");
    ok &= c == 0xcbf43926u;
    mid_log("[pe-dylib] calling RtlCompareMemory");
    size_t same = cmp("Windows on iPhone", "Windows on iPad", 17);
    mid_log("[pe-dylib] RtlCompareMemory = %zu (want 13) %s", same, same == 13 ? "ok" : "WRONG");
    ok &= same == 13;
    mid_log("[pe-dylib] calling RtlSetLastWin32Error(1234): writes TEB->LastErrorValue through x18");
    seterr(1234);
    uint32_t v;
    memcpy(&v, teb + 0x68, 4);
    mid_log("[pe-dylib] TEB->LastErrorValue = %u (want 1234) %s", v, v == 1234 ? "ok" : "WRONG");
    ok &= v == 1234;
    v = 5678;
    memcpy(teb + 0x68, &v, 4);
    uint32_t g = geterr();
    mid_log("[pe-dylib] RtlGetLastWin32Error() = %u (want 5678) %s", g, g == 5678 ? "ok" : "WRONG");
    ok &= g == 5678;
    if (curteb) PCHECK("NtCurrentTeb()", curteb(), teb);
    mid_log("[pe-dylib] %s: %s", name, ok ? "PASSED (Wine's ntdll ran from a signed dylib)" : "FAILED");
    snprintf(line, linelen, "%s: %s", name, ok ? "passed (Wine ntdll)" : "failed (see log)");
    return ok;
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
    if (strstr(name, "ntdll")) return run_ntdll(&pe, name, line, linelen);
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
    fails += !check_x18(&pe);

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
    // Spike aarch64, then spike arm64ec (needs more of the loader), then Wine's ntdll last.
    for (int i = 0; i + 1 < count; i++)
        for (int j = i + 1; j < count; j++)
            if ((strstr(names[i], "ntdll") != NULL) - (strstr(names[j], "ntdll") != NULL) > 0 ||
                ((strstr(names[i], "ntdll") != NULL) == (strstr(names[j], "ntdll") != NULL) &&
                 strcmp(names[i], names[j]) > 0)) {
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
