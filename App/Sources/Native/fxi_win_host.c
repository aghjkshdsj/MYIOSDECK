// SPDX-License-Identifier: GPL-3.0-or-later
// FXI as Wine's x64 CPU without JIT (docs/NO_JIT_WINDOWS.md step D): what the emulator DLL
// (engine/pedylib/emu, loaded by Wine as xtajit64.dll) calls into. Wine's no-JIT map hook
// (engine/wine/patches/nojit_dylib.py) stores mid_fxi_win_host_table() into that DLL when it
// maps it; the DLL's exports branch through the table to fxi_win_glue.S and to the C here.

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
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

// ---- The x64 CPU: FXI, or FXR (experimental) when MYIOSDECK_WIN_CPU=fxr ----
// Settings › Without JIT › "Windows games: FXR". The CPU is opaque here (the glue relies only on
// the layout both share: GPRs at 0x00, rip at 0x98, xmm at 0xa0), so one table serves both.
typedef struct {
    const char *name;
    void *(*cpu_new)(void);
    uint64_t (*run)(void *c);
    const char *(*error)(void *c);
    void (*set_teb)(void *c, uint64_t teb);
    uint64_t (*rip)(void *c);
    void (*load_context)(void *c, const void *ctx);
    void (*save_context)(void *c, void *ctx, uint64_t rip);
    int (*exception)(void *c, void *e);
    uint64_t (*fault_rip)(void *c, int *is_fetch);
    int (*trail)(void *c, uint64_t *rips, int max);
    int (*df_source)(void *c, uint64_t *rip);
    uint64_t (*profile)(void *c, uint64_t *lookups, uint64_t *exits, uint64_t *blocks);
    int (*exit_counts)(void *c, uint64_t *targets, uint64_t *counts, int max);
    // FXR: the x64 state from the host registers of the thread (x0-x30, sp, pc, q0-q7), at a fault
    // (fault = 1) or a stop; 1 = exact (the CPU loaded). NULL for FXI: its state is in memory.
    int (*host_state)(void *c, const uint64_t *x31, uint64_t sp, uint64_t *pc, const void *q, int fault, uint64_t *rip);
    // FXR (NULL for FXI): the native functions it runs itself (named by the host), and the
    // profiler's view of a stopped thread (WS_* below), its counters and FXI handler names.
    void (*set_native)(int (*kind)(uint64_t target));
    int (*sample)(void *c, uint64_t pc, uint64_t x21, int (*rd)(uint64_t, void *, size_t), uint64_t *rip, const void **fn);
    void (*counters)(void *c, uint64_t out[4]);
    const char *(*op_name)(const void *fn, char *buf, size_t n);
} win_cpu;
enum { WS_UNKNOWN, WS_PINNED, WS_SLOW, WS_LOOKUP, WS_ENTER, WS_COUNT };   // engine/fxr/fxi.h FXR_WS_*

static void *fxi_cpu_new(void) { return fxi_win_cpu_new(); }
static uint64_t fxi_run(void *c) { return fxi_win_run(c); }
static const char *fxi_error(void *c) { return fxi_win_error(c); }
static void fxi_set_teb(void *c, uint64_t teb) { fxi_win_set_teb(c, teb); }
static uint64_t fxi_rip(void *c) { return fxi_win_rip(c); }
static void fxi_load_context(void *c, const void *ctx) { fxi_win_load_context(c, ctx); }
static void fxi_save_context(void *c, void *ctx, uint64_t rip) { fxi_win_save_context(c, ctx, rip); }
static int fxi_exception(void *c, void *e) { return fxi_win_exception(c, e); }
static uint64_t fxi_fault_rip(void *c, int *is_fetch) { return fxi_win_fault_rip(c, is_fetch); }
static int fxi_trail(void *c, uint64_t *rips, int max) { return fxi_win_trail(c, rips, max); }
static int fxi_df_source(void *c, uint64_t *rip) { return fxi_win_df_source(c, rip); }
static uint64_t fxi_profile(void *c, uint64_t *l, uint64_t *e, uint64_t *b) { return fxi_win_profile(c, l, e, b); }
static int fxi_exit_counts(void *c, uint64_t *t, uint64_t *n, int max) { return fxi_win_exit_counts(c, t, n, max); }
static const win_cpu k_fxi = { "FXI", fxi_cpu_new, fxi_run, fxi_error, fxi_set_teb, fxi_rip, fxi_load_context,
                               fxi_save_context, fxi_exception, fxi_fault_rip, fxi_trail, fxi_df_source, fxi_profile,
                               fxi_exit_counts, NULL, NULL, NULL, NULL, NULL };

