// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK JIT core. See jit_core.h for the design.

#include "jit_core.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <os/log.h>
#include <os/proc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CS_OPS_STATUS 0
#define CS_GET_TASK_ALLOW 0x00000004u
#define CS_DEBUGGED 0x10000000u
#define MID_PAGE 0x4000ul /* 16 KB pages on every arm64 iPhone */

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
/* Exported by libsystem_kernel; used by MeloNX and Madeira to take JIT pages
 * off the jetsam footprint. Undocumented: every failure is non-fatal. */
extern kern_return_t mach_memory_entry_ownership(mach_port_t entry, mach_port_t owner,
                                                 int ledger_tag, int ledger_flags);
#define MID_LEDGER_TAG_DEFAULT 0
#define MID_LEDGER_FLAG_NO_FOOTPRINT 1

// ---------------------------------------------------------------- logging --

static mid_log_fn g_sink;

void mid_set_log_sink(mid_log_fn sink) { g_sink = sink; }

void mid_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    os_log(OS_LOG_DEFAULT, "%{public}s", buf);
    if (g_sink) g_sink(buf);
}

// ---------------------------------------------------------- signing state --

uint32_t mid_cs_flags(void) {
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags) != 0) return 0;
    return flags;
}

bool mid_jit_debugged(void) { return (mid_cs_flags() & CS_DEBUGGED) != 0; }

bool mid_has_get_task_allow(void) { return (mid_cs_flags() & CS_GET_TASK_ALLOW) != 0; }

// ------------------------------------------------------------ BRK protocol --

__attribute__((noinline, optnone)) void *mid_jit26_prepare_region(void *addr, size_t len) {
    register void *x0 __asm__("x0") = addr;
    register size_t x1 __asm__("x1") = len;
    __asm__ volatile("mov x16, #1\n"
                     "brk #0xf00d\n"
                     : "+r"(x0)
                     : "r"(x1)
                     : "x16", "memory");
    return x0;
}

__attribute__((noinline, optnone)) void mid_jit26_detach(void) {
    __asm__ volatile("mov x16, #0\n"
                     "brk #0xf00d\n" ::: "x16", "memory");
}

/* Only our own BRK #0xf00d is skipped (x0 = 0 means "nobody answered").
 * Anything else, e.g. a Swift runtime trap (BRK #1), gets the default action
 * back so the app crashes with a real report instead of running on. */
static void sigtrap_handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    ucontext_t *uc = (ucontext_t *)context;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    if ((pc & 3) || *(const uint32_t *)(uintptr_t)pc != 0xd43e01a0u /* brk #0xf00d */) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(sig, &dfl, NULL);
        return;
    }
    uc->uc_mcontext->__ss.__pc += 4;
    uc->uc_mcontext->__ss.__x[0] = 0;
}

void mid_jit_arm_trap_fallback(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigtrap_handler;
    sigaction(SIGTRAP, &sa, NULL);
}

// ------------------------------------------------------------------ pool --

static void *g_rx;
static void *g_rw;
static size_t g_size;
static _Atomic size_t g_used;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    mid_log("[jit] %s", err);
}

static void try_no_footprint(void *addr, size_t size) {
    /* Pool pages count against the jetsam limit unless the kernel accepts an
     * ownership change. Madeira found plain/self or plain/TASK_NULL entries to be
     * the shapes the kernel takes; try both, log the footprint either side. */
    mach_port_t task = mach_task_self();
    uint64_t before = mid_phys_footprint();
    const struct { unsigned flags; int self; const char *name; } v[] = {
        { VM_PROT_READ | VM_PROT_WRITE, 0, "plain/null" },
        { VM_PROT_READ | VM_PROT_WRITE, 1, "plain/self" },
        { MAP_MEM_VM_SHARE | VM_PROT_READ | VM_PROT_WRITE, 1, "share/self" },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        memory_object_size_t esz = size;
        mach_port_t entry = MACH_PORT_NULL;
        kern_return_t kr = mach_make_memory_entry_64(task, &esz, (memory_object_offset_t)(uintptr_t)addr,
                                                     v[i].flags, &entry, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS || entry == MACH_PORT_NULL) continue;
        kr = mach_memory_entry_ownership(entry, v[i].self ? task : TASK_NULL, MID_LEDGER_TAG_DEFAULT,
                                         MID_LEDGER_FLAG_NO_FOOTPRINT);
        mach_port_deallocate(task, entry);
        if (kr == KERN_SUCCESS) {
            mid_log("[jit] pool off jetsam footprint via %s (footprint %llu MB -> %llu MB)", v[i].name,
                    before >> 20, mid_phys_footprint() >> 20);
            return;
        }
    }
    mid_log("[jit] pool stays on jetsam footprint (ownership refused); keep the pool moderate");
}

