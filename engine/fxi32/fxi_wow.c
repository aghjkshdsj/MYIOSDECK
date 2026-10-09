// SPDX-License-Identifier: GPL-3.0-or-later
// FXI32 WoW64 mode (docs/NO_JIT_WOW64.md): FXI32 as the x86 CPU of a 32-bit Wine process, in
// the role FEX's xtajit.dll plays with JIT. The x86 code runs from the process's guest window;
// a run ends when control reaches the BOP page (the system-call entry g, the unix-call entry
// g + 2), at an exception or at an error, and the host (App/Sources/Native/fxi_wow_host.c)
// carries out the call through wow64.dll. API: engine/fxi32/fx32.h.
#define FXI_I386 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "../fxi/fxi_internal.h"
#include "fx32.h"

// A lookup of g or g + 2 returns a one-uop block that stops the run there (the bytes at g are
// never decoded: cd 2e cd 2e, FEX's convention, only marks the page).
Block *fxi_wow_bop_block(struct Fxi *vm, uint64_t rip) {
    if (!vm->bop || (rip != vm->bop && rip != vm->bop + 2)) return NULL;
    Block *b = calloc(1, sizeof(Block) + sizeof(Uop));
    b->rip = rip;
    b->n = 1;
    b->u[0].fn = fxi_named("bop_exit");
    b->u[0].imm = rip;
    b->u[0].rip = rip;
    return b;
}

// One instance (block cache) per 32-bit process, found by its window. Never freed: a process's
// threads may still be inside its blocks when another thread exits.
#define MAX_PROCESSES 16
static struct { uint64_t gbase; struct Fxi *vm; } g_procs[MAX_PROCESSES];
static pthread_mutex_t g_procs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct Fxi *process_vm(uint64_t gbase, uint32_t bop) {
    struct Fxi *vm = NULL;
    pthread_mutex_lock(&g_procs_lock);
    for (int i = 0; i < MAX_PROCESSES && !vm; i++)
        if (g_procs[i].vm && g_procs[i].gbase == gbase) vm = g_procs[i].vm;
    if (!vm) {
        // A window freed by an exited process can be taken again: the newest entry wins.
        int slot = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) if (!g_procs[i].vm) { slot = i; break; }
        vm = fxi_vm_new();
        vm->windows = 1;
        vm->gbase = gbase;
        vm->bop = bop;
        g_procs[slot].vm = vm;
        g_procs[slot].gbase = gbase;
    }
    pthread_mutex_unlock(&g_procs_lock);
    return vm;
}

Fx32Cpu *fx32_wow_cpu_new(uint64_t gbase, uint32_t bop) {
    FxiCpu *c = NULL;
    if (posix_memalign((void **)&c, 64, sizeof *c)) return NULL;
    memset(c, 0, sizeof *c);
    c->vm = process_vm(gbase, bop);
    c->gbase = gbase;
    c->err = calloc(1, 256);
    c->mxcsr = 0x1f80;
    fxi_x87_init(c);
    fxi_set_rflags(c, 0x202);
    return c;
}

int fx32_wow_run(Fx32Cpu *c) {
    c->stop = 0;
    c->cur = NULL;
    if (c->err) c->err[0] = 0;
    Block *b = fxi_lookup(c, c->rip);
    if (!c->stop) b->u[0].fn(c, b->u);   // returns when the chain stops
    switch (c->stop) {
    case FXI_STOP_BOP: c->stop = 0; return c->rip == c->vm->bop ? FX32_RUN_SYSCALL : FX32_RUN_UNIXCALL;
    case FXI_STOP_EXCEPTION: return FX32_RUN_EXCEPTION;
    default:
        if (c->err && !c->err[0])
            snprintf(c->err, 256, "dispatch chain left with stop=%d at %#llx", c->stop, (unsigned long long)c->rip);
        return FX32_RUN_ERROR;
    }
}

const char *fx32_wow_error(Fx32Cpu *c) { return c->err ? c->err : "?"; }

// I386_CONTEXT (Wine's winnt.h, pack 4): ContextFlags 0x00, FloatSave 0x1c (ControlWord,
// StatusWord, TagWord, ErrorOffset, ErrorSelector, DataOffset, DataSelector, RegisterArea[80],
// Cr0NpxState), SegGs 0x8c SegFs 0x90 SegEs 0x94 SegDs 0x98, Edi 0x9c Esi 0xa0 Ebx 0xa4 Edx 0xa8
// Ecx 0xac Eax 0xb0, Ebp 0xb4 Eip 0xb8 SegCs 0xbc EFlags 0xc0 Esp 0xc4 SegSs 0xc8,
// ExtendedRegisters[512] 0xcc (fxsave layout).
enum { CTX_CONTROL = 0x10001, CTX_INTEGER = 0x10002, CTX_SEGMENTS = 0x10004, CTX_FLOAT = 0x10008, CTX_EXTENDED = 0x10020 };
static const int k_gpr_off[8] = { 0xb0, 0xac, 0xa8, 0xa4, 0xc4, 0xb4, 0xa0, 0x9c };   // EAX ECX EDX EBX ESP EBP ESI EDI

