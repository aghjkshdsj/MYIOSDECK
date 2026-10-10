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
#include <strings.h>

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
    // Diagnostics: the last 32 system calls (number, return into the stub, the stub's caller, status).
    struct { uint32_t num, stub, caller, status; } ring[32];
    uint32_t ring_n;
} wow_thread;

static uint64_t g_last_base;   // the newest 32-bit process's window (a notice before any thread runs)

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

// After a system or unix call the CPU area is the state, reloaded every time (as wow64cpu
// restores from it after every call): a callback during the call ran x86 code on this same CPU,
// and wow64 put the caller's state back into the CPU area with BTCpuSetContext, then cleared
// RESET_STATE again as it restored the flags it saved on entry (Wow64KiUserCallbackDispatcher).
// Trusting the flag alone resumed the callback's registers: its code ran on after NtCallbackReturn
// and the next NtCallbackReturn failed (STATUS_NO_CALLBACK_ACTIVE) or KiUserCallbackDispatcher
// raised the outer call's status. A new context (an exception, an APC, NtContinue) goes on at
// its own EIP; otherwise the call returns to its caller.
static void finish_call(wow_thread *t, uint32_t status, uint32_t pop, uint32_t stop) {
    Fx32Cpu *c = t->cpu;
    reload(t);
    if (fx32_wow_eip(c) != stop) return;   // the context was replaced: go on there
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

static void find_terminate(wow_thread *t, uint32_t stub_ret);
static void exit_report(wow_thread *t, uint32_t handle, uint32_t code);
static void note_call(wow_thread *t, int unix_call, uint32_t num, uint32_t esp, uint32_t caller, uint32_t status);
static uint32_t g_terminate_num = ~0u;   // NtTerminateProcess's number, from the 32-bit ntdll's stub

// [esp] return address into the stub, [esp + 4] the stub's caller, the arguments from esp + 8
// (FEX's Wow64SystemServiceEx call).
static void system_call(wow_thread *t) {
    Fx32Cpu *c = t->cpu;
    uint32_t num = fx32_wow_reg(c, FX32_EAX), esp = fx32_wow_reg(c, FX32_ESP);
    store(t);
    t->syscalls++;
    uint32_t slot = t->ring_n++ & 31;
    t->ring[slot].num = num;
    t->ring[slot].stub = guest32(t, esp);
    uint32_t caller = guest32(t, esp + 4);
    t->ring[slot].caller = caller;
    t->ring[slot].status = ~0u;
    if (g_terminate_num == ~0u) find_terminate(t, t->ring[slot].stub);
    if (num == g_terminate_num) exit_report(t, guest32(t, esp + 8), guest32(t, esp + 12));
    if (t->proc->pending_items) call(t, t->proc->pending_items, 0, 0, 0);
    uint32_t status = (uint32_t)call(t, t->proc->system_service, num, (long long)(t->proc->base + esp + 8), 0);
    t->ring[slot].status = status;
    note_call(t, 0, num, esp, caller, status);
    if (trace_call())
        mid_log("[fxi-wow] syscall %#x from %#x -> %08x", num, guest32(t, esp), status);
    finish_call(t, status, 0, (uint32_t)t->proc->bop);
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
    note_call(t, 1, code, esp, guest32(t, esp), status);
    finish_call(t, status, 16, (uint32_t)t->proc->bop + 2);
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

// ---- Diagnostics: which 32-bit module and function an address belongs to ----
// Guest memory is read with vm_read_overwrite: a broken list or an unmapped address is reported,
// not faulted on. The 32-bit loader's list: TEB32+0x30 PEB32, +0x0c Ldr, +0x0c
// InLoadOrderModuleList; an entry: DllBase +0x18, SizeOfImage +0x20, BaseDllName +0x2c
// {Length, MaximumLength, Buffer}.
typedef struct { uint32_t base, size; char name[40]; } wow_module;

static int rd_guest(uint64_t base, uint32_t a, void *out, size_t n) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)(base + a), n, (vm_address_t)out, &got) == KERN_SUCCESS &&
           got == n;
}
static uint32_t rd_guest32(uint64_t base, uint32_t a) { uint32_t v = 0; return rd_guest(base, a, &v, 4) ? v : 0; }
static uint16_t rd_guest16(uint64_t base, uint32_t a) { uint16_t v = 0; return rd_guest(base, a, &v, 2) ? v : 0; }

