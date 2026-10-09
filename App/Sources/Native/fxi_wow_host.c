// SPDX-License-Identifier: GPL-3.0-or-later
// FXI32 as the x86 CPU of a 32-bit Windows process without JIT (docs/NO_JIT_WOW64.md, stage 3):
// what the WoW64 CPU module (engine/pedylib/emu/xtajit_wow.c, loaded by wow64.dll as xtajit.dll)
// calls into. Wine's no-JIT map hook (engine/wine/patches/nojit_dylib.py) stores
// mid_fxi_wow_host_table() into that DLL when it maps it.
//
// The thread runs x86 code from BTCpuSimulate on its own stack, inside host_simulate's loop.
// The CPU area (TEB64->TlsSlots[WOW64_TLS_CPURESERVED]: { USHORT Flags, Machine } + I386_CONTEXT)
// is the hand-over with wow64.dll: the state goes there whenever the thread leaves x86 code, and
// comes back from there when wow64 set a new one (WOW64_CPURESERVED_FLAG_RESET_STATE) or at entry.
// Every call into Windows code goes through the module's MyiosdeckWowCall, whose unwind frame
// leads back to BTCpuSimulate: wow64's longjmp (a callback's NtCallbackReturn) and exception
// dispatch then never walk the app's frames.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>

#include "fx32.h"
#include "jit_core.h"

#if __has_include("wine_version.h")
#include "wine_version.h"
#endif

extern int fxi_win_tsd_offset;   // fxi_win_host.c: the TEB's byte offset in the TSD array

// What the module hands over at process init (engine/pedylib/emu/xtajit_wow.c struct wow_process).
typedef struct {
    uint64_t base, bop;
    void *call, *system_service, *pass_exception, *raise_exception, *pending_items, *unix_call;
    uint32_t tsd_offset;
} wow_process;

typedef long long (*wow_call_fn)(void *fn, long long a0, long long a1, long long a2, void *entry_ctx);

// Per thread, in TEB64->TlsSlots[14] (free in both Windows' and Wine's WoW64 layout: Madeira's FEX
// module found 16 is ntdll's errno cell).
typedef struct {
    Fx32Cpu *cpu;
    const wow_process *proc;
    uint8_t *teb, *cpu_reserved, *ctx;   // TEB64; WOW64_CPURESERVED; its I386_CONTEXT
    uint32_t teb32;                      // guest address of the 32-bit TEB (the FS base)
    volatile int in_x86;                 // interpreting x86 code (a fault there is the guest's)
    void *entry_ctx;                     // the innermost BTCpuSimulate's captured CONTEXT
    uint64_t syscalls, unixcalls;
} wow_thread;

#define TLS_SLOT(teb, i) (((void **)((teb) + 0x1480))[i])
enum { WOW64_TLS_CPURESERVED = 1, WOW_THREAD_SLOT = 14 };
#define RESET_STATE 1

static uint8_t *teb_now(void) {
    uint64_t tsd;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(tsd));
    return *(uint8_t **)((tsd & ~7ull) + (uint64_t)fxi_win_tsd_offset);
}
static wow_thread *thread_now(void) {
    uint8_t *teb = teb_now();
    return teb ? (wow_thread *)TLS_SLOT(teb, WOW_THREAD_SLOT) : NULL;
}

static void __attribute__((noreturn)) fatal(wow_thread *t, const char *what);

static long long call(wow_thread *t, void *fn, long long a0, long long a1, long long a2) {
    return ((wow_call_fn)t->proc->call)(fn, a0, a1, a2, t->entry_ctx);
}

// ---- The state hand-over with wow64 ----
static void store(wow_thread *t) { fx32_wow_save_context(t->cpu, t->ctx); }
static void reload(wow_thread *t) {
    __atomic_fetch_and((uint16_t *)t->cpu_reserved, (uint16_t)~RESET_STATE, __ATOMIC_ACQ_REL);
    fx32_wow_load_context(t->cpu, t->ctx, t->teb32);
}
static int reset_requested(wow_thread *t) {
    return __atomic_load_n((uint16_t *)t->cpu_reserved, __ATOMIC_ACQUIRE) & RESET_STATE;
}
static uint32_t guest32(wow_thread *t, uint32_t a) { return *(const uint32_t *)(uintptr_t)(t->proc->base + a); }