// ----------------------------------------------------------- address space --
/* Same layout as Madeira (JITAllocator.c, ml1040): claim, at image load and
 * before any runtime allocation can fragment it,
 *   - [0x140000000, +128 MB): where non-relocatable x64 .exe images must load
 *     (handed to Wine via WINE_IOS_EXE_WINDOW), and
 *   - the largest run directly above it, as a placeholder for the JIT pool.
 * Both are PROT_NONE reservations and cost no memory. The debugger allocates
 * the pool first-fit with no address hint, so the placeholder is released just
 * before asking and lower holes that could win are plugged meanwhile; a low
 * pool keeps translated code within branch reach of the images. */
static const vm_address_t kExeWin = 0x140000000ul, kExeWinSize = 0x8000000ul;
static vm_address_t g_win_base, g_ph_base;
static vm_size_t g_ph_size;

__attribute__((constructor(101), used)) static void mid_early_va_claim(void) {
    vm_address_t a = kExeWin;
    if (vm_allocate(mach_task_self(), &a, kExeWinSize, VM_FLAGS_FIXED) == KERN_SUCCESS && a == kExeWin) {
        vm_protect(mach_task_self(), a, kExeWinSize, 0, VM_PROT_NONE);
        g_win_base = a;
    }
    for (vm_size_t sz = 1280ul << 20; sz >= (256ul << 20); sz -= (16ul << 20)) {
        a = kExeWin + kExeWinSize;
        if (vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_FIXED) == KERN_SUCCESS && a == kExeWin + kExeWinSize) {
            vm_protect(mach_task_self(), a, sz, 0, VM_PROT_NONE);
            g_ph_base = a;
            g_ph_size = sz;
            break;
        }
    }
}

bool mid_exe_window(uint64_t *base, uint64_t *size) {
    if (!g_win_base) return false;
    *base = g_win_base;
    *size = kExeWinSize;
    return true;
}

/* Plug free gaps below `limit` that are at least `need` bytes, so a first-fit
 * allocation cannot land there. Returns how many plugs were placed. */
static int plug_low_holes(vm_address_t limit, vm_size_t need, vm_address_t *plugs, vm_size_t *sizes, int max) {
    int n = 0;
    vm_address_t addr = 0x100000000ul, prev_end = 0x100000000ul;
    while (addr < limit && n < max) {
        vm_address_t ra = addr;
        vm_size_t rs = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &ra, &rs, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        vm_address_t gap_end = ra < limit ? ra : limit;
        if (gap_end > prev_end && gap_end - prev_end >= need) {
            vm_address_t p = prev_end;
            if (vm_allocate(mach_task_self(), &p, gap_end - prev_end, VM_FLAGS_FIXED) == KERN_SUCCESS) {
                plugs[n] = p;
                sizes[n] = gap_end - prev_end;
                n++;
            }
        }
        prev_end = ra + rs;
        addr = ra + rs;
    }
    return n;
}

