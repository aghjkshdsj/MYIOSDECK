// SPDX-License-Identifier: GPL-3.0-or-later
// FXI as Wine's x64 CPU without JIT (docs/NO_JIT_WINDOWS.md step D): what the emulator DLL
// (engine/pedylib/emu, loaded by Wine as xtajit64.dll) calls into. Wine's no-JIT map hook
// (engine/wine/patches/nojit_dylib.py) stores mid_fxi_win_host_table() into that DLL when it
// maps it; the DLL's exports branch through the table to fxi_win_glue.S and to the C here.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    void *process_init;         // 0x20 long (void)
    void *thread_init;          // 0x28 long (void)
    void *feature_present;      // 0x30 long (unsigned feature)
} mid_fxi_win_host;

void fxi_win_exit_to_x64(void);
void fxi_win_dispatch_jump(void);
void fxi_win_ret_to_entry_thunk(void);
void fxi_win_begin_simulation(void);

static uint8_t *teb_now(void) {
    uint64_t tsd;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(tsd));
    return *(uint8_t **)((tsd & ~7ull) + (uint64_t)fxi_win_tsd_offset);
}

static long host_process_init(void) {
    mid_log("[fxi-win] FXI is the x64 CPU of this Windows process (no JIT)");
    return 0;
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
        (void *)host_feature_present,
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
    if (!target) fatal(fxi_win_error(c));
    return target;
}

void fxi_win_glue_load_context(FxiCpu *c, const void *ctx) {
    fxi_win_load_context(c, ctx);
    fxi_win_set_teb(c, (uint64_t)(uintptr_t)teb_now());
}

void fxi_win_glue_no_cpu(void) { fatal("x64 code entered on a thread without an FXI CPU (ThreadInit did not run)"); }