// After a system or unix call: a new context (an exception, an APC, NtContinue, a callback that
// returned through longjmp) is reloaded; otherwise the call returns to its caller.
static void finish_call(wow_thread *t, uint32_t status, uint32_t pop) {
    Fx32Cpu *c = t->cpu;
    uint32_t bop = fx32_wow_eip(c);
    if (reset_requested(t)) {
        reload(t);
        if (fx32_wow_eip(c) != bop) return;   // the context was replaced: go on there
    }
    uint32_t esp = fx32_wow_reg(c, FX32_ESP);
    fx32_wow_set_reg(c, FX32_EAX, status);
    fx32_wow_set_eip(c, guest32(t, esp));
    fx32_wow_set_reg(c, FX32_ESP, esp + 4 + pop);
}

// The first calls of a run, for the log: how far a new program got.
static int trace_call(void) {
    static int n;
    return __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 48;
}

// [esp] return address into the stub, the arguments from esp + 8 (FEX's Wow64SystemServiceEx call).
static void system_call(wow_thread *t) {
    Fx32Cpu *c = t->cpu;
    uint32_t num = fx32_wow_reg(c, FX32_EAX), esp = fx32_wow_reg(c, FX32_ESP);
    store(t);
    t->syscalls++;
    if (t->proc->pending_items) call(t, t->proc->pending_items, 0, 0, 0);
    uint32_t status = (uint32_t)call(t, t->proc->system_service, num, (long long)(t->proc->base + esp + 8), 0);
    if (trace_call())
        mid_log("[fxi-wow] syscall %#x from %#x -> %08x", num, guest32(t, esp), status);
    finish_call(t, status, 0);
}

// [esp] return address, then { u64 handle; u32 code; u32 args } (popped by the callee).
static void unix_call(wow_thread *t) {
    Fx32Cpu *c = t->cpu;
    uint32_t esp = fx32_wow_reg(c, FX32_ESP);
    const uint8_t *s = (const uint8_t *)(uintptr_t)(t->proc->base + esp + 4);
    uint64_t handle; uint32_t code, args;
    memcpy(&handle, s, 8); memcpy(&code, s + 8, 4); memcpy(&args, s + 12, 4);
    store(t);
    t->unixcalls++;
    uint32_t status = (uint32_t)call(t, t->proc->unix_call, (long long)handle, code,
                                     args ? (long long)(t->proc->base + args) : 0);
    if (trace_call())
        mid_log("[fxi-wow] unix call %#llx/%u from %#x -> %08x", (unsigned long long)handle, code, guest32(t, esp), status);
    finish_call(t, status, 16);
}

// An instruction raised an exception (int3, ud2, int n, a divide error): wow64 builds the 32-bit
// record and points the context at the 32-bit KiUserExceptionDispatcher (Wow64RaiseException).
static void cpu_exception(wow_thread *t) {
    Fx32Cpu *c = t->cpu;
    fx32_wow_exc e;
    fx32_wow_exception(c, &e);
    static int logged;
    if (logged < 32) {
        logged++;
        mid_log("[fxi-wow] x86 exception %08x at %#x", e.code, e.eip);
    }
    uint8_t rec[0x98];   // EXCEPTION_RECORD (64-bit); Wow64RaiseException also writes the one it builds here
    memset(rec, 0, sizeof rec);
    if (e.code == 0x80000003) {
        // A breakpoint: Windows reports Eip after the int3 (wow64 takes one off for a software CPU).
        fx32_wow_set_eip(c, e.eip + 1);
        store(t);
        call(t, t->proc->raise_exception, 3, (long long)(uintptr_t)rec, 0);
    } else {
        fx32_wow_set_eip(c, e.eip);
        store(t);
        // A 64-bit record with host addresses: wow64 converts it once (ExceptionAddress, an access
        // violation's address) into the guest's 32-bit record.
        uint64_t addr = t->proc->base + e.eip;
        memcpy(rec, &e.code, 4);
        memcpy(rec + 4, &e.flags, 4);
        memcpy(rec + 0x10, &addr, 8);
        uint32_t np = e.nparams > 2 ? 2 : e.nparams;
        memcpy(rec + 0x18, &np, 4);
        uint64_t info[2] = { e.info[0], e.info[1] };
        if (e.code == 0xC0000005 && np > 1) info[1] = t->proc->base + (uint32_t)info[1];
        memcpy(rec + 0x20, info, 16);
        call(t, t->proc->raise_exception, -1, (long long)(uintptr_t)rec, 0);
    }
    if (reset_requested(t)) reload(t);
    else fatal(t, "an x86 exception was not dispatched (wow64 set no new context)");
}