static int wow_modules(uint64_t base, uint32_t teb32, wow_module *m, int max) {
    uint32_t peb = rd_guest32(base, teb32 + 0x30), ldr = peb ? rd_guest32(base, peb + 0x0c) : 0;
    if (!ldr) return 0;
    uint32_t head = ldr + 0x0c, e = rd_guest32(base, head);
    int n = 0;
    for (int guard = 0; e && e != head && n < max && guard < 1024; guard++, e = rd_guest32(base, e)) {
        m[n].base = rd_guest32(base, e + 0x18);
        m[n].size = rd_guest32(base, e + 0x20);
        uint16_t len = rd_guest16(base, e + 0x2c), w[39];
        uint32_t buf = rd_guest32(base, e + 0x30);
        unsigned k = len / 2 < 39 ? len / 2 : 39;
        if (!buf || !rd_guest(base, buf, w, k * 2)) k = 0;
        for (unsigned i = 0; i < k; i++) m[n].name[i] = w[i] < 0x80 ? (char)w[i] : '?';
        m[n].name[k] = 0;
        n++;
    }
    return n;
}

// A PE32 image at img: its SizeOfImage (0: not an image).
static uint32_t pe_size(uint64_t base, uint32_t img) {
    if (rd_guest16(base, img) != 0x5a4d) return 0;
    uint32_t pe = img + rd_guest32(base, img + 0x3c);
    if (rd_guest32(base, pe) != 0x4550 || rd_guest16(base, pe + 0x18) != 0x10b) return 0;
    return rd_guest32(base, pe + 0x18 + 56);
}
// The image containing a (images start on 64 KB boundaries): one no longer on the loader's list.
static uint32_t pe_find(uint64_t base, uint32_t a) {
    for (uint32_t img = a & ~0xffffu, i = 0; i < 1024; i++, img -= 0x10000) {
        uint32_t size = pe_size(base, img);
        if (size && a - img < size) return img;
        if (!img) break;
    }
    return 0;
}

