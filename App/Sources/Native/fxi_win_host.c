// SPDX-License-Identifier: GPL-3.0-or-later
// FXI as Wine's x64 CPU without JIT (docs/NO_JIT_WINDOWS.md step D): what the emulator DLL
// (engine/pedylib/emu, loaded by Wine as xtajit64.dll) calls into. Wine's no-JIT map hook
// (engine/wine/patches/nojit_dylib.py) stores mid_fxi_win_host_table() into that DLL when it
// maps it; the DLL's exports branch through the table to fxi_win_glue.S and to the C here.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach/mach.h>
#include <pthread.h>
#include <time.h>

#include "fxi.h"
#include "jit_core.h"

#if __has_include("wine_version.h")
#include "wine_version.h"
#endif

// Byte offset of the TEB in the TSD array (TPIDRRO_EL0 & ~7): Wine's ios_teb_tls_slot_offset.
int fxi_win_tsd_offset;

// Order and size are the emulator DLL's contract (engine/pedylib/emu/xtajit64_stub.c).
typedef struct {
    void *exit_to_x64;          // 0x00 [ExitToX64]
    void *dispatch_jump;        // 0x08 [DispatchJump]
    void *ret_to_entry_thunk;   // 0x10 [RetToEntryThunk]
    void *begin_simulation;     // 0x18 [BeginSimulation]
    void *process_init;         // 0x20 long (KiUserExceptionDispatcher, native entry)
    void *thread_init;          // 0x28 long (void)
    void *feature_present;      // 0x30 long (unsigned feature)
    void *reset_to_consistent;  // 0x38 long (EXCEPTION_RECORD *, ARM64_NT_CONTEXT *)
} mid_fxi_win_host;

void fxi_win_exit_to_x64(void);
void fxi_win_dispatch_jump(void);
void fxi_win_ret_to_entry_thunk(void);
void fxi_win_begin_simulation(void);
void __attribute__((noreturn)) fxi_win_jump_stack(void *sp, uint64_t pc);   // fxi_win_glue.S

static uint8_t *teb_now(void) {
    uint64_t tsd;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(tsd));
    return *(uint8_t **)((tsd & ~7ull) + (uint64_t)fxi_win_tsd_offset);
}
static uint8_t *cpu_area_now(void) {
    uint8_t *teb = teb_now();
    return teb ? *(uint8_t **)(teb + 0x1788) : NULL;           // ChpeV2CpuAreaInfo
}

// ntdll's KiUserExceptionDispatcher (its native ARM64EC entry), from the emulator DLL.
static uint64_t g_ki_dispatcher;

static long host_process_init(uint64_t ki_dispatcher) {
    // iOS maps nothing below 4 GB (__PAGEZERO): a smaller value is a broken pointer (build 63
    // passed a 32-bit-truncated one); leave exceptions disabled rather than crash on it.
    if (ki_dispatcher < 0x100000000ull) {
        mid_log("[fxi-win] FXI is the x64 CPU of this Windows process (no JIT); exception dispatcher %#llx "
                "UNEXPECTED (not a valid address): x64 exceptions disabled", (unsigned long long)ki_dispatcher);
        return 0;
    }
    g_ki_dispatcher = ki_dispatcher;
    // Its first instruction is `sub sp, sp, #0x4d0` (d11343ff) when this is the native entry.
    uint32_t first = *(const uint32_t *)(uintptr_t)ki_dispatcher;
    mid_log("[fxi-win] FXI is the x64 CPU of this Windows process (no JIT); exception dispatcher %#llx "
            "(first insn %08x%s)", (unsigned long long)ki_dispatcher, first,
            first == 0xd11343ffu ? ", native entry ok" : ", UNEXPECTED: x64 exceptions disabled");
    if (first != 0xd11343ffu) g_ki_dispatcher = 0;
    return 0;
}