// ---- The module's calls ----
static long host_process_init(const wow_process *p) {
    mid_log("[fxi-wow] FXI32 is the x86 CPU of this 32-bit process (no JIT): window B=%#llx, BOP page at guest %#llx, "
            "TEB slot %#x", (unsigned long long)p->base, (unsigned long long)p->bop, p->tsd_offset);
    if (!fxi_win_tsd_offset) fxi_win_tsd_offset = (int)p->tsd_offset;
    return 0;
}

static long host_thread_init(const wow_process *p, uint8_t *teb, uint8_t *cpu_reserved) {
    if (!teb || !cpu_reserved) return (long)0xC0000001;
    wow_thread *t = TLS_SLOT(teb, WOW_THREAD_SLOT);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return (long)0xC0000017;
        t->cpu = fx32_wow_cpu_new(p->base, (uint32_t)p->bop);
        if (!t->cpu) return (long)0xC0000017;
        TLS_SLOT(teb, WOW_THREAD_SLOT) = t;
    }
    t->proc = p;
    t->teb = teb;
    t->cpu_reserved = cpu_reserved;
    t->ctx = cpu_reserved + 4;                                  // RtlWow64GetCpuAreaInfo: aligned to 4
    int32_t wow_teb_offset;
    memcpy(&wow_teb_offset, teb + 0x180c, 4);                   // TEB64->WowTebOffset
    t->teb32 = (uint32_t)((uint64_t)(uintptr_t)teb + (int64_t)wow_teb_offset - p->base);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    mid_log("[fxi-wow] thread ready: TEB %p, TEB32 at guest %#x, CPU area %p", (void *)teb, t->teb32, (void *)cpu_reserved);
    return 0;
}

static long host_thread_term(uint8_t *teb) {
    wow_thread *t = teb ? TLS_SLOT(teb, WOW_THREAD_SLOT) : NULL;
    if (t) mid_log("[fxi-wow] thread exit: %llu system calls, %llu unix calls", (unsigned long long)t->syscalls,
                   (unsigned long long)t->unixcalls);
    return 0;   // the CPU stays: nothing else may be running it, and it is small
}

// BTCpuSimulate: run x86 code from the CPU area's state until wow64 unwinds this frame away.
static long host_simulate(void *entry_ctx) {
    wow_thread *t = thread_now();
    if (!t) fatal(NULL, "x86 code entered on a thread without a CPU (BTCpuThreadInit did not run)");
    t->entry_ctx = entry_ctx;
    reload(t);
    static int entries;
    if (__atomic_fetch_add(&entries, 1, __ATOMIC_RELAXED) < 16)
        mid_log("[fxi-wow] %s x86 code at %#x (esp %#x, TEB32 %#x)", entries == 1 ? "first" : "entering",
                fx32_wow_eip(t->cpu), fx32_wow_reg(t->cpu, FX32_ESP), t->teb32);
    for (;;) {
        t->entry_ctx = entry_ctx;   // a nested simulation (a callback) replaced it
        t->in_x86 = 1;
        int why = fx32_wow_run(t->cpu);
        t->in_x86 = 0;
        switch (why) {
        case FX32_RUN_SYSCALL: system_call(t); break;
        case FX32_RUN_UNIXCALL: unix_call(t); break;
        case FX32_RUN_EXCEPTION: cpu_exception(t); break;
        default: fatal(t, fx32_wow_error(t->cpu));
        }
    }
}

// Every exception the 64-bit side dispatches. One raised while this thread interprets x86 code
// (a host fault in guest memory) becomes the guest's: the state at the faulting instruction goes
// to the CPU area, and dispatch continues from BTCpuSimulate's frame, where wow64's handler
// passes it to the 32-bit dispatcher (FEX's BTCpuResetToConsistentState does the same).
static long host_reset(uint8_t **ptrs) {
    wow_thread *t = thread_now();
    if (!t || !t->in_x86 || !t->entry_ctx) return 0;
    uint8_t *rec = ptrs[0], *ctx = ptrs[1];
    t->in_x86 = 0;
    int fetch;
    uint32_t eip = fx32_wow_fault_eip(t->cpu, &fetch);
    fx32_wow_set_eip(t->cpu, eip);
    store(t);
    uint32_t code;
    memcpy(&code, rec, 4);
    uint64_t addr = t->proc->base + eip;
    memcpy(rec + 0x10, &addr, 8);                               // ExceptionAddress (host; wow64 takes B off)
    if (fetch && code == 0xC0000005) {
        uint64_t info[2] = { 8, addr };
        uint32_t np = 2;
        memcpy(rec + 0x18, &np, 4);
        memcpy(rec + 0x20, info, 16);
    }
    static int logged;
    if (logged < 32) {
        logged++;
        uint64_t a;
        memcpy(&a, rec + 0x28, 8);
        mid_log("[fxi-wow] x86 fault %08x at %#x (address %#llx%s)", code, eip, (unsigned long long)a, fetch ? ", fetching code" : "");
    }
    memcpy(ctx, t->entry_ctx, 0x390);                           // ARM64_NT_CONTEXT
    return 0;
}