bool mid_jit_pool_create(size_t size, char *err, size_t errlen) {
    pthread_mutex_lock(&g_pool_lock);
    if (g_rx) {
        pthread_mutex_unlock(&g_pool_lock);
        return true;
    }
    size = (size + MID_PAGE - 1) & ~(MID_PAGE - 1);
    if (!mid_jit_debugged()) {
        set_err(err, errlen, "No debugger: CS_DEBUGGED is not set. Enable JIT with StikDebug first.");
        pthread_mutex_unlock(&g_pool_lock);
        return false;
    }
    mid_jit_arm_trap_fallback();

    vm_address_t plugs[16];
    vm_size_t plug_sizes[16];
    int nplugs = 0;
    /* Like Madeira: keep the pool low, next to the images, even if that means a
     * smaller pool than asked for (its log: 608/592/448 MB pools by launch). */
    if (g_ph_base && g_ph_size < size && g_ph_size >= (256ul << 20)) {
        mid_log("[jit] only %zu MB free above the exe window; using that instead of %zu MB",
                (size_t)(g_ph_size >> 20), size >> 20);
        size = g_ph_size;
    }
    if (g_ph_base && g_ph_size >= size) {
        nplugs = plug_low_holes(g_ph_base, size, plugs, plug_sizes, 16);
        vm_deallocate(mach_task_self(), g_ph_base, g_ph_size);
        mid_log("[jit] released pool placeholder %p+%zu MB, plugged %d lower holes", (void *)g_ph_base,
                (size_t)(g_ph_size >> 20), nplugs);
    }
    mid_log("[jit] asking the debugger for %zu MB of RX memory", size >> 20);
    void *rx = mid_jit26_prepare_region(NULL, size);
    for (int i = 0; i < nplugs; i++) vm_deallocate(mach_task_self(), plugs[i], plug_sizes[i]);
    if (!rx) {
        set_err(err, errlen,
                "The debugger did not answer the prepare request. Use StikDebug's \"Enable JIT\" from "
                "inside MYIOSDECK (it sends our script), not StikDebug's own app list.");
        pthread_mutex_unlock(&g_pool_lock);
        return false;
    }

    mach_port_t task = mach_task_self();
    vm_address_t rw = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(task, &rw, size, 0, VM_FLAGS_ANYWHERE, task, (vm_address_t)rx, FALSE, &cur,
                                &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        set_err(err, errlen, "vm_remap for the RW alias failed: %s (%d)", mach_error_string(kr), kr);
        pthread_mutex_unlock(&g_pool_lock);
        return false;
    }
    kr = vm_protect(task, rw, size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(task, rw, size);
        set_err(err, errlen, "vm_protect(RW) on the alias failed: %s (%d)", mach_error_string(kr), kr);
        pthread_mutex_unlock(&g_pool_lock);
        return false;
    }

    /* Coherence: what we write through RW must be visible through RX. */
    volatile uint32_t *probe_rw = (uint32_t *)rw;
    *probe_rw = 0xCAFEBABEu;
    if (*(volatile uint32_t *)rx != 0xCAFEBABEu) {
        vm_deallocate(task, rw, size);
        set_err(err, errlen, "RW and RX views are not coherent");
        pthread_mutex_unlock(&g_pool_lock);
        return false;
    }
    *probe_rw = 0;

    g_rx = rx;
    g_rw = (void *)rw;
    g_size = size;
    atomic_store(&g_used, MID_PAGE); /* first page reserved for the self-test */
    pthread_mutex_unlock(&g_pool_lock);

    mid_log("[jit] pool ready: RX=%p RW=%p size=%zu MB offset=%lld", g_rx, g_rw, g_size >> 20,
            (long long)mid_jit_pool_write_offset());
    try_no_footprint(g_rx, g_size);
    return true;
}

bool mid_jit_pool_ready(void) { return g_rx != NULL; }
void *mid_jit_pool_rx_base(void) { return g_rx; }
void *mid_jit_pool_rw_base(void) { return g_rw; }
size_t mid_jit_pool_size(void) { return g_size; }
size_t mid_jit_pool_used(void) { return atomic_load(&g_used); }

int64_t mid_jit_pool_write_offset(void) {
    if (!g_rx) return 0;
    return (int64_t)((intptr_t)g_rw - (intptr_t)g_rx);
}

void *mid_jit_pool_alloc(size_t size) {
    if (!g_rx) return NULL;
    size = (size + MID_PAGE - 1) & ~(MID_PAGE - 1);
    size_t off = atomic_fetch_add(&g_used, size);
    if (off + size > g_size) {
        mid_log("[jit] pool exhausted: wanted %zu KB at %zu MB of %zu MB", size >> 10, off >> 20, g_size >> 20);
        return NULL;
    }
    return (uint8_t *)g_rx + off;
}

int64_t mid_jit_selftest(void) {
    if (!g_rx) return -1;
    static const uint32_t code[] = {
        0xD2800540u, /* mov x0, #42 */
        0xD65F03C0u, /* ret */
    };
    memcpy(g_rw, code, sizeof code);
    sys_icache_invalidate(g_rx, sizeof code);
    int64_t (*fn)(void) = (int64_t (*)(void))g_rx;
    return fn();
}

// ---------------------------------------------------------------- memory --

uint64_t mid_available_memory(void) { return (uint64_t)os_proc_available_memory(); }

uint64_t mid_phys_footprint(void) {
    task_vm_info_data_t vmi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) != KERN_SUCCESS) return 0;
    return vmi.phys_footprint;
}

uint64_t mid_task_max_address(void) {
    task_vm_info_data_t vmi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) != KERN_SUCCESS) return 0;
    return vmi.max_address;
}