// fxr_win_shim.c
void *mid_fxr_win_cpu_new(void);
uint64_t mid_fxr_win_run(void *c);
const char *mid_fxr_win_error(void *c);
void mid_fxr_win_set_teb(void *c, uint64_t teb);
uint64_t mid_fxr_win_rip(void *c);
void mid_fxr_win_load_context(void *c, const void *ctx);
void mid_fxr_win_save_context(void *c, void *ctx, uint64_t rip);
int mid_fxr_win_exception(void *c, void *e);
uint64_t mid_fxr_win_fault_rip(void *c, int *is_fetch);
int mid_fxr_win_trail(void *c, uint64_t *rips, int max);
int mid_fxr_win_df_source(void *c, uint64_t *rip);
uint64_t mid_fxr_win_profile(void *c, uint64_t *lookups, uint64_t *exits, uint64_t *blocks);
int mid_fxr_win_exit_counts(void *c, uint64_t *targets, uint64_t *counts, int max);
int mid_fxr_win_host_state(void *c, const uint64_t *x31, uint64_t sp, uint64_t *pc, const void *q, int fault, uint64_t *rip);
void mid_fxr_win_set_native(int (*kind)(uint64_t target));
int mid_fxr_win_sample(void *c, uint64_t pc, uint64_t x21, int (*rd)(uint64_t, void *, size_t), uint64_t *rip, const void **fn);
void mid_fxr_win_counters(void *c, uint64_t out[4]);
const char *mid_fxr_win_op_name(const void *fn, char *buf, size_t n);
static const win_cpu k_fxr = { "FXR", mid_fxr_win_cpu_new, mid_fxr_win_run, mid_fxr_win_error, mid_fxr_win_set_teb,
                               mid_fxr_win_rip, mid_fxr_win_load_context, mid_fxr_win_save_context,
                               mid_fxr_win_exception, mid_fxr_win_fault_rip, mid_fxr_win_trail, mid_fxr_win_df_source,
                               mid_fxr_win_profile, mid_fxr_win_exit_counts, mid_fxr_win_host_state,
                               mid_fxr_win_set_native, mid_fxr_win_sample, mid_fxr_win_counters, mid_fxr_win_op_name };
static const win_cpu *g_cpu = &k_fxi;

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
    int df_how = g_cpu->df_source(c, &df_rip);
    static const char *df_names[4] = { "never set", "std", "popf", "a context load" };
    mid_log("[fxi-win]   DF %u, last set by %s at %#llx", (ef >> 10) & 1, df_names[df_how & 3], (unsigned long long)df_rip);
    if (df_how) log_code("df code", df_rip > 48 ? df_rip - 48 : df_rip, 64);
    uint64_t from = rip - 160 < (rip & ~0x3fffull) ? (rip & ~0x3fffull) : rip - 160;
    log_code("code before", from, (unsigned)(rip - from));
    log_code("code at", rip, 32);
    uint64_t t[16];
    int n = g_cpu->trail(c, t, 16);
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
    g_cpu->save_context(c, ctx64, rip);
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
    if (g_cpu->host_state) {
        // FXR: the x64 registers are in host registers at the fault. Wine's ARM64 CONTEXT: X0-X28
        // at 0x08, Fp 0xf0, Lr 0xf8, Sp 0x100, Pc 0x108, V0-V31 at 0x110.
        uint64_t x[31], pc = rd64(arm_ctx + 0x108), r;
        for (int i = 0; i < 29; i++) x[i] = rd64(arm_ctx + 0x08 + 8 * i);
        x[29] = rd64(arm_ctx + 0xf0);
        x[30] = rd64(arm_ctx + 0xf8);
        if (!g_cpu->host_state(c, x, rd64(arm_ctx + 0x100), &pc, arm_ctx + 0x110, 1, &r))
            fatal("FXR: no exact x64 state at this host fault");
    }
    uint64_t rip = g_cpu->fault_rip(c, &fetch);
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

