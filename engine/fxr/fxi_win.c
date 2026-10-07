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

Block *fxi_win_exit_block(uint64_t rip) {
    Block *b = calloc(1, sizeof(Block) + sizeof(Uop));
    b->rip = rip;
    b->n = 1;
    b->u[0].fn = fxi_named("ec_exit");
    b->u[0].imm = rip;
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

uint64_t fxi_win_run(FxiCpu *c) {
    c->stop = 0;
    c->cur = NULL;
    if (c->err) c->err[0] = 0;
    Block *b = fxi_lookup(c, c->rip);
    if (!c->stop) fxr_enter(c, b);   // FXR (Windows mode is not supported by FXR yet)
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
    fxi_x87_fxrstor(c, p + 0x100, 1);            // FltSave (fxsave layout): x87 and XMM0-15
    c->mxcsr = (uint32_t)ld32((uint64_t)(uintptr_t)(p + 0x34));
}