// ---- x64 exceptions ----
// An exception in interpreted x64 code (a CPU exception FXI raises, or a host fault while it
// runs) is dispatched as Windows would for x64 code: the x64 state is packed into an ARM64EC
// context (x64 registers in the ARM64EC mapping, Pc = the x64 rip) and ntdll's
// KiUserExceptionDispatcher runs on the guest stack, below RSP. Wine then unwinds x64 frames
// with their .pdata, calls x64 handlers through the emulator, and continues x64 code through
// BeginSimulation. The same scheme as FEX (Source/Windows/ARM64EC/Module.cpp,
// RethrowGuestException).

// The stack layout Wine's ARM64EC KiUserExceptionDispatcher expects at sp.
typedef struct __attribute__((aligned(16))) {
    uint8_t context[0x390];     // ARM64_NT_CONTEXT
    uint64_t pad[4];
    uint8_t rec[0x98];          // EXCEPTION_RECORD
    uint64_t align;
    uint64_t redzone[2];
} ki_layout;
_Static_assert(__builtin_offsetof(ki_layout, rec) == 0x3b0, "KiUserExceptionDispatcher layout");

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// AMD64 CONTEXT -> ARM64_NT_CONTEXT in the ARM64EC register mapping (x87 registers in the
// MMX slots: Lr X6 X7 X9 / X10 X11 X12 X15, exponents packed in X16 / X17).
static void pack_ec_context(uint8_t *a, const uint8_t *x) {
    static const struct { uint16_t amd64, arm; } map[] = {
        { 0x78, 8 + 8 * 8 },  { 0x80, 8 + 0 * 8 },  { 0x88, 8 + 1 * 8 },  { 0x90, 8 + 27 * 8 },   // rax rcx rdx rbx
        { 0x98, 0x100 },      { 0xa0, 0xf0 },       { 0xa8, 8 + 25 * 8 }, { 0xb0, 8 + 26 * 8 },   // rsp rbp rsi rdi
        { 0xb8, 8 + 2 * 8 },  { 0xc0, 8 + 3 * 8 },  { 0xc8, 8 + 4 * 8 },  { 0xd0, 8 + 5 * 8 },    // r8-r11
        { 0xd8, 8 + 19 * 8 }, { 0xe0, 8 + 20 * 8 }, { 0xe8, 8 + 21 * 8 }, { 0xf0, 8 + 22 * 8 },   // r12-r15
        { 0xf8, 0x108 },                                                                           // rip -> Pc
    };
    memset(a, 0, 0x390);
    uint32_t flags = 0x400007;   // CONTEXT_ARM64_FULL
    memcpy(a, &flags, 4);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) wr64(a + map[i].arm, rd64(x + map[i].amd64));
    memcpy(a + 0x110, x + 0x1a0, 16 * 16);   // xmm0-15 -> v0-15
    static const uint16_t mm_lo[8] = { 0xf8, 8 + 6 * 8, 8 + 7 * 8, 8 + 9 * 8, 8 + 10 * 8, 8 + 11 * 8, 8 + 12 * 8, 8 + 15 * 8 };
    uint64_t hi[2] = { 0, 0 };
    for (int i = 0; i < 8; i++) {
        const uint8_t *st = x + 0x120 + 16 * i;   // FltSave.FloatRegisters[i]
        wr64(a + mm_lo[i], rd64(st));
        uint16_t e; memcpy(&e, st + 8, 2);
        hi[i >> 2] |= (uint64_t)e << (16 * (i & 3));
    }
    wr64(a + 8 + 16 * 8, hi[0]);
    wr64(a + 8 + 17 * 8, hi[1]);
    uint32_t ef; memcpy(&ef, x + 0x44, 4);
    uint32_t cpsr = (ef & 0x100 ? 1u << 21 : 0) | (ef & 0x800 ? 1u << 28 : 0) | (ef & 0x1 ? 1u << 29 : 0) |
                    (ef & 0x40 ? 1u << 30 : 0) | (ef & 0x80 ? 1u << 31 : 0);
    memcpy(a + 4, &cpsr, 4);
    uint64_t fpcr, fpsr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    uint32_t f32[2] = { (uint32_t)fpcr, (uint32_t)fpsr };
    memcpy(a + 0x310, f32, 8);
}