// The image's export directory, read whole (the name, function, ordinal arrays and the strings
// are inside it as linkers lay it out).
typedef struct { uint8_t *d; uint32_t rva, size; } pe_exp;
static int exp_load(uint64_t base, uint32_t img, pe_exp *e) {
    e->d = NULL;
    if (!pe_size(base, img)) return 0;
    uint32_t opt = img + rd_guest32(base, img + 0x3c) + 0x18;
    e->rva = rd_guest32(base, opt + 96);
    e->size = rd_guest32(base, opt + 100);
    if (!e->rva || e->size < 0x28 || e->size > (8u << 20)) return 0;
    e->d = malloc(e->size);
    if (e->d && rd_guest(base, img + e->rva, e->d, e->size)) return 1;
    free(e->d);
    e->d = NULL;
    return 0;
}
static const uint8_t *exp_at(const pe_exp *e, uint32_t rva, uint32_t n) {
    return rva >= e->rva && rva - e->rva <= e->size && n <= e->size - (rva - e->rva) ? e->d + (rva - e->rva) : NULL;
}
static uint32_t exp_u32(const pe_exp *e, uint32_t off) { uint32_t v = 0; memcpy(&v, e->d + off, 4); return v; }
static const char *exp_str(const pe_exp *e, uint32_t rva) {
    const uint8_t *s = exp_at(e, rva, 1);
    return s && memchr(s, 0, e->size - (rva - e->rva)) ? (const char *)s : NULL;
}
// The export at or below rva within max bytes: "name" or "name+off".
static int exp_near(const pe_exp *e, uint32_t rva, uint32_t max, int with_off, char *out, size_t len) {
    uint32_t nfunc = exp_u32(e, 0x14), nname = exp_u32(e, 0x18);
    const uint8_t *funcs = exp_at(e, exp_u32(e, 0x1c), nfunc * 4), *names = exp_at(e, exp_u32(e, 0x20), nname * 4),
                  *ords = exp_at(e, exp_u32(e, 0x24), nname * 2);
    if (!funcs || !names || !ords || nfunc > 65536 || nname > 65536) return 0;
    int best = -1;
    uint32_t best_rva = 0;
    for (uint32_t i = 0; i < nname; i++) {
        uint16_t o; uint32_t f;
        memcpy(&o, ords + 2 * i, 2);
        if (o >= nfunc) continue;
        memcpy(&f, funcs + 4 * o, 4);
        if (f <= rva && rva - f <= max && (best < 0 || f > best_rva)) { best = (int)i; best_rva = f; }
    }
    if (best < 0) return 0;
    uint32_t name_rva;
    memcpy(&name_rva, names + 4 * (uint32_t)best, 4);
    const char *name = exp_str(e, name_rva);
    if (!name) return 0;
    if (!with_off || rva == best_rva) snprintf(out, len, "%s", name);
    else snprintf(out, len, "%s+%#x", name, rva - best_rva);
    return 1;
}
static uint32_t exp_find(const pe_exp *e, const char *want) {
    uint32_t nfunc = exp_u32(e, 0x14), nname = exp_u32(e, 0x18);
    const uint8_t *funcs = exp_at(e, exp_u32(e, 0x1c), nfunc * 4), *names = exp_at(e, exp_u32(e, 0x20), nname * 4),
                  *ords = exp_at(e, exp_u32(e, 0x24), nname * 2);
    if (!funcs || !names || !ords || nfunc > 65536 || nname > 65536) return 0;
    for (uint32_t i = 0; i < nname; i++) {
        uint32_t name_rva, f;
        uint16_t o;
        memcpy(&name_rva, names + 4 * i, 4);
        const char *name = exp_str(e, name_rva);
        if (!name || strcmp(name, want)) continue;
        memcpy(&o, ords + 2 * i, 2);
        if (o >= nfunc) return 0;
        memcpy(&f, funcs + 4 * o, 4);
        return f;
    }
    return 0;
}

// "module+offset (export+off)" for a guest address; images off the loader's list by their
// export directory's name.
static const char *describe(uint64_t base, const wow_module *m, int n, uint32_t a, char *buf, size_t len) {
    uint32_t img = 0;
    const char *mod = NULL;
    for (int i = 0; i < n && !mod; i++)
        if (a >= m[i].base && a - m[i].base < m[i].size) { img = m[i].base; mod = m[i].name; }
    char own[48] = "", fn[64] = "";
    pe_exp e;
    if (!mod && (img = pe_find(base, a))) mod = own;
    if (!mod) { snprintf(buf, len, "outside every module"); return buf; }
    if (exp_load(base, img, &e)) {
        if (mod == own) { const char *s = exp_str(&e, exp_u32(&e, 0x0c)); snprintf(own, sizeof own, "%s", s ? s : "?"); }
        exp_near(&e, a - img, 0x1000, 1, fn, sizeof fn);
        free(e.d);
    }
    if (mod == own && !own[0]) snprintf(own, sizeof own, "image@%08x", img);
    if (fn[0]) snprintf(buf, len, "%s+%#x (%s)", mod, a - img, fn);
    else snprintf(buf, len, "%s+%#x", mod, a - img);
    return buf;
}