static uint32_t ctx_rd32(const uint8_t *p, int off) { uint32_t v; memcpy(&v, p + off, 4); return v; }
static void ctx_wr32(uint8_t *p, int off, uint32_t v) { memcpy(p + off, &v, 4); }

void fx32_wow_load_context(Fx32Cpu *c, const void *ctx, uint32_t teb32) {
    const uint8_t *p = ctx;
    uint32_t flags = ctx_rd32(p, 0);
    if ((flags & CTX_INTEGER) == CTX_INTEGER)
        for (int i = 0; i < 8; i++) if (i != R_SP && i != R_BP) c->r[i] = ctx_rd32(p, k_gpr_off[i]);
    if ((flags & CTX_CONTROL) == CTX_CONTROL) {
        c->r[R_SP] = ctx_rd32(p, 0xc4);
        c->r[R_BP] = ctx_rd32(p, 0xb4);
        c->rip = ctx_rd32(p, 0xb8);
        fxi_set_rflags(c, ctx_rd32(p, 0xc0));
        if (c->df) { c->df_rip = c->rip; c->df_how = 3; }
    }
    if ((flags & CTX_EXTENDED) == CTX_EXTENDED) fxi_x87_fxrstor(c, p + 0xcc, 1);   // x87 + XMM0-7 (fxsave layout)
    c->r[R_FS] = teb32;   // fs: the TEB32 (guest address)
    c->r[R_GS] = 0;
}

void fx32_wow_save_context(Fx32Cpu *c, void *ctx) {
    uint8_t *p = ctx;
    memset(p, 0, 0x2cc);
    ctx_wr32(p, 0, CTX_CONTROL | CTX_INTEGER | CTX_SEGMENTS | CTX_FLOAT | CTX_EXTENDED);
    for (int i = 0; i < 8; i++) ctx_wr32(p, k_gpr_off[i], (uint32_t)c->r[i]);
    ctx_wr32(p, 0xb8, (uint32_t)c->rip);
    ctx_wr32(p, 0xc0, (uint32_t)fxi_rflags(c));
    // WoW64's flat selectors (Wine's 32-bit context: cs 0x23, fs 0x53, the rest 0x2b)
    ctx_wr32(p, 0xbc, 0x23); ctx_wr32(p, 0x90, 0x53);
    ctx_wr32(p, 0x8c, 0x2b); ctx_wr32(p, 0x94, 0x2b); ctx_wr32(p, 0x98, 0x2b); ctx_wr32(p, 0xc8, 0x2b);
    uint8_t *fx = p + 0xcc;
    fxi_x87_fxsave(c, fx, 1);
    // FloatSave (fnsave layout) from the fxsave image: full tag word from the abridged one
    // (a valid register is reported as valid, 00), registers packed to 10 bytes.
    uint16_t cw, sw;
    memcpy(&cw, fx, 2); memcpy(&sw, fx + 2, 2);
    uint32_t tag = 0;
    for (int i = 0; i < 8; i++) if (!((fx[4] >> i) & 1)) tag |= 3u << (2 * i);
    ctx_wr32(p, 0x1c, 0xffff0000u | cw);
    ctx_wr32(p, 0x20, 0xffff0000u | sw);
    ctx_wr32(p, 0x24, 0xffff0000u | tag);
    for (int i = 0; i < 8; i++) memcpy(p + 0x1c + 28 + 10 * i, fx + 32 + 16 * i, 10);
}

uint32_t fx32_wow_reg(Fx32Cpu *c, int r) { return (uint32_t)c->r[r & 7]; }
void fx32_wow_set_reg(Fx32Cpu *c, int r, uint32_t v) { c->r[r & 7] = v; }
uint32_t fx32_wow_eip(Fx32Cpu *c) { return (uint32_t)c->rip; }
void fx32_wow_set_eip(Fx32Cpu *c, uint32_t eip) { c->rip = eip; }

void fx32_wow_exception(Fx32Cpu *c, fx32_wow_exc *e) {
    e->code = c->exc_code; e->flags = c->exc_flags; e->nparams = c->exc_nparams;
    e->info[0] = c->exc_info[0]; e->info[1] = c->exc_info[1];
    e->eip = (uint32_t)c->rip;
    c->stop = 0;
}

uint32_t fx32_wow_fault_eip(Fx32Cpu *c, int *is_fetch) {
    if (is_fetch) *is_fetch = c->cur == NULL;
    return (uint32_t)(c->cur ? c->cur->rip : c->rip);
}

// Not yet: FXI keeps decoded blocks for good (docs/NO_JIT_WOW64.md 2.7, stage 3 follow-up). The
// host counts and logs the requests so a program that reloads code is recognisable.
void fx32_wow_invalidate(uint64_t gbase, uint32_t start, uint32_t len) { (void)gbase; (void)start; (void)len; }