static void __attribute__((noreturn)) fatal(const char *what);

// Hex of guest code [a, a+n), clamped to a's 16 KB page. Copied with vm_read_overwrite: an
// unreadable address (a bad jump target) is reported, not faulted on.
static void log_code(const char *label, uint64_t a, unsigned n) {
    uint64_t page_end = (a | 0x3fffull) + 1;
    if (a + n > page_end) n = (unsigned)(page_end - a);
    if (n > 192) n = 192;
    uint8_t buf[192];
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)buf, &got) != KERN_SUCCESS) {
        mid_log("[fxi-win]   %s %#llx: (unreadable)", label, (unsigned long long)a);
        return;
    }
    char hex[3 * 192 + 1];
    for (unsigned i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
    hex[3 * n] = 0;
    mid_log("[fxi-win]   %s %#llx: %s", label, (unsigned long long)a, hex);
}

// The first exceptions of a process: registers, the code before the faulting instruction and
// the last first-time control-flow edges, enough to find the instruction that went wrong.
static void log_exception_state(FxiCpu *c, const uint8_t *ctx64, uint64_t rip) {
    static const char *names[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                     "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    for (int i = 0; i < 16; i += 4)
        mid_log("[fxi-win]   %-3s %016llx  %-3s %016llx  %-3s %016llx  %-3s %016llx",
                names[i], (unsigned long long)rd64(ctx64 + 0x78 + 8 * i),
                names[i + 1], (unsigned long long)rd64(ctx64 + 0x80 + 8 * i),
                names[i + 2], (unsigned long long)rd64(ctx64 + 0x88 + 8 * i),
                names[i + 3], (unsigned long long)rd64(ctx64 + 0x90 + 8 * i));
    uint32_t ef; memcpy(&ef, ctx64 + 0x44, 4);
    mid_log("[fxi-win]   eflags %08x", ef);
    uint64_t df_rip;
    int df_how = fxi_win_df_source(c, &df_rip);
    static const char *df_names[4] = { "never set", "std", "popf", "a context load" };
    mid_log("[fxi-win]   DF %u, last set by %s at %#llx", (ef >> 10) & 1, df_names[df_how & 3], (unsigned long long)df_rip);
    if (df_how) log_code("df code", df_rip > 48 ? df_rip - 48 : df_rip, 64);
    uint64_t from = rip - 160 < (rip & ~0x3fffull) ? (rip & ~0x3fffull) : rip - 160;
    log_code("code before", from, (unsigned)(rip - from));
    log_code("code at", rip, 32);
    uint64_t t[16];
    int n = fxi_win_trail(c, t, 16);
    for (int i = 0; i < n; i++) {
        char label[24];
        snprintf(label, sizeof label, "edge %d/%d", i + 1, n);
        log_code(label, t[i], 48);
    }
}

static void __attribute__((noreturn)) raise_x64(FxiCpu *c, uint64_t rip, uint32_t code, uint32_t flags,
                                                uint32_t nparams, const uint64_t *info, const char *why) {
    static int logged;
    if (logged < 64) {
        logged++;
        mid_log("[fxi-win] x64 exception %08x at %#llx (%s, info %#llx %#llx)", code, (unsigned long long)rip, why,
                (unsigned long long)(nparams > 0 ? info[0] : 0), (unsigned long long)(nparams > 1 ? info[1] : 0));
    }
    if (!g_ki_dispatcher) fatal("x64 exception but no exception dispatcher (ProcessInit did not pass it)");
    // Out of the simulation first: a fault writing the frame (a blown guest stack) must not
    // come back here.
    uint8_t *area = cpu_area_now();
    if (area) area[0] = 0;                       // InSimulation
    uint8_t ctx64[0x4d0];
    fxi_win_save_context(c, ctx64, rip);
    static int dumped;
    if (dumped < 3) { dumped++; log_exception_state(c, ctx64, rip); }
    ki_layout *k = (ki_layout *)(uintptr_t)(rd64(ctx64 + 0x98) & ~63ull) - 1;   // below RSP
    memset(k, 0, sizeof *k);
    pack_ec_context(k->context, ctx64);
    memcpy(k->rec, &code, 4);
    memcpy(k->rec + 4, &flags, 4);
    wr64(k->rec + 0x10, rip);                    // ExceptionAddress
    memcpy(k->rec + 0x18, &nparams, 4);
    for (uint32_t i = 0; i < nparams && i < 2; i++) wr64(k->rec + 0x20 + 8 * i, info[i]);
    fxi_win_jump_stack(k, g_ki_dispatcher);
}

// [ResetToConsistentState] Wine's exception path asks the emulator first. A fault inside FXI
// (InSimulation set) becomes an exception of the x64 instruction that faulted.
static long host_reset_to_consistent(const uint8_t *rec, const uint8_t *arm_ctx) {
    uint8_t *area = cpu_area_now();
    if (!area || !area[0]) return 0;             // native code: Wine's own dispatch
    FxiCpu *c = *(FxiCpu **)(area + 0x30);
    if (!c) return 0;
    uint32_t code, flags, nparams;
    memcpy(&code, rec, 4); memcpy(&flags, rec + 4, 4); memcpy(&nparams, rec + 0x18, 4);
    uint64_t info[2] = { nparams > 0 ? rd64(rec + 0x20) : 0, nparams > 1 ? rd64(rec + 0x28) : 0 };
    int fetch;
    uint64_t rip = fxi_win_fault_rip(c, &fetch);
    if (nparams > 2) nparams = 2;
    if (fetch && code == 0xC0000005) { info[0] = 8; info[1] = rip; nparams = 2; }   // execute fault
    char why[96];
    snprintf(why, sizeof why, "host fault at pc %#llx%s", (unsigned long long)rd64(arm_ctx + 0x108),
             fetch ? ", fetching code" : "");
    raise_x64(c, rip, code, flags, nparams, info, why);
}

// ---- Wild guest pointers (build 78) ----
// A wild guest pointer faults inside an FXI handler. Madeira's Mach handler then looks for the
// thread's TEB by stack containment; FXI's emulator stack is not the TEB's stack, so it fell
// back to a best-effort delivery that worked once (build 76) and left the thread stuck the
// next time (build 77). Madeira's Mach server now asks FXI first (mid_fxi_mach_fault, called
// before its last-resort delivery, engine/wine/patches/nojit_fault_hook.py): when the thread
// is in simulation and the address is not mapped at all (wild or null pointers, non-canonical
// values), the thread is redirected to fault_entry, which raises the x64 access violation the
// way host_reset_to_consistent does. Everything else stays Madeira's.
#define FAULT_THREADS 1024
// cpu is set once the thread has one (FxiCpus are never freed); area and teb belong to Wine and
// are gone once the thread exits, so the profiler reads them only with vm_read_overwrite.
static struct { mach_port_t thread; uint8_t *area, *teb; FxiCpu *cpu; } g_fault_threads[FAULT_THREADS];

static uint8_t *fault_thread_area(mach_port_t thread) {
    for (int i = 0; i < FAULT_THREADS; i++)
        if (__atomic_load_n(&g_fault_threads[i].thread, __ATOMIC_ACQUIRE) == thread) return g_fault_threads[i].area;
    return NULL;
}

static void fault_thread_register(uint8_t *area, uint8_t *teb, FxiCpu *cpu) {
    mach_port_t self = mach_thread_self();
    int slot = -1;
    for (int i = 0; i < FAULT_THREADS && slot < 0; i++)
        if (g_fault_threads[i].thread == self) slot = i;   // a reused port name: a new thread
    for (int i = 0; i < FAULT_THREADS && slot < 0; i++) {
        mach_port_t expect = MACH_PORT_NULL;
        if (__atomic_compare_exchange_n(&g_fault_threads[i].thread, &expect, self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            slot = i;
    }
    if (slot < 0) return;
    g_fault_threads[slot].area = area;
    g_fault_threads[slot].teb = teb;
    g_fault_threads[slot].cpu = cpu;
    __atomic_store_n(&g_fault_threads[slot].thread, self, __ATOMIC_RELEASE);
}

// ---- Sampling profiler (build 87) ----
// Where does a no-JIT game spend its time? A host thread samples every running FXI thread
// 500 times a second: in x64 code (InSimulation) it records the rip of the instruction that
// last touched memory (FXI keeps it for exact faults), otherwise it counts native (Wine, the
// D3D/Metal DLLs). Every 10 s it logs [fxi-prof] lines: x64 vs native time per busy thread,
// block lookups and calls into native code per second, x64 time per module (from the PEB's
// loader list; Mono's generated code is outside every module) and the hottest instructions.
// Sampling is racy by design and costs the game nothing (one Mach call per running thread).
#define PROF_HZ 500
#define PROF_PERIOD_S 10
#define PROF_SLOTS 16384
typedef struct { uint64_t rip, n; } prof_hit;
static prof_hit g_prof_hits[PROF_SLOTS];
static struct { uint64_t x64, native, last_lookups, last_exits; } g_prof_thr[FAULT_THREADS];

static int vmr(uint64_t a, void *out, size_t n) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}

typedef struct { uint64_t base, size, x64; char name[48]; } prof_mod;
// The process's modules, from PEB->Ldr->InLoadOrderModuleList (read without faulting).
static int prof_modules(uint8_t *teb, prof_mod *m, int max) {
    uint64_t peb, ldr, head, e;
    if (!vmr((uint64_t)(uintptr_t)teb + 0x60, &peb, 8) || !vmr(peb + 0x18, &ldr, 8)) return 0;
    head = ldr + 0x10;
    if (!vmr(head, &e, 8)) return 0;
    int n = 0;
    for (int guard = 0; e && e != head && n < max && guard < 512; guard++) {
        uint64_t base, size, buf, next; uint16_t len;
        if (!vmr(e + 0x30, &base, 8) || !vmr(e + 0x40, &size, 8) || !vmr(e + 0x58, &len, 2) ||
            !vmr(e + 0x60, &buf, 8) || !vmr(e, &next, 8)) break;
        uint32_t s32 = (uint32_t)size;
        m[n].base = base; m[n].size = s32; m[n].x64 = 0;
        uint16_t w[47]; unsigned chars = len / 2 < 47 ? len / 2 : 47;
        if (!chars || !vmr(buf, w, chars * 2)) chars = 0;
        for (unsigned i = 0; i < chars; i++) m[n].name[i] = (char)(w[i] < 128 ? w[i] : '?');
        m[n].name[chars] = 0;
        n++;
        e = next;
    }
    return n;
}

static int prof_cmp_hit(const void *a, const void *b) {
    uint64_t x = ((const prof_hit *)a)->n, y = ((const prof_hit *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void prof_report(uint64_t period_samples) {
    static prof_hit sorted[PROF_SLOTS];
    static prof_mod mods[256];
    uint8_t *teb = NULL;
    uint64_t x64 = 0, native = 0, blocks = 0;
    for (int i = 0; i < FAULT_THREADS; i++) {
        if (!g_fault_threads[i].thread) continue;
        if (!teb) teb = g_fault_threads[i].teb;
        x64 += g_prof_thr[i].x64; native += g_prof_thr[i].native;
    }
    if (!x64 && !native) return;
    mid_log("[fxi-prof] last %d s: %llu samples of running FXI threads, x64 %.1f%%, native %.1f%%",
            PROF_PERIOD_S, (unsigned long long)(x64 + native), 100.0 * x64 / (double)(x64 + native),
            100.0 * native / (double)(x64 + native));
    // Busiest threads: running share of the period, x64 vs native, lookups and native calls per second.
    for (int shown = 0; shown < 6; shown++) {
        int best = -1; uint64_t bestn = 0;
        for (int i = 0; i < FAULT_THREADS; i++) {
            uint64_t t = g_prof_thr[i].x64 + g_prof_thr[i].native;
            if (g_fault_threads[i].thread && t > bestn) { bestn = t; best = i; }
        }
        if (best < 0 || bestn * 50 < period_samples) break;   // under 2% of the period: skip
        FxiCpu *c = g_fault_threads[best].cpu;
        uint64_t lk = 0, ex = 0;
        if (c) fxi_win_profile(c, &lk, &ex, &blocks);
        mid_log("[fxi-prof]   thread TEB %p: running %.0f%% of the time, x64 %.0f%% / native %.0f%%, "
                "%llu lookups/s, %llu native calls/s", (void *)g_fault_threads[best].teb,
                100.0 * bestn / (double)period_samples, 100.0 * g_prof_thr[best].x64 / (double)bestn,
                100.0 * g_prof_thr[best].native / (double)bestn,
                (unsigned long long)((lk - g_prof_thr[best].last_lookups) / PROF_PERIOD_S),
                (unsigned long long)((ex - g_prof_thr[best].last_exits) / PROF_PERIOD_S));
        g_prof_thr[best].x64 = g_prof_thr[best].native = 0;   // shown: do not pick it again
    }
    for (int i = 0; i < FAULT_THREADS; i++) {                 // counters for the next period
        if (!g_fault_threads[i].thread) continue;
        FxiCpu *c = g_fault_threads[i].cpu;
        if (c) fxi_win_profile(c, &g_prof_thr[i].last_lookups, &g_prof_thr[i].last_exits, &blocks);
        g_prof_thr[i].x64 = g_prof_thr[i].native = 0;
    }
    // x64 time per module, then the hottest instructions.
    int nm = teb ? prof_modules(teb, mods, 256) : 0;
    int nh = 0; uint64_t other = 0;
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof_hits[i].n) continue;
        sorted[nh++] = g_prof_hits[i];
        int found = 0;
        for (int k = 0; k < nm; k++)
            if (g_prof_hits[i].rip - mods[k].base < mods[k].size) { mods[k].x64 += g_prof_hits[i].n; found = 1; break; }
        if (!found) other += g_prof_hits[i].n;
    }
    memset(g_prof_hits, 0, sizeof g_prof_hits);
    if (!x64) return;
    char line[512]; int o = 0;
    for (int shown = 0; shown < 10; shown++) {
        int best = -1;
        for (int k = 0; k < nm; k++) if (mods[k].x64 && (best < 0 || mods[k].x64 > mods[best].x64)) best = k;
        if (best < 0) break;
        o += snprintf(line + o, sizeof line - (size_t)o, "%s%s %.1f%%", shown ? ", " : "", mods[best].name,
                      100.0 * mods[best].x64 / (double)x64);
        mods[best].x64 = 0;
        if (o > 400) break;
    }
    mid_log("[fxi-prof]   x64 time by module: %s%s(no module: generated/JIT code) %.1f%%", line, o ? ", " : "",
            100.0 * other / (double)x64);
    qsort(sorted, (size_t)nh, sizeof sorted[0], prof_cmp_hit);
    for (int i = 0; i < nh && i < 16; i++) {
        const char *mod = "(generated)"; uint64_t off = sorted[i].rip;
        for (int k = 0; k < nm; k++)
            if (sorted[i].rip - mods[k].base < mods[k].size) { mod = mods[k].name; off = sorted[i].rip - mods[k].base; break; }
        uint8_t code[16]; char hex[49] = "?";
        if (vmr(sorted[i].rip, code, 16)) for (int b = 0; b < 16; b++) snprintf(hex + 3 * b, 4, "%02x ", code[b]);
        mid_log("[fxi-prof]   hot %2d: %5.2f%% %s+%#llx (%#llx): %s", i + 1, 100.0 * sorted[i].n / (double)x64, mod,
                (unsigned long long)off, (unsigned long long)sorted[i].rip, hex);
    }
    mid_log("[fxi-prof]   blocks translated so far: %llu", (unsigned long long)blocks);
}

static void *prof_thread(void *arg) {
    (void)arg;
    uint64_t ticks = 0;
    for (;;) {
        struct timespec ts = { 0, 1000000000 / PROF_HZ };
        nanosleep(&ts, NULL);
        for (int i = 0; i < FAULT_THREADS; i++) {
            mach_port_t t = __atomic_load_n(&g_fault_threads[i].thread, __ATOMIC_ACQUIRE);
            if (!t) continue;
            thread_basic_info_data_t bi;
            mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS || bi.run_state != TH_STATE_RUNNING)
                continue;
            // The thread may have exited and Wine freed its CPU area (build 87 crashed here
            // reading it directly): read InSimulation through vm_read_overwrite.
            FxiCpu *c = g_fault_threads[i].cpu;
            uint8_t insim;
            if (!c || !vmr((uint64_t)(uintptr_t)g_fault_threads[i].area, &insim, 1)) continue;
            if (!insim) { g_prof_thr[i].native++; continue; }
            g_prof_thr[i].x64++;
            uint64_t lk, ex, bl, rip = fxi_win_profile(c, &lk, &ex, &bl);
            uint64_t h = (rip * 0x9E3779B97F4A7C15ull) >> 50;
            for (int probe = 0; probe < 32; probe++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof_hits[h].rip == rip) { g_prof_hits[h].n++; break; }
                if (!g_prof_hits[h].n) { g_prof_hits[h].rip = rip; g_prof_hits[h].n = 1; break; }
            }
        }
        if (++ticks % (PROF_HZ * PROF_PERIOD_S) == 0) prof_report(PROF_HZ * PROF_PERIOD_S);
    }
    return NULL;
}

static void prof_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, prof_thread, NULL) == 0) pthread_detach(t);
}