// ---- Performance or efficiency cores (build 120) ----
// iOS decides which core a thread runs on; its QoS class is the hint: user-interactive threads
// stay on the performance cores, utility ones go to the efficiency cores, which do the same work
// for a fraction of the energy and heat (build 118: Stick Fight's P-cores throttled from 3.7 to
// 2.0 GHz within three minutes, its frame rate with them). Every x64 thread starts
// user-interactive (build 88: an interpreted thread on an efficiency core runs at about half
// speed). Once a second the profiler thread looks at how much of it each x64 thread ran (in x64
// or native code): the busiest one (the game's main loop) and any that ran QOS_HIGH_PCT or more
// stay on, or come back to, the performance cores at once; one that ran under QOS_LOW_PCT for
// QOS_LOW_SECS seconds in a row moves to the efficiency cores. The thread switches its own class
// the next time it enters x64 code (pthread_set_qos_class_self_np acts on the calling thread
// only); it finds its slot in its CPU area's EmulatorData[1] (+0x38; [0] holds the CPU).
// Threads that never run x64 code (Wine's, DXMT's, the audio engine's) keep their class.
// MYIOSDECK_ECORES=0 keeps every x64 thread on the performance cores.
#define QOS_HIGH_PCT 35
#define QOS_LOW_PCT 15
#define QOS_LOW_SECS 3
static struct { uint32_t run; uint8_t low_secs, want_e, have_e, alive; } g_qos[FAULT_THREADS];
static int g_qos_on = 1;

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
    g_qos[slot].run = 0; g_qos[slot].low_secs = 0; g_qos[slot].want_e = 0; g_qos[slot].have_e = 0;
    *(uint64_t *)(area + 0x38) = (uint64_t)slot + 1;   // EmulatorData[1]: this thread's slot (qos_apply)
    __atomic_store_n(&g_fault_threads[slot].thread, self, __ATOMIC_RELEASE);
}

// The calling thread, entering x64 code: the class the profiler chose for it.
static void qos_apply(void) {
    uint8_t *area = cpu_area_now();
    uint64_t s = area ? *(uint64_t *)(area + 0x38) : 0;
    if (!s || s > FAULT_THREADS) return;
    int i = (int)s - 1;
    uint8_t w = __atomic_load_n(&g_qos[i].want_e, __ATOMIC_RELAXED);
    if (__builtin_expect(w == g_qos[i].have_e, 1)) return;
    g_qos[i].have_e = w;
    pthread_set_qos_class_self_np(w ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, 0);
}

// Once a second, on the profiler thread (run: the samples that found the thread running).
static void qos_update(uint32_t samples) {
    int busiest = -1;
    uint32_t most = 0;
    for (int i = 0; i < FAULT_THREADS; i++)
        if (g_fault_threads[i].thread && g_qos[i].run > most) { most = g_qos[i].run; busiest = i; }
    static int logged;
    for (int i = 0; i < FAULT_THREADS; i++) {
        if (!g_fault_threads[i].thread || !g_qos[i].alive) continue;   // gone (its slot stays)
        g_qos[i].alive = 0;
        unsigned pct = samples ? (unsigned)(100ull * g_qos[i].run / samples) : 100;
        uint8_t want = g_qos[i].want_e;
        if (!g_qos_on || i == busiest || pct >= QOS_HIGH_PCT) { want = 0; g_qos[i].low_secs = 0; }
        else if (pct < QOS_LOW_PCT) { if (g_qos[i].low_secs < 255) g_qos[i].low_secs++; if (g_qos[i].low_secs >= QOS_LOW_SECS) want = 1; }
        else g_qos[i].low_secs = 0;
        if (want != g_qos[i].want_e) {
            __atomic_store_n(&g_qos[i].want_e, want, __ATOMIC_RELAXED);
            if (logged < 64) {
                logged++;
                mid_log("[fxi-win] thread TEB %p to the %s cores (it ran %u%% of the last second)", (void *)g_fault_threads[i].teb,
                        want ? "efficiency" : "performance", pct);
            }
        }
        g_qos[i].run = 0;
    }
}

// ---- Sampling profiler (build 87) ----
// Where does a no-JIT game spend its time? A host thread samples every running FXI thread
// 500 times a second: in x64 code (InSimulation) it records the rip of the instruction that
// last touched memory (FXI keeps it for exact faults), otherwise it counts native (Wine, the
// D3D/Metal DLLs). Every 10 s it logs [fxi-prof] lines: x64 vs native time per busy thread,
// block lookups and calls into native code per second, x64 time per module (from the PEB's
// loader list; Mono's generated code is outside every module) and the hottest instructions.
// Sampling is racy by design and costs the game nothing (one Mach call per running thread).
// FXR (build 119): FXR keeps no "last instruction" while its pinned handlers run, so a sample
// reads the thread's pc and x21 (thread_get_state stops it for a moment): a pinned handler's pc
// means guest code at full speed, x21 its uop and so the exact x64 instruction; elsewhere FXR's
// phase says FXI's handler for an instruction (slow path, named), a block lookup, or the way in
// from native code. The report splits each thread's x64 time that way and names the FXI
// handlers that take the most, the instructions most worth a pinned form.
#define PROF_HZ 500
#define PROF_PERIOD_S 10
#define PROF_SLOTS 16384
typedef struct { uint64_t rip, n; } prof_hit;
static prof_hit g_prof_hits[PROF_SLOTS];   // rip | where << 56 (FXR: WS_*)
static prof_hit g_prof_fns[512];           // FXR: FXI handler -> samples in it
static struct { uint64_t x64, native, last_lookups, last_exits, ws[WS_COUNT], last_cnt[4]; } g_prof_thr[FAULT_THREADS];
static int prof_rd(uint64_t a, void *out, size_t n);

