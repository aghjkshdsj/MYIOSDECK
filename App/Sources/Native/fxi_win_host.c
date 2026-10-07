// SPDX-License-Identifier: GPL-3.0-or-later
// FXI as Wine's x64 CPU without JIT (docs/NO_JIT_WINDOWS.md step D): what the emulator DLL
// (engine/pedylib/emu, loaded by Wine as xtajit64.dll) calls into. Wine's no-JIT map hook
// (engine/wine/patches/nojit_dylib.py) stores mid_fxi_win_host_table() into that DLL when it
// maps it; the DLL's exports branch through the table to fxi_win_glue.S and to the C here.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
