// SPDX-License-Identifier: GPL-3.0-or-later
// FXI Windows mode (docs/NO_JIT_WINDOWS.md step D): FXI as the x86-64 CPU of Wine's ARM64EC
// processes, in the role FEX (xtajit64.dll) plays with JIT.
//
// Wine's ARM64EC world is native ARM64 code; only the program's own x64 code needs a CPU.
// Every thread gets an FxiCpu; all threads share one block cache (struct Fxi, windows = 1).
// A run starts at an x64 address and interprets until control reaches native ARM64EC code
// (a call into a Windows DLL, a return into an exit thunk): fxi_lookup turns such targets
// into cached one-uop exit blocks, and fxi_win_run returns the native address. The
// register hand-over on either side (ARM64EC register mapping, entry thunks, the emulator
// stack) is the app's transition glue, App/Sources/Native/fxi_win_glue.S.
//
// Portable C: nothing here touches the host's TEB register or Wine directly.

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "fxi_internal.h"

// The transition glue saves and restores these members at fixed offsets.
_Static_assert(offsetof(FxiCpu, r) == 0x00, "FxiCpu.r");
_Static_assert(offsetof(FxiCpu, rip) == 0x98, "FxiCpu.rip");
_Static_assert(offsetof(FxiCpu, xmm) == 0xa0, "FxiCpu.xmm");

static struct Fxi *g_win_vm;
static pthread_once_t g_win_once = PTHREAD_ONCE_INIT;

static void win_vm_init(void) {
    g_win_vm = fxi_vm_new();
    g_win_vm->windows = 1;
}

// PEB->EcCodeBitMap (TEB+0x60 -> PEB, PEB+0x368): one bit per 4 KB page of native EC code.
int fxi_win_is_ec(FxiCpu *c, uint64_t rip) {
    uint64_t teb = c->r[R_GS];
    if (!teb) return 0;
    uint64_t peb = ld64(teb + 0x60);
    if (!peb) return 0;
    uint64_t bitmap = ld64(peb + 0x368);
    if (!bitmap) return 0;
    return (int)((ld64(bitmap + ((rip >> 18) << 3)) >> ((rip >> 12) & 63)) & 1);
}

// FXR: the host's answer for a native target: a function FXR runs itself (fxr_pin.c, p_nat_*)
static int (*g_native_kind)(uint64_t target);                                 // FXR
void fxi_win_set_native(int (*kind)(uint64_t target)) { g_native_kind = kind; }   // FXR

Block *fxi_win_exit_block(uint64_t rip) {
    Block *b = calloc(1, sizeof(Block) + sizeof(Uop));
    b->rip = rip;
    b->n = 1;
    b->u[0].fn = fxi_named("ec_exit");
    b->u[0].imm = rip;
    b->u[0].rip = rip;  // FXR: a fault in a native function FXR runs itself is reported here
    fxr_init_slow(b);   // FXR: its handlers run uops through u->p (here: FXI's handler, the state spilled)
    if (g_native_kind) fxr_init_native(b, g_native_kind(rip));   // FXR: TlsGetValue, critical sections
    return b;
}

FxiCpu *fxi_win_cpu_new(void) {
    pthread_once(&g_win_once, win_vm_init);
    FxiCpu *c = NULL;
    if (posix_memalign((void **)&c, 64, sizeof *c)) return NULL;
    memset(c, 0, sizeof *c);
    c->vm = g_win_vm;
    c->err = calloc(1, 256);
    c->mxcsr = 0x1f80;
    fxi_x87_init(c);
    fxi_set_rflags(c, 0x202);
    return c;
}

// Entering x64 code: usually a return from a native call, whose x64 call pushed this rip on
// the return-address ring with its call site's return-block slot (build 89: Stick Fight's main
// thread made 2.1M native calls/s and paid a hash lookup for every return).
static Block *entry_block(FxiCpu *c, uint64_t rip) {
    uint32_t top = (c->ras_top - 1) & 31;
    if (c->ras[top].rip == rip && c->ras[top].slot) {
        Block **slot = c->ras[top].slot;
        c->ras_top--;
        Block *b = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (b) return b;
        b = fxi_lookup(c, rip);
        if (b != fxi_stop) __atomic_store_n(slot, b, __ATOMIC_RELEASE);
        return b;
    }
    // FXR: its calls do not push that ring, but each thread has an indirect-branch cache (rip ->
    // the block's first uop, fxr_pin.h IBTC): returns from native calls come back to the same rips
    __typeof__(c->fxr_ibtc[0]) *e = &c->fxr_ibtc[(rip ^ (rip >> 10)) & 1023u];                       // FXR
    if (c->fxr_ready && e->rip == rip && e->u) return (Block *)(void *)((char *)e->u - offsetof(Block, u));   // FXR
    c->n_entry_miss++;                                                                                // FXR
    Block *b = fxi_lookup(c, rip);                                                                    // FXR
    if (c->fxr_ready && b != fxi_stop) { e->rip = rip; e->u = b->u; }                                 // FXR
    return b;                                                                                         // FXR
}