// Return addresses on the x86 stack: values inside a module just after a call (e8 rel32, or
// ff /2 in its register and memory forms).
static int after_call(uint64_t base, uint32_t v) {
    uint8_t b[7];   // b[k] is at v - 7 + k
    if (v < 7 || !rd_guest(base, v - 7, b, 7)) return 0;
    if (b[2] == 0xe8) return 1;
    if (b[5] == 0xff && (b[6] & 0x38) == 0x10 && (b[6] >> 6 == 3 || (b[6] >> 6 == 0 && (b[6] & 7) != 4 && (b[6] & 7) != 5)))
        return 1;   // call reg / [reg]
    if (b[4] == 0xff && (b[5] & 0x38) == 0x10 && b[5] >> 6 == 1 && (b[5] & 7) != 4) return 1;   // [reg + disp8]
    if (b[3] == 0xff && (b[4] & 0x38) == 0x10 && b[4] >> 6 == 1 && (b[4] & 7) == 4) return 1;   // [sib + disp8]
    if (b[1] == 0xff && (b[2] & 0x38) == 0x10 && ((b[2] >> 6 == 0 && (b[2] & 7) == 5) || (b[2] >> 6 == 2 && (b[2] & 7) != 4)))
        return 1;   // [disp32] / [reg + disp32]
    if (b[0] == 0xff && (b[1] & 0x38) == 0x10 && b[1] >> 6 == 2 && (b[1] & 7) == 4) return 1;   // [sib + disp32]
    return 0;
}

static void stack_report(wow_thread *t, const wow_module *m, int n, int max) {
    uint64_t base = t->proc->base;
    uint32_t esp = fx32_wow_reg(t->cpu, FX32_ESP);
    static uint32_t words[0x1000];
    size_t sz = sizeof words;
    while (sz >= 0x100 && !rd_guest(base, esp, words, sz)) sz /= 2;
    if (sz < 0x100) { mid_log("[fxi-wow]   (the x86 stack at %#x is unreadable)", esp); return; }
    int shown = 0;
    for (size_t i = 0; i < sz / 4 && shown < max; i++) {
        uint32_t v = words[i];
        int in = 0;
        for (int k = 0; k < n && !in; k++) in = v >= m[k].base && v - m[k].base < m[k].size;
        if (!in || !after_call(base, v)) continue;
        char where[128];
        mid_log("[fxi-wow]   esp+%#05zx: return to %08x %s", i * 4, v, describe(base, m, n, v, where, sizeof where));
        shown++;
    }
    if (!shown) mid_log("[fxi-wow]   (no return addresses into modules in %#zx bytes from esp %#x)", sz, esp);
}

static void syscall_report(wow_thread *t, const wow_module *m, int n) {
    uint64_t base = t->proc->base;
    uint32_t count = t->ring_n < 32 ? t->ring_n : 32;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t k = (t->ring_n - count + i) & 31;
        char fn[64] = "", where[128], st[16];
        uint32_t img = 0;
        for (int j = 0; j < n && !img; j++)
            if (t->ring[k].stub - m[j].base < m[j].size) img = m[j].base;
        if (!img) img = pe_find(base, t->ring[k].stub);
        pe_exp e;
        if (img && exp_load(base, img, &e)) {
            exp_near(&e, t->ring[k].stub - img, 0x40, 0, fn, sizeof fn);   // the stub's export
            free(e.d);
        }
        if (t->ring[k].status == ~0u) snprintf(st, sizeof st, "(running)");
        else snprintf(st, sizeof st, "%08x", t->ring[k].status);
        mid_log("[fxi-wow]   syscall %#05x %-32s -> %s from %s", t->ring[k].num, fn[0] ? fn : "?", st,
                describe(base, m, n, t->ring[k].caller, where, sizeof where));
    }
}

static void module_report(const wow_module *m, int n) {
    for (int i = 0; i < n; i++) mid_log("[fxi-wow]   module %08x-%08x %s", m[i].base, m[i].base + m[i].size, m[i].name);
}