static void __attribute__((noreturn, used)) fault_entry(uint64_t addr, uint64_t is_write) {
    uint8_t *area = cpu_area_now();
    FxiCpu *c = area ? *(FxiCpu **)(area + 0x30) : NULL;
    if (!c) fatal("wild pointer: no FXI CPU on the faulting thread");
    int fetch;
    uint64_t rip = fxi_win_fault_rip(c, &fetch);
    uint64_t info[2] = { fetch ? 8 : is_write, fetch ? rip : addr };
    char why[96];
    snprintf(why, sizeof why, "unmapped address%s, via the Mach hook", fetch ? ", fetching code" : "");
    raise_x64(c, rip, 0xC0000005, 0, 2, info, why);
}

static int address_mapped(uint64_t a) {
    vm_address_t r = (vm_address_t)a;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &r, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return 0;
    return r <= a;   // the region found starts at or below a: a lies in it
}

// Called by Madeira's Mach exception server for an EXC_BAD_ACCESS nothing else claimed, with
// the faulting thread suspended. 1 = *state now enters fault_entry (Madeira writes it back).
int mid_fxi_mach_fault(mach_port_t thread, arm_thread_state64_t *state, uint64_t fault_addr) {
    uint8_t *area = fault_thread_area(thread);
    if (!area || !area[0]) return 0;                       // not in simulation
    if (address_mapped(fault_addr)) return 0;              // guard page, write-watch, ...
    arm_exception_state64_t es;
    mach_msg_type_number_t en = ARM_EXCEPTION_STATE64_COUNT;
    uint64_t is_write = 0;
    if (thread_get_state(thread, ARM_EXCEPTION_STATE64, (thread_state_t)&es, &en) == KERN_SUCCESS)
        is_write = (es.__esr >> 6) & 1;                     // ESR WnR (data abort)
    state->__x[0] = fault_addr;
    state->__x[1] = is_write;
    arm_thread_state64_set_lr_fptr(*state, (void *)0);
    arm_thread_state64_set_sp(*state, arm_thread_state64_get_sp(*state) & ~15ull);
    arm_thread_state64_set_pc_fptr(*state, (void *)fault_entry);
    static int logged;
    if (logged < 8) {
        logged++;
        mid_log("[fxi-win] wild pointer %#llx in simulation: raised as an x64 access violation", (unsigned long long)fault_addr);
    }
    return 1;
}