static int vmr(uint64_t a, void *out, size_t n) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}
static int prof_rd(uint64_t a, void *out, size_t n) { return vmr(a, out, n); }

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

static uint64_t ffs_target(uint64_t a);
static int read_name(uint64_t a, char *s, size_t n);
// The export of a module whose x64 fast-forward thunk leads to addr (Wine's ARM64EC DLLs: the
// export is the thunk, addr its native code), else the export closest below addr ("name+0x10"),
// from its PE export directory. Names found are kept (the scan reads every export).
static void prof_export_name(const prof_mod *m, uint64_t addr, char *out, size_t outsz) {
    static struct { uint64_t addr; char name[96]; } cache[64];
    static unsigned cache_n;
    for (unsigned i = 0; i < cache_n && i < 64; i++)
        if (cache[i].addr == addr) { snprintf(out, outsz, "%s", cache[i].name); return; }
    snprintf(out, outsz, "?");
    uint32_t lfanew, dir_rva, nnames, funcs_rva, names_rva, ords_rva;
    uint64_t b = m->base;
    // On failure the name says which step failed (?hdr, ?dir, ?tab, ?ord, ?below, ?str).
    if (!vmr(b + 0x3c, &lfanew, 4) || !vmr(b + lfanew + 24 + 112, &dir_rva, 4)) { snprintf(out, outsz, "?hdr"); return; }
    if (!dir_rva) { snprintf(out, outsz, "?dir"); return; }
    if (!vmr(b + dir_rva + 0x18, &nnames, 4) || !vmr(b + dir_rva + 0x1c, &funcs_rva, 4) ||
        !vmr(b + dir_rva + 0x20, &names_rva, 4) || !vmr(b + dir_rva + 0x24, &ords_rva, 4) || nnames > 20000) {
        snprintf(out, outsz, "?tab"); return;
    }
    uint32_t rva = (uint32_t)(addr - b), best_rva = 0, best_name = 0, thunk_name = 0;
    for (uint32_t i = 0; i < nnames && !thunk_name; i++) {
        uint16_t ord; uint32_t f, nm;
        if (!vmr(b + ords_rva + 2ull * i, &ord, 2) || !vmr(b + funcs_rva + 4ull * ord, &f, 4)) { snprintf(out, outsz, "?ord"); return; }
        if (f != rva && ffs_target(b + f) == addr && vmr(b + names_rva + 4ull * i, &nm, 4)) thunk_name = nm;
        if (f <= rva && f >= best_rva && vmr(b + names_rva + 4ull * i, &nm, 4)) { best_rva = f; best_name = nm; }
    }
    if (thunk_name) { best_name = thunk_name; best_rva = rva; }
    if (!best_name) { snprintf(out, outsz, "?below(%u names)", nnames); return; }
    char name[64] = { 0 };
    if (!read_name(b + best_name, name, sizeof name)) { snprintf(out, outsz, "?str"); return; }
    if (rva == best_rva) snprintf(out, outsz, "%s", name);
    else snprintf(out, outsz, "%s+%#x", name, rva - best_rva);
    cache[cache_n % 64].addr = addr;
    snprintf(cache[cache_n % 64].name, sizeof cache[0].name, "%s", out);
    cache_n++;
}

// ---- Native functions FXR runs itself (build 119) ----
// Mono's hottest calls into Wine are TlsGetValue and Enter/LeaveCriticalSection (Stick Fight,
// build 118: 2.8 M a second on the main thread, each a full transition to native code and back).
// FXR runs Wine's code paths for them itself (engine/fxr/fxr_pin.c, p_nat_*) once this names the
// native target of an exit block: the ARM64EC code an export's x64 fast-forward thunk jumps to,
// found through the export table of the PE image the target lies in (Wine's DLLs are mapped from
// the signed dylibs, where dladdr gives the image's start: symbol myiosdeck_pe_image).
// MYIOSDECK_FXR_NATIVE=0 turns it off.
static const struct { const char *name; int kind; } k_native[] = {
    { "TlsGetValue", 1 }, { "RtlEnterCriticalSection", 2 }, { "RtlLeaveCriticalSection", 3 },
};
#define N_NATIVE (sizeof k_native / sizeof k_native[0])
static uint64_t g_native_addr[N_NATIVE];
static uint64_t g_native_pe[128];   // images already searched (FXR calls in under its translation lock)
static int g_native_npe;