static void fatal_report(wow_thread *t) {
    Fx32Cpu *c = t->cpu;
    uint64_t base = t->proc->base;
    static wow_module mods[160];
    int nm = wow_modules(base, t->teb32, mods, 160);
    char where[128];
    mid_log("[fxi-wow]   eip %08x is %s", fx32_wow_eip(c), describe(base, mods, nm, fx32_wow_eip(c), where, sizeof where));
    uint32_t trail[16];
    int nt = fx32_wow_trail(c, trail, 16);
    for (int i = 0; i < nt; i++) {
        uint8_t code[12] = { 0 };
        rd_guest(base, trail[i], code, sizeof code);
        char hex[3 * sizeof code + 1];
        for (unsigned k = 0; k < sizeof code; k++) snprintf(hex + 3 * k, 4, "%02x ", code[k]);
        mid_log("[fxi-wow]   jump %2d/%d to %08x (%s): %s", i + 1, nt, trail[i], describe(base, mods, nm, trail[i], where, sizeof where), hex);
    }
    mid_log("[fxi-wow]   the last system calls on this thread, oldest first:");
    syscall_report(t, mods, nm);
    module_report(mods, nm);
}

// NtTerminateProcess's number: the 32-bit ntdll's export starts "mov eax, imm32" (b8), the number
// (through one jmp if it has one). Found from the image the first system calls return into.
static void find_terminate(wow_thread *t, uint32_t stub_ret) {
    static int tries;
    if (__atomic_fetch_add(&tries, 1, __ATOMIC_RELAXED) >= 8) return;
    uint64_t base = t->proc->base;
    uint32_t img = pe_find(base, stub_ret), rva = 0;
    pe_exp e;
    if (!img || !exp_load(base, img, &e)) return;
    rva = exp_find(&e, "NtTerminateProcess");
    free(e.d);
    uint8_t stub[5] = { 0 };
    uint32_t at = img + rva;
    if (rva && rd_guest(base, at, stub, 5) && stub[0] == 0xe9) {
        int32_t rel;
        memcpy(&rel, stub + 1, 4);
        at = at + 5 + (uint32_t)rel;
        if (!rd_guest(base, at, stub, 5)) return;
    }
    if (rva && stub[0] == 0xb8) {
        uint32_t num;
        memcpy(&num, stub + 1, 4);
        __atomic_store_n(&g_terminate_num, num, __ATOMIC_RELAXED);
        mid_log("[fxi-wow] NtTerminateProcess is system call %#x (32-bit ntdll at guest %#x)", num, img);
    }
}

// The 32-bit program ends itself (NtTerminateProcess(0 or -1)): why is in its last steps. Once
// per run: who called it (return addresses on the stack), its last system calls, its modules.
static void exit_report(wow_thread *t, uint32_t handle, uint32_t code) {
    if (handle && handle != 0xffffffffu) return;   // another process
    static int reported;
    if (__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) return;
    uint64_t base = t->proc->base;
    mid_log("[fxi-wow] the 32-bit program ends itself: NtTerminateProcess(%s, exit code %#x = %u), %llu system calls and "
            "%llu unix calls on this thread", handle ? "self" : "0", code, code, (unsigned long long)t->syscalls,
            (unsigned long long)t->unixcalls);
    static wow_module mods[160];
    int nm = wow_modules(base, t->teb32, mods, 160);
    mid_log("[fxi-wow]   who called it (return addresses on the x86 stack, innermost first):");
    stack_report(t, mods, nm, 24);
    mid_log("[fxi-wow]   the last system calls on this thread, oldest first:");
    syscall_report(t, mods, nm);
    module_report(mods, nm);
}

// ---- Diagnostics: failed calls by name, and every process start ----
// The 32-bit ntdll's and win32u's Nt* exports start "mov eax, imm32" (b8): that number is the
// system call's. The table is filled once each image is loaded.
enum { SYSNAMES = 0x2000 };
static const char *g_sysname[SYSNAMES];
static int g_sysnames_ntdll, g_sysnames_win32u;
static uint32_t g_create_num = ~0u;   // NtCreateUserProcess