static long host_thread_init(void) {
    uint8_t *teb = teb_now();
    uint8_t *area = teb ? *(uint8_t **)(teb + 0x1788) : NULL;   // ChpeV2CpuAreaInfo
    if (!area) {
        mid_log("[fxi-win] ThreadInit: no CPU area (teb %p)", (void *)teb);
        return (long)0xC0000001;   // STATUS_UNSUCCESSFUL
    }
    FxiCpu **slot = (FxiCpu **)(area + 0x30);                   // EmulatorData[0]
    if (!*slot) {
        *slot = fxi_win_cpu_new();
        fxi_win_set_teb(*slot, (uint64_t)(uintptr_t)teb);
    }
    fault_thread_register(area, teb, *slot);
    static pthread_once_t prof_once = PTHREAD_ONCE_INIT;
    pthread_once(&prof_once, prof_start);
    mid_log("[fxi-win] thread ready: TEB %p, CPU area %p, emulator stack %p", (void *)teb, (void *)area,
            *(void **)(area + 0x8));
    return *slot ? 0 : (long)0xC0000017;                        // STATUS_NO_MEMORY
}

// x64 features Windows code may ask about (IsProcessorFeaturePresent): what FXI implements.
static long host_feature_present(unsigned feature) {
    switch (feature) {
    case 6:  // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 10: // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
        return 1;
    default:
        return 0;
    }
}