uint64_t fxi_win_run(FxiCpu *c) {
    c->stop = 0;
    c->cur = NULL;
    c->fxr_phase = FXR_PH_ENTER;   // FXR: the profiler (fxi_win_sample)
    if (c->err) c->err[0] = 0;
    Block *b = entry_block(c, c->rip);
    if (!c->stop) fxr_enter(c, b);   // FXR (Windows mode: experimental; fault state, fxr_win_host_state)
    if (c->stop == FXI_STOP_EC) {
        c->stop = 0;
        return c->rip;
    }
    if (c->err && !c->err[0])
        snprintf(c->err, 256, "dispatch chain left with stop=%d at %#llx", c->stop, (unsigned long long)c->rip);
    return 0;
}

int fxi_win_exception(FxiCpu *c, fxi_win_exc *e) {
    if (c->stop != FXI_STOP_EXCEPTION) return 0;
    e->code = c->exc_code; e->flags = c->exc_flags; e->nparams = c->exc_nparams;
    e->info[0] = c->exc_info[0]; e->info[1] = c->exc_info[1];
    e->rip = c->rip;
    c->stop = 0;
    return 1;
}

// A host fault while interpreting: the instruction whose memory access faulted (its state
// is as before it ran), or the block being translated (an execute fault).
uint64_t fxi_win_fault_rip(FxiCpu *c, int *is_fetch) {
    if (is_fetch) *is_fetch = c->cur == NULL;
    return c->cur ? c->cur->rip : c->rip;
}

// Diagnostics: where DF was last set (1 std, 2 popf, 3 context load, 0 never) and its rip.
int fxi_win_df_source(FxiCpu *c, uint64_t *rip) { *rip = c->df_rip; return (int)c->df_how; }

// Profiler (sampled from another thread, racy by design): the last memory-touching instruction
// (or the block being looked up), block lookups, exits to native code, blocks translated.
uint64_t fxi_win_profile(FxiCpu *c, uint64_t *lookups, uint64_t *exits, uint64_t *blocks) {
    Uop *u = __atomic_load_n(&c->cur, __ATOMIC_RELAXED);
    *lookups = c->n_lookups; *exits = c->n_exits; *blocks = c->vm->blocks;
    return u ? u->rip : c->rip;
}

// FXR profiler counters: uops run by FXI's handlers (exits included), indirect-branch cache misses,
// entries from native code that missed it, native calls FXR ran itself.
void fxi_win_counters(FxiCpu *c, uint64_t out[4]) {                                    // FXR
    out[0] = c->n_slow; out[1] = c->n_ibtc_miss; out[2] = c->n_entry_miss; out[3] = c->n_native;   // FXR
}                                                                                      // FXR

// Profiler: calls into native code per target since the last call (counts are reset).
int fxi_win_exit_counts(FxiCpu *c, uint64_t *targets, uint64_t *counts, int max) {
    int n = 0;
    for (int i = 0; i < 256 && n < max; i++) {
        uint64_t k = __atomic_exchange_n(&c->exit_tab[i].n, 0, __ATOMIC_RELAXED);
        if (k) { targets[n] = c->exit_tab[i].target; counts[n] = k; n++; }
    }
    return n;
}

// Diagnostics: the last block lookups, oldest first.
int fxi_win_trail(FxiCpu *c, uint64_t *rips, int max) {
    int n = c->trail_n < 16 ? (int)c->trail_n : 16;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) rips[i] = c->trail[(c->trail_n - (uint32_t)n + (uint32_t)i) & 15];
    return n;
}

// The CPU state as an AMD64 CONTEXT (CONTEXT_FULL | CONTEXT_FLOATING_POINT), rip given.
void fxi_win_save_context(FxiCpu *c, void *ctx, uint64_t rip) {
    uint8_t *p = ctx;
    memset(p, 0, 0x4d0);
    uint32_t flags = 0x10000b, eflags = (uint32_t)fxi_rflags(c);   // CONTEXT_AMD64 | CONTROL | INTEGER | FLOATING_POINT
    memcpy(p + 0x30, &flags, 4);
    memcpy(p + 0x34, &c->mxcsr, 4);
    memcpy(p + 0x44, &eflags, 4);
    memcpy(p + 0x78, c->r, 16 * 8);
    memcpy(p + 0xf8, &rip, 8);
    fxi_x87_fxsave(c, p + 0x100, 1);
}

const char *fxi_win_error(FxiCpu *c) { return c->err ? c->err : "?"; }
void fxi_win_set_teb(FxiCpu *c, uint64_t teb) { c->r[R_GS] = teb; }
uint64_t fxi_win_rip(FxiCpu *c) { return c->rip; }

// An AMD64 CONTEXT (BeginSimulation, after NtContinue / at thread start).
void fxi_win_load_context(FxiCpu *c, const void *ctx) {
    const uint8_t *p = ctx;
    memcpy(c->r, p + 0x78, 16 * 8);              // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8-R15
    c->rip = ld64((uint64_t)(uintptr_t)(p + 0xf8));
    fxi_set_rflags(c, ld32((uint64_t)(uintptr_t)(p + 0x44)));
    if (c->df) { c->df_rip = c->rip; c->df_how = 3; }
    fxi_x87_fxrstor(c, p + 0x100, 1);            // FltSave (fxsave layout): x87 and XMM0-15
    c->mxcsr = (uint32_t)ld32((uint64_t)(uintptr_t)(p + 0x34));
}