static void sysnames_from(uint64_t base, uint32_t img) {
    pe_exp e;
    if (!exp_load(base, img, &e)) return;
    uint32_t nfunc = exp_u32(&e, 0x14), nname = exp_u32(&e, 0x18);
    const uint8_t *funcs = exp_at(&e, exp_u32(&e, 0x1c), nfunc * 4), *names = exp_at(&e, exp_u32(&e, 0x20), nname * 4),
                  *ords = exp_at(&e, exp_u32(&e, 0x24), nname * 2);
    for (uint32_t i = 0; funcs && names && ords && nfunc <= 65536 && i < nname && nname <= 65536; i++) {
        uint32_t name_rva, f, num;
        uint16_t o;
        uint8_t stub[5];
        memcpy(&name_rva, names + 4 * i, 4);
        const char *name = exp_str(&e, name_rva);
        if (!name || strncmp(name, "Nt", 2)) continue;
        memcpy(&o, ords + 2 * i, 2);
        if (o >= nfunc) continue;
        memcpy(&f, funcs + 4 * o, 4);
        if (!rd_guest(base, img + f, stub, 5) || stub[0] != 0xb8) continue;
        memcpy(&num, stub + 1, 4);
        if (num >= SYSNAMES || g_sysname[num]) continue;
        g_sysname[num] = strdup(name);
        if (!strcmp(name, "NtCreateUserProcess")) g_create_num = num;
    }
    free(e.d);
}

static const char *sysname(wow_thread *t, uint32_t num) {
    if (!g_sysnames_ntdll || !g_sysnames_win32u) {
        static wow_module mods[160];
        int n = wow_modules(t->proc->base, t->teb32, mods, 160);
        for (int i = 0; i < n; i++) {
            if (!g_sysnames_ntdll && !strcasecmp(mods[i].name, "ntdll.dll")) {
                g_sysnames_ntdll = 1;
                sysnames_from(t->proc->base, mods[i].base);
            }
            if (!g_sysnames_win32u && !strcasecmp(mods[i].name, "win32u.dll")) {
                g_sysnames_win32u = 1;
                sysnames_from(t->proc->base, mods[i].base);
            }
        }
    }
    return num < SYSNAMES ? g_sysname[num] : NULL;
}

// A UNICODE_STRING32 of RTL_USER_PROCESS_PARAMETERS (Buffer relative to the block unless it is
// normalized, Flags bit 0) as ASCII, '?' for the rest.
static void params_string(uint64_t base, uint32_t params, uint32_t off, char *out, size_t len) {
    uint16_t n = rd_guest16(base, params + off), w[260];
    uint32_t buf = rd_guest32(base, params + off + 4);
    if (!(rd_guest32(base, params + 8) & 1)) buf += params;
    unsigned k = n / 2 < 260 ? n / 2 : 260;
    if (k >= len) k = (unsigned)len - 1;
    if (!buf || !rd_guest(base, buf, w, k * 2)) k = 0;
    for (unsigned i = 0; i < k; i++) out[i] = w[i] && w[i] < 0x80 ? (char)w[i] : '?';
    out[k] = 0;
}

// After a system or unix call. A failure (an NTSTATUS error) is logged by name with its caller,
// once per (call, status, caller), the first 400 of a run: what a program tried and could not do
// before it stopped or did nothing. Every process start is logged with its image and command
// line, whatever the result (a launcher whose button does nothing).
static void note_call(wow_thread *t, int unix_call, uint32_t num, uint32_t esp, uint32_t caller, uint32_t status) {
    int create = !unix_call && num == g_create_num;
    if (status < 0xC0000000u && !create) return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t seen[1024];
    static unsigned distinct;
    uint64_t key = ((uint64_t)unix_call << 63) ^ ((uint64_t)num << 40) ^ ((uint64_t)caller << 8) ^ (status & 0xff) ^
                   ((uint64_t)(status >> 8 & 0xff) << 32);
    if (!key) key = 1;
    pthread_mutex_lock(&lock);
    if (!create) {
        unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 54);
        while (seen[h] && seen[h] != key) h = (h + 1) & 1023;
        if (seen[h] || distinct >= 400) { pthread_mutex_unlock(&lock); return; }
        seen[h] = key;
        distinct++;
    }
    uint64_t base = t->proc->base;
    static wow_module mods[160];
    int nm = wow_modules(base, t->teb32, mods, 160);
    char where[128];
    describe(base, mods, nm, caller, where, sizeof where);
    if (create) {
        // NtCreateUserProcess(..., RTL_USER_PROCESS_PARAMETERS *params (9th), ...): ImagePathName
        // at 0x38, CommandLine at 0x40 of the 32-bit block.
        uint32_t params = rd_guest32(base, esp + 8 + 8 * 4);
        char image[200] = "", cmd[200] = "";
        if (params) { params_string(base, params, 0x38, image, sizeof image); params_string(base, params, 0x40, cmd, sizeof cmd); }
        mid_log("[fxi-wow] start process \"%s\" (command line \"%s\") -> %08x from %s", image, cmd, status, where);
    } else if (unix_call) {
        mid_log("[fxi-wow] failed: unix call %u -> %08x from %s", num, status, where);
    } else {
        const char *name = sysname(t, num);
        if (name) mid_log("[fxi-wow] failed: %s -> %08x from %s", name, status, where);
        else mid_log("[fxi-wow] failed: system call %#x -> %08x from %s", num, status, where);
    }
    pthread_mutex_unlock(&lock);
}