// An export's code: where its x64 fast-forward thunk (mov rax, rsp; mov [rax+0x20], rbx; push rbp;
// pop rbp; jmp rel32) leads, or the export itself.
static uint64_t ffs_target(uint64_t a) {
    static const uint8_t k_ffs[10] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9 };
    uint8_t k[14];
    if (!vmr(a, k, sizeof k) || memcmp(k, k_ffs, sizeof k_ffs)) return a;
    int32_t rel;
    memcpy(&rel, k + 10, 4);
    return a + 14 + (uint64_t)(int64_t)rel;
}
static int read_name(uint64_t a, char *s, size_t n) {   // a NUL-terminated name, cut at n - 1
    for (size_t len = n - 1; len >= 8; len /= 2)
        if (vmr(a, s, len)) { s[len] = 0; return 1; }
    return 0;
}
// The code export `want` of the PE32+ image at base leads to (binary search: Wine sorts the names);
// 0 when it has no such export or forwards it.
static uint64_t pe_export_code(uint64_t base, const char *want) {
    uint16_t mz, magic;
    uint32_t lfanew, sig, dir_rva, dir_size, nnames, funcs_rva, names_rva, ords_rva;
    if (!vmr(base, &mz, 2) || mz != 0x5a4d || !vmr(base + 0x3c, &lfanew, 4) || lfanew > 0x1000 ||
        !vmr(base + lfanew, &sig, 4) || sig != 0x4550 || !vmr(base + lfanew + 24, &magic, 2) || magic != 0x20b ||
        !vmr(base + lfanew + 24 + 112, &dir_rva, 4) || !vmr(base + lfanew + 24 + 116, &dir_size, 4) || !dir_rva)
        return 0;
    uint64_t d = base + dir_rva;
    if (!vmr(d + 0x18, &nnames, 4) || !vmr(d + 0x1c, &funcs_rva, 4) || !vmr(d + 0x20, &names_rva, 4) ||
        !vmr(d + 0x24, &ords_rva, 4) || nnames > 65536)
        return 0;
    uint32_t lo = 0, hi = nnames;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, nm, f;
        uint16_t ord;
        char s[48];
        if (!vmr(base + names_rva + 4ull * mid, &nm, 4) || !read_name(base + nm, s, sizeof s)) return 0;
        int cmp = strcmp(s, want);
        if (cmp < 0) { lo = mid + 1; continue; }
        if (cmp > 0) { hi = mid; continue; }
        if (!vmr(base + ords_rva + 2ull * mid, &ord, 2) || !vmr(base + funcs_rva + 4ull * ord, &f, 4)) return 0;
        if (f >= dir_rva && f < dir_rva + dir_size) return 0;   // a forwarder
        return ffs_target(base + f);
    }
    return 0;
}
static int host_native_kind(uint64_t target) {
    Dl_info di;
    if (dladdr((const void *)(uintptr_t)target, &di) && di.dli_sname && !strcmp(di.dli_sname, "myiosdeck_pe_image")) {
        uint64_t base = (uint64_t)(uintptr_t)di.dli_saddr;
        int seen = 0;
        for (int i = 0; i < g_native_npe && !seen; i++) seen = g_native_pe[i] == base;
        if (!seen && g_native_npe < 128) {
            g_native_pe[g_native_npe++] = base;
            for (size_t k = 0; k < N_NATIVE; k++) {
                if (g_native_addr[k]) continue;
                uint64_t a = pe_export_code(base, k_native[k].name);
                if (!a) continue;
                g_native_addr[k] = a;
                mid_log("[fxi-win] FXR runs %s itself (its ARM64EC code at %#llx is not entered)", k_native[k].name,
                        (unsigned long long)a);
            }
        }
    }
    for (size_t k = 0; k < N_NATIVE; k++)
        if (g_native_addr[k] == target) return k_native[k].kind;
    return 0;
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
        if (c) g_cpu->profile(c, &lk, &ex, &blocks);
        mid_log("[fxi-prof]   thread TEB %p: running %.0f%% of the time, x64 %.0f%% / native %.0f%%, "
                "%llu lookups/s, %llu native calls/s, %s cores", (void *)g_fault_threads[best].teb,
                100.0 * bestn / (double)period_samples, 100.0 * g_prof_thr[best].x64 / (double)bestn,
                100.0 * g_prof_thr[best].native / (double)bestn,
                (unsigned long long)((lk - g_prof_thr[best].last_lookups) / PROF_PERIOD_S),
                (unsigned long long)((ex - g_prof_thr[best].last_exits) / PROF_PERIOD_S),
                g_qos[best].have_e ? "efficiency" : "performance");
        if (c && g_cpu->counters) {   // FXR: its x64 time by where it goes, and its counters per second
            uint64_t k[4], *w = g_prof_thr[best].ws, *l = g_prof_thr[best].last_cnt;
            g_cpu->counters(c, k);
            double x = g_prof_thr[best].x64 ? (double)g_prof_thr[best].x64 : 1.0;
            mid_log("[fxi-prof]     FXR x64 time: pinned handlers %.0f%%, FXI handlers (slow path) %.0f%%, block lookups "
                    "%.0f%%, entering from native %.0f%%, unknown %.0f%% | per second: %llu uops by FXI handlers, %llu "
                    "indirect-branch cache misses, %llu entries that missed it, %llu native calls run by FXR",
                    100.0 * w[WS_PINNED] / x, 100.0 * w[WS_SLOW] / x, 100.0 * w[WS_LOOKUP] / x, 100.0 * w[WS_ENTER] / x,
                    100.0 * w[WS_UNKNOWN] / x, (unsigned long long)((k[0] - l[0]) / PROF_PERIOD_S),
                    (unsigned long long)((k[1] - l[1]) / PROF_PERIOD_S), (unsigned long long)((k[2] - l[2]) / PROF_PERIOD_S),
                    (unsigned long long)((k[3] - l[3]) / PROF_PERIOD_S));
        }
        g_prof_thr[best].x64 = g_prof_thr[best].native = 0;   // shown: do not pick it again
    }
    for (int i = 0; i < FAULT_THREADS; i++) {                 // counters for the next period
        if (!g_fault_threads[i].thread) continue;
        FxiCpu *c = g_fault_threads[i].cpu;
        if (c) g_cpu->profile(c, &g_prof_thr[i].last_lookups, &g_prof_thr[i].last_exits, &blocks);
        if (c && g_cpu->counters) g_cpu->counters(c, g_prof_thr[i].last_cnt);
        g_prof_thr[i].x64 = g_prof_thr[i].native = 0;
        memset(g_prof_thr[i].ws, 0, sizeof g_prof_thr[i].ws);
    }
    // x64 time per module, then the hottest instructions.
    int nm = teb ? prof_modules(teb, mods, 256) : 0;
    int nh = 0; uint64_t other = 0;
    const uint64_t rip_mask = (1ull << 56) - 1;   // FXR: the top byte says where (WS_*)
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof_hits[i].n) continue;
        sorted[nh++] = g_prof_hits[i];
        int found = 0;
        for (int k = 0; k < nm; k++)
            if ((g_prof_hits[i].rip & rip_mask) - mods[k].base < mods[k].size) { mods[k].x64 += g_prof_hits[i].n; found = 1; break; }
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
    static const char *const ws_tag[WS_COUNT] = { "", "", " [FXI handler]", " [block lookup]", " [entering]" };
    for (int i = 0; i < nh && i < (g_cpu->sample ? 24 : 16); i++) {
        uint64_t rip = sorted[i].rip & rip_mask;
        unsigned ws = (unsigned)(sorted[i].rip >> 56) % WS_COUNT;
        const char *mod = "(generated)"; uint64_t off = rip;
        for (int k = 0; k < nm; k++)
            if (rip - mods[k].base < mods[k].size) { mod = mods[k].name; off = rip - mods[k].base; break; }
        uint8_t code[16]; char hex[49] = "?";
        if (vmr(rip, code, 16)) for (int b = 0; b < 16; b++) snprintf(hex + 3 * b, 4, "%02x ", code[b]);
        mid_log("[fxi-prof]   hot %2d: %5.2f%% %s+%#llx (%#llx)%s: %s", i + 1, 100.0 * sorted[i].n / (double)x64, mod,
                (unsigned long long)off, (unsigned long long)rip, ws_tag[ws], hex);
    }
    if (g_cpu->op_name) {   // FXR: the FXI handlers (slow path) that took the most x64 time
        static prof_hit fns[512];
        memcpy(fns, g_prof_fns, sizeof fns);
        memset(g_prof_fns, 0, sizeof g_prof_fns);
        qsort(fns, 512, sizeof fns[0], prof_cmp_hit);
        char line2[640]; int o2 = 0;
        for (int i = 0; i < 16 && fns[i].n && o2 < 560; i++) {
            char nmb[48];
            o2 += snprintf(line2 + o2, sizeof line2 - (size_t)o2, "%s%s %.1f%%", i ? ", " : "",
                           g_cpu->op_name((const void *)(uintptr_t)fns[i].rip, nmb, sizeof nmb), 100.0 * fns[i].n / (double)x64);
        }
        if (o2) mid_log("[fxi-prof]   x64 time in FXI handlers, by instruction kind: %s", line2);
    }
    mid_log("[fxi-prof]   blocks translated so far: %llu", (unsigned long long)blocks);
    // Calls from x64 into native code per target, all threads: each costs a full transition.
    static prof_hit calls[1024];
    memset(calls, 0, sizeof calls);
    uint64_t total_calls = 0;
    for (int i = 0; i < FAULT_THREADS; i++) {
        if (!g_fault_threads[i].thread || !g_fault_threads[i].cpu) continue;
        uint64_t tg[256], ct[256];
        int k = g_cpu->exit_counts(g_fault_threads[i].cpu, tg, ct, 256);
        for (int j = 0; j < k; j++) {
            total_calls += ct[j];
            uint64_t h = (tg[j] * 0x9E3779B97F4A7C15ull) >> 54;
            for (int probe = 0; probe < 64; probe++, h = (h + 1) & 1023) {
                if (calls[h].rip == tg[j]) { calls[h].n += ct[j]; break; }
                if (!calls[h].n) { calls[h].rip = tg[j]; calls[h].n = ct[j]; break; }
            }
        }
    }
    if (!total_calls) return;
    qsort(calls, 1024, sizeof calls[0], prof_cmp_hit);
    mid_log("[fxi-prof]   calls into native code: %llu/s; top targets:", (unsigned long long)(total_calls / PROF_PERIOD_S));
    for (int i = 0; i < 12 && calls[i].n; i++) {
        const char *mod = "?"; char fn[96] = "?";
        for (int k = 0; k < nm; k++)
            if (calls[i].rip - mods[k].base < mods[k].size) { mod = mods[k].name; prof_export_name(&mods[k], calls[i].rip, fn, sizeof fn); break; }
        // Wine's ARM64EC DLLs are mapped from signed dylibs: when no export is at or below the
        // target (log 54: "?below" for every ntdll/kernel32 target), the dylib's own symbols may be.
        Dl_info di;
        if (fn[0] == '?' && dladdr((const void *)(uintptr_t)calls[i].rip, &di) && di.dli_sname)
            snprintf(fn, sizeof fn, "%s+%#llx [dylib]", di.dli_sname, (unsigned long long)(calls[i].rip - (uintptr_t)di.dli_saddr));
        mid_log("[fxi-prof]   native %2d: %5.1f%% %llu/s %s!%s (%#llx)", i + 1, 100.0 * calls[i].n / (double)total_calls,
                (unsigned long long)(calls[i].n / PROF_PERIOD_S), mod, fn, (unsigned long long)calls[i].rip);
    }
}