// Code changed in the window: FXI32 does not drop decoded blocks yet (docs/NO_JIT_WOW64.md 2.7).
static long host_invalidate(uint64_t addr, uint64_t size) {
    static uint64_t n;
    if (++n <= 8 || !(n & (n - 1)))
        mid_log("[fxi-wow] code change notice #%llu at %#llx+%#llx (not acted on yet)", (unsigned long long)n,
                (unsigned long long)addr, (unsigned long long)size);
    return 0;
}

// IsProcessorFeaturePresent from 32-bit code: what FXI32 implements.
static long host_feature(unsigned feature) {
    switch (feature) {
    case 2:  // PF_COMPARE_EXCHANGE_DOUBLE (cmpxchg8b)
    case 6:  // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 8:  // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 9:  // PF_PAE_ENABLED
    case 10: // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
    case 12: // PF_NX_ENABLED
        return 1;
    default:
        return 0;
    }
}

// SystemCpuInformation for 32-bit code: an x86 family 6 CPU with FXI32's features (CPUID agrees).
static long host_cpu_info(uint8_t *info) {
    uint16_t arch = 0 /* PROCESSOR_ARCHITECTURE_INTEL */, level = 6, rev = 0x3a09;
    uint32_t bits = 0x2 | 0x8 | 0x80 | 0x200 | 0x800 | 0x2000 | 0x10000 | 0x20000000;   // TSC CMOV CX8 X86 FXSR SSE SSE2 NX
    memcpy(info, &arch, 2); memcpy(info + 2, &level, 2); memcpy(info + 4, &rev, 2); memcpy(info + 8, &bits, 4);
    return 0;
}

void *mid_fxi_wow_host_table(int tsd_offset) {
    static void *table[] = {
        (void *)host_process_init, (void *)host_thread_init, (void *)host_thread_term, (void *)host_simulate,
        (void *)host_reset,        (void *)host_invalidate,  (void *)host_feature,     (void *)host_cpu_info,
    };
    if (tsd_offset) fxi_win_tsd_offset = tsd_offset;
    mid_log("[fxi-wow] WoW64 CPU module ready (FXI32)");
    return table;
}

#if MYIOSDECK_WITH_WINE
long NtTerminateProcess(void *handle, long status);   // Wine's unix side
#endif

static void __attribute__((noreturn)) fatal(wow_thread *t, const char *what) {
    mid_log("[fxi-wow] STOP: %s", what);
    if (t && t->cpu) {
        Fx32Cpu *c = t->cpu;
        mid_log("[fxi-wow]   eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x eip %08x",
                fx32_wow_reg(c, FX32_EAX), fx32_wow_reg(c, FX32_ECX), fx32_wow_reg(c, FX32_EDX), fx32_wow_reg(c, FX32_EBX),
                fx32_wow_reg(c, FX32_ESP), fx32_wow_reg(c, FX32_EBP), fx32_wow_reg(c, FX32_ESI), fx32_wow_reg(c, FX32_EDI),
                fx32_wow_eip(c));
        // The code at EIP, read with vm_read_overwrite (an unmapped EIP is reported, not faulted on).
        uint8_t code[16];
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)(t->proc->base + fx32_wow_eip(c)), sizeof code,
                              (vm_address_t)code, &got) == KERN_SUCCESS) {
            char hex[3 * sizeof code + 1];
            for (unsigned i = 0; i < sizeof code; i++) snprintf(hex + 3 * i, 4, "%02x ", code[i]);
            mid_log("[fxi-wow]   code at eip: %s", hex);
        }
        mid_log("[fxi-wow]   %llu system calls, %llu unix calls on this thread", (unsigned long long)t->syscalls,
                (unsigned long long)t->unixcalls);
    }
#if MYIOSDECK_WITH_WINE
    NtTerminateProcess((void *)(intptr_t)-1, (long)0xE0F0F001);   // ends the Windows program, not the app
#endif
    abort();
}