void *mid_fxi_win_host_table(int tsd_offset) {
    static mid_fxi_win_host table = {
        (void *)fxi_win_exit_to_x64,       (void *)fxi_win_dispatch_jump,  (void *)fxi_win_ret_to_entry_thunk,
        (void *)fxi_win_begin_simulation,  (void *)host_process_init,      (void *)host_thread_init,
        (void *)host_feature_present,      (void *)host_reset_to_consistent,
    };
    fxi_win_tsd_offset = tsd_offset;
    return &table;
}

#if MYIOSDECK_WITH_WINE
long NtTerminateProcess(void *handle, long status);   // Wine's unix side
#endif

static void __attribute__((noreturn)) fatal(const char *what) {
    mid_log("[fxi-win] STOP: %s", what);
#if MYIOSDECK_WITH_WINE
    NtTerminateProcess((void *)(intptr_t)-1, (long)0xE0F0F001);   // ends the Windows program, not the app
#endif
    abort();
}

// Called by fxi_win_glue.S on the emulator stack.
uint64_t fxi_win_glue_run(FxiCpu *c) {
    static int announced;
    if (!announced) {
        announced = 1;
        mid_log("[fxi-win] first x64 code at %#llx", (unsigned long long)fxi_win_rip(c));
    }
    uint64_t target = fxi_win_run(c);
    if (!target) {
        fxi_win_exc e;
        if (fxi_win_exception(c, &e)) raise_x64(c, e.rip, e.code, e.flags, e.nparams, e.info, "CPU exception");
        fatal(fxi_win_error(c));
    }
    return target;
}

void fxi_win_glue_load_context(FxiCpu *c, const void *ctx) {
    fxi_win_load_context(c, ctx);
    fxi_win_set_teb(c, (uint64_t)(uintptr_t)teb_now());
}

void fxi_win_glue_no_cpu(void) { fatal("x64 code entered on a thread without an FXI CPU (ThreadInit did not run)"); }