static void *prof_thread(void *arg) {
    (void)arg;
    // A diagnostic that wakes 500 times a second: efficiency cores (it was created by a game
    // thread and inherited user-interactive).
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    uint64_t ticks = 0;
    for (;;) {
        struct timespec ts = { 0, 1000000000 / PROF_HZ };
        nanosleep(&ts, NULL);
        for (int i = 0; i < FAULT_THREADS; i++) {
            mach_port_t t = __atomic_load_n(&g_fault_threads[i].thread, __ATOMIC_ACQUIRE);
            if (!t) continue;
            thread_basic_info_data_t bi;
            mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS) continue;
            g_qos[i].alive = 1;
            if (bi.run_state != TH_STATE_RUNNING) continue;
            g_qos[i].run++;   // running, x64 or native: how busy it is (qos_update)
            // The thread may have exited and Wine freed its CPU area (build 87 crashed here
            // reading it directly): read InSimulation through vm_read_overwrite.
            FxiCpu *c = g_fault_threads[i].cpu;
            uint8_t insim;
            if (!c || !vmr((uint64_t)(uintptr_t)g_fault_threads[i].area, &insim, 1)) continue;
            if (!insim) { g_prof_thr[i].native++; continue; }
            g_prof_thr[i].x64++;
            uint64_t lk, ex, bl, rip;
            int ws = WS_UNKNOWN;
            if (g_cpu->sample) {   // FXR: where the thread is (see above)
                arm_thread_state64_t st;
                mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
                const void *fn = NULL;
                rip = 0;
                if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS)
                    ws = g_cpu->sample(c, (uint64_t)arm_thread_state64_get_pc(st), st.__x[21], prof_rd, &rip, &fn);
                if (ws < 0 || ws >= WS_COUNT) ws = WS_UNKNOWN;
                g_prof_thr[i].ws[ws]++;
                if (ws == WS_SLOW && fn) {
                    uint64_t f = (uint64_t)(uintptr_t)fn, h = (f * 0x9E3779B97F4A7C15ull) >> 55;
                    for (int probe = 0; probe < 32; probe++, h = (h + 1) & 511) {
                        if (g_prof_fns[h].rip == f) { g_prof_fns[h].n++; break; }
                        if (!g_prof_fns[h].n) { g_prof_fns[h].rip = f; g_prof_fns[h].n = 1; break; }
                    }
                }
                if (ws == WS_UNKNOWN) continue;
                rip = (rip & ((1ull << 56) - 1)) | (uint64_t)ws << 56;
            } else {
                rip = g_cpu->profile(c, &lk, &ex, &bl);
            }
            uint64_t h = (rip * 0x9E3779B97F4A7C15ull) >> 50;
            for (int probe = 0; probe < 32; probe++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof_hits[h].rip == rip) { g_prof_hits[h].n++; break; }
                if (!g_prof_hits[h].n) { g_prof_hits[h].rip = rip; g_prof_hits[h].n = 1; break; }
            }
        }
        ++ticks;
        if (ticks % PROF_HZ == 0) qos_update(PROF_HZ);
        if (ticks % (PROF_HZ * PROF_PERIOD_S) == 0) prof_report(PROF_HZ * PROF_PERIOD_S);
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
    uint64_t rip = g_cpu->fault_rip(c, &fetch);
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
    if (g_cpu->host_state) {
        // FXR: the x64 registers are in the faulting thread's host registers (and XMM0-7 in
        // q0-q7): load them into its CPU before the state below is replaced
        FxiCpu *c = NULL;
        for (int i = 0; i < FAULT_THREADS && !c; i++)
            if (__atomic_load_n(&g_fault_threads[i].thread, __ATOMIC_ACQUIRE) == thread) c = g_fault_threads[i].cpu;
        if (!c) return 0;
        uint64_t x[31], pc = arm_thread_state64_get_pc(*state), rip;
        for (int i = 0; i < 29; i++) x[i] = state->__x[i];
        x[29] = arm_thread_state64_get_fp(*state);
        x[30] = arm_thread_state64_get_lr(*state);
        uint8_t q[8][16] = { { 0 } };
        arm_neon_state64_t ns;
        mach_msg_type_number_t nn = ARM_NEON_STATE64_COUNT;
        if (thread_get_state(thread, ARM_NEON_STATE64, (thread_state_t)&ns, &nn) == KERN_SUCCESS) memcpy(q, &ns.__v[0], sizeof q);
        if (!g_cpu->host_state(c, x, arm_thread_state64_get_sp(*state), &pc, q, 1, &rip)) return 0;
    }
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
        *slot = g_cpu->cpu_new();
        g_cpu->set_teb(*slot, (uint64_t)(uintptr_t)teb);
    }
    fault_thread_register(area, teb, *slot);
    // Game threads start at user-interactive QoS: the scheduler keeps them on the performance cores.
    // Build 88's log had the busiest thread (Stick Fight's audio) spending 113 of 249 ms on an
    // efficiency core, at about half the speed; one interpreted thread cannot use more than one
    // core, so a busy one should at least be a fast one. Quiet ones move to the efficiency cores
    // later (qos_update).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
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
    const char *cpu = getenv("MYIOSDECK_WIN_CPU");   // set by WineController from Settings
    g_cpu = cpu && !strcmp(cpu, "fxr") ? &k_fxr : &k_fxi;
    mid_log("[fxi-win] x64 CPU: %s", g_cpu->name);
    const char *nat = getenv("MYIOSDECK_FXR_NATIVE");
    if (g_cpu->set_native && !(nat && !strcmp(nat, "0"))) g_cpu->set_native(host_native_kind);
    const char *ecores = getenv("MYIOSDECK_ECORES");
    g_qos_on = !(ecores && !strcmp(ecores, "0"));
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
        mid_log("[fxi-win] first x64 code at %#llx", (unsigned long long)g_cpu->rip(c));
    }
    qos_apply();
    uint64_t target = g_cpu->run(c);
    if (!target) {
        fxi_win_exc e;
        if (g_cpu->exception(c, &e)) raise_x64(c, e.rip, e.code, e.flags, e.nparams, e.info, "CPU exception");
        fatal(g_cpu->error(c));
    }
    return target;
}

void fxi_win_glue_load_context(FxiCpu *c, const void *ctx) {
    g_cpu->load_context(c, ctx);
    g_cpu->set_teb(c, (uint64_t)(uintptr_t)teb_now());
}

void fxi_win_glue_no_cpu(void) { fatal("x64 code entered on a thread without an FXI CPU (ThreadInit did not run)"); }