// ---- The module's calls ----
static long host_process_init(const wow_process *p) {
    mid_log("[fxi-wow] FXI32 is the x86 CPU of this 32-bit process (no JIT): window B=%#llx, BOP page at guest %#llx, "
            "TEB slot %#x", (unsigned long long)p->base, (unsigned long long)p->bop, p->tsd_offset);
    if (!fxi_win_tsd_offset) fxi_win_tsd_offset = (int)p->tsd_offset;
    g_last_base = p->base;
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

// Code changed in the window (a view unmapped, memory freed or re-protected, a file read into it,
// another process's write): FXI32 drops its decoded blocks there (docs/NO_JIT_WOW64.md 2.7).
// Addresses arrive as host addresses; size 0 (an unmapped view, a full flush) drops every block.
static long host_invalidate(uint64_t addr, uint64_t size) {
    wow_thread *t = thread_now();
    uint64_t base = t && t->proc ? t->proc->base : g_last_base;
    if (!base) return 0;
    uint32_t start = 0, len = 0;
    if (size) {
        if (addr < base || addr - base >= (1ull << 32)) return 0;   // not in this process's window
        start = (uint32_t)(addr - base);
        len = size > 0xffffffffull - start ? 0xffffffffu - start : (uint32_t)size;
    }
    unsigned dropped = fx32_wow_invalidate(base, start, len);
    static uint64_t n, n_dropped;
    n++;
    if (dropped && (++n_dropped <= 16 || !(n_dropped & (n_dropped - 1)))) {
        // A view unmapped at an image's base (a DLL unloaded: already off the loader's list, still
        // mapped): named by its export directory.
        char what[64] = "";
        pe_exp e;
        if (len && !(start & 0xffff) && exp_load(base, start, &e)) {
            const char *s = exp_str(&e, exp_u32(&e, 0x0c));
            snprintf(what, sizeof what, " (the image %s)", s ? s : "?");
            free(e.d);
        } else if (len && !(start & 0xffff) && pe_size(base, start)) {
            snprintf(what, sizeof what, " (an image without exports)");
        }
        mid_log("[fxi-wow] code changed at guest %#x+%#x%s: %u decoded blocks dropped (change #%llu)", start, len, what,
                dropped, (unsigned long long)n);
    }
    return 0;
}

// wow64's NtTerminateProcess(0, code), the first step of ExitProcess (BTCpuProcessTerm): the exit
// report, if the system-call number did not already give it.
static long host_process_term(uint64_t handle, long after) {
    wow_thread *t = thread_now();
    if (after || handle || !t || !t->proc || !t->cpu) return 0;
    exit_report(t, 0, guest32(t, fx32_wow_reg(t->cpu, FX32_ESP) + 12));
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
        (void *)host_process_term,
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
        if (t->proc) fatal_report(t);
    }
#if MYIOSDECK_WITH_WINE
    NtTerminateProcess((void *)(intptr_t)-1, (long)0xE0F0F001);   // ends the Windows program, not the app
#endif
    abort();
}
