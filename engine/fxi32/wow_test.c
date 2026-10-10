// SPDX-License-Identifier: GPL-3.0-or-later
// FXI32's WoW64 API (fx32.h) on Linux, the way the app's host (App/Sources/Native/fxi_wow_host.c)
// drives it (docs/NO_JIT_WOW64.md, stage 3): a 4 GB guest window at a 4 GB-aligned host address,
// a TEB32 for fs:, a BOP page, and hand-assembled i386 code that makes a system call and a unix
// call, raises int3 and a divide error, faults on an unmapped guest address, and runs on
// after each, then runs code that changes under an invalidation. Each check prints one line;
// any failure exits 1.
//   wow_test        (built by engine/fxi32/build.sh, run by fxi.yml)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fx32.h"

static int g_fail;
static void check(const char *what, int ok, const char *detail) {
    printf("%-34s %s  %s\n", what, ok ? "ok  " : "FAIL", detail);
    if (!ok) g_fail = 1;
}

static uint8_t *g_base;   // host address of guest 0
static uint8_t *G(uint32_t a) { return g_base + a; }
static uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }

enum { CODE = 0x00401000, DATA = 0x00500000, BOP = 0x00600000, TEB32 = 0x7ffd0000, STACK_TOP = 0x00800000 };

static sigjmp_buf g_fault_jb;
static volatile uint64_t g_fault_addr;
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    g_fault_addr = (uint64_t)(uintptr_t)si->si_addr;
    siglongjmp(g_fault_jb, 1);
}

static void put(uint32_t at, const uint8_t *bytes, size_t n) { memcpy(G(at), bytes, n); }
static void put32(uint32_t at, uint32_t v) { memcpy(G(at), &v, 4); }

int main(void) {
    size_t res = 3ull << 32;
    uint8_t *r = mmap(0, res, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) { perror("mmap"); return 1; }
    g_base = (uint8_t *)(((uintptr_t)r + 0xffffffffull) & ~(uintptr_t)0xffffffffull);
    // Guest memory: code, data, the BOP page, a stack, the TEB32. Everything else faults.
    if (mprotect(G(CODE), 0x4000, PROT_READ | PROT_WRITE) || mprotect(G(DATA), 0x4000, PROT_READ | PROT_WRITE) ||
        mprotect(G(BOP), 0x4000, PROT_READ | PROT_WRITE) || mprotect(G(STACK_TOP - 0x10000), 0x10000, PROT_READ | PROT_WRITE) ||
        mprotect(G(TEB32), 0x4000, PROT_READ | PROT_WRITE)) { perror("mprotect"); return 1; }
    put32(BOP, 0x2ecd2ecd);
    put32(DATA, BOP);                    // __wine_syscall_dispatcher
    put32(DATA + 4, BOP + 2);            // __wine_unix_call_dispatcher
    put32(TEB32 + 0x18, TEB32);          // TEB32->Tib.Self

    // Program: a function that makes system call 0x1234(0x11, 0x22) through a Wine-style stub,
    // then a unix call (handle 0x1122334455667788, code 9, args 0x500100), reads fs:[0x18],
    // int3, a divide error, a load from unmapped guest 0x10, and stops with ud2.
    uint32_t pc = CODE;
    const uint8_t prog[] = {
        0x6a, 0x22,                         //  0 push 0x22
        0x6a, 0x11,                         //  2 push 0x11
        0xe8, 0x0b, 0x00, 0x00, 0x00,       //  4 call stub (20)
        0x89, 0xc3,                         //  9 mov ebx, eax           (the system call's status)
        // unix call: push args, code, handle high, handle low; call [DATA + 4]
        0x68, 0x00, 0x01, 0x50, 0x00,       // 11 push 0x500100
        0x6a, 0x09,                         // 16 push 9
        0xeb, 0x0e,                         // 18 jmp 34 (over the stub)
        // stub: mov eax, 0x1234 ; call [DATA] ; ret 8 (a Wine-style 32-bit system call stub)
        0xb8, 0x34, 0x12, 0x00, 0x00,       // 20
        0xff, 0x15, 0x00, 0x00, 0x50, 0x00, // 25 call [DATA]            (= BOP; returns to 31)
        0xc2, 0x08, 0x00,                   // 31 ret 8
        0x68, 0x55, 0x66, 0x77, 0x11,       // 34 push 0x11776655        (handle high)
        0x68, 0x88, 0x77, 0x66, 0x55,       // 39 push 0x55667788        (handle low)
        0xff, 0x15, 0x04, 0x00, 0x50, 0x00, // 44 call [DATA + 4]        (= BOP + 2; returns to 50)
        0x89, 0xc6,                         // 50 mov esi, eax
        0x64, 0xa1, 0x18, 0x00, 0x00, 0x00, // 52 mov eax, fs:[0x18]     (moffs, fs: the TEB32)
        0x89, 0xc7,                         // 58 mov edi, eax
        0xcc,                               // 60 int3
        0x31, 0xc9,                         // 61 xor ecx, ecx
        0x31, 0xd2,                         // 63 xor edx, edx
        0xb8, 0x07, 0x00, 0x00, 0x00,       // 65 mov eax, 7
        0xf7, 0xf1,                         // 70 div ecx                (#DE)
        0xa1, 0x10, 0x00, 0x00, 0x00,       // 72 mov eax, [0x10]        (unmapped guest memory)
        0x0f, 0x0b,                         // 77 ud2
    };
    put(pc, prog, sizeof prog);

    Fx32Cpu *c = fx32_wow_cpu_new((uint64_t)(uintptr_t)g_base, BOP);
    // The initial context the way Wine's unix side writes it (CONTEXT_I386_ALL, fpu defaults).
    uint8_t ctx[0x2cc];
    memset(ctx, 0, sizeof ctx);
    uint32_t v;
    v = 0x1003f; memcpy(ctx, &v, 4);                     // ContextFlags: CONTEXT_I386_ALL
    v = STACK_TOP - 16; memcpy(ctx + 0xc4, &v, 4);       // Esp
    v = CODE; memcpy(ctx + 0xb8, &v, 4);                 // Eip
    v = 0x202; memcpy(ctx + 0xc0, &v, 4);                // EFlags
    uint16_t fcw = 0x27f; memcpy(ctx + 0xcc, &fcw, 2);   // ExtendedRegisters.ControlWord
    v = 0x1f80; memcpy(ctx + 0xcc + 24, &v, 4);          // MxCsr
    fx32_wow_load_context(c, ctx, TEB32);
    char d[160];

    // 1. The system call: stops at the BOP page with [esp] = return into the stub and the
    //    arguments at esp + 8.
    int why = fx32_wow_run(c);
    uint32_t esp = fx32_wow_reg(c, FX32_ESP);
    snprintf(d, sizeof d, "why %d eip %#x eax %#x [esp] %#x args %#x %#x", why, fx32_wow_eip(c), fx32_wow_reg(c, FX32_EAX),
             rd(esp), rd(esp + 8), rd(esp + 12));
    check("system call stops at the BOP page", why == FX32_RUN_SYSCALL && fx32_wow_eip(c) == BOP &&
          fx32_wow_reg(c, FX32_EAX) == 0x1234 && rd(esp) == CODE + 31 && rd(esp + 8) == 0x11 && rd(esp + 12) == 0x22, d);
    // The state goes to the CPU area around the call and back (fxi_wow_host.c store/reload).
    uint8_t saved[0x2cc];
    fx32_wow_save_context(c, saved);
    uint32_t seip, sesp, sfl;
    memcpy(&seip, saved + 0xb8, 4); memcpy(&sesp, saved + 0xc4, 4); memcpy(&sfl, saved, 4);
    snprintf(d, sizeof d, "flags %#x eip %#x esp %#x", sfl, seip, sesp);
    check("context saved at the system call", sfl == 0x1002f && seip == BOP && sesp == esp, d);
    fx32_wow_load_context(c, saved, TEB32);
    // Return as fxi_wow_host.c finish_call does: eax = status, eip = [esp], esp += 4.
    fx32_wow_set_reg(c, FX32_EAX, 0xc0000022);
    fx32_wow_set_eip(c, rd(esp));
    fx32_wow_set_reg(c, FX32_ESP, esp + 4);

    // 2. The unix call: [esp] = return address, then handle (8 bytes), code, args.
    why = fx32_wow_run(c);
    esp = fx32_wow_reg(c, FX32_ESP);
    uint64_t handle;
    memcpy(&handle, G(esp + 4), 8);
    snprintf(d, sizeof d, "why %d eip %#x handle %#llx code %u args %#x ebx %#x", why, fx32_wow_eip(c),
             (unsigned long long)handle, rd(esp + 12), rd(esp + 16), fx32_wow_reg(c, FX32_EBX));
    check("unix call stops at BOP + 2", why == FX32_RUN_UNIXCALL && fx32_wow_eip(c) == BOP + 2 &&
          handle == 0x1177665555667788ull && rd(esp + 12) == 9 && rd(esp + 16) == 0x500100 &&
          fx32_wow_reg(c, FX32_EBX) == 0xc0000022, d);
    fx32_wow_set_reg(c, FX32_EAX, 0x77);
    fx32_wow_set_eip(c, rd(esp));
    fx32_wow_set_reg(c, FX32_ESP, esp + 4 + 16);

    // 3. fs:[0x18] reads the TEB32 through the window; int3 stops with the breakpoint.
    why = fx32_wow_run(c);
    fx32_wow_exc e;
    if (why == FX32_RUN_EXCEPTION) fx32_wow_exception(c, &e); else memset(&e, 0, sizeof e);
    snprintf(d, sizeof d, "why %d code %#x eip %#x esi %#x edi %#x", why, e.code, e.eip, fx32_wow_reg(c, FX32_ESI),
             fx32_wow_reg(c, FX32_EDI));
    check("fs: and int3", why == FX32_RUN_EXCEPTION && e.code == 0x80000003 && e.eip == CODE + 60 &&
          fx32_wow_reg(c, FX32_ESI) == 0x77 && fx32_wow_reg(c, FX32_EDI) == TEB32, d);
    fx32_wow_set_eip(c, CODE + 61);

    // 4. A divide error at the div, the state before it.
    why = fx32_wow_run(c);
    if (why == FX32_RUN_EXCEPTION) fx32_wow_exception(c, &e); else memset(&e, 0, sizeof e);
    snprintf(d, sizeof d, "why %d code %#x eip %#x eax %#x", why, e.code, e.eip, fx32_wow_reg(c, FX32_EAX));
    check("divide error", why == FX32_RUN_EXCEPTION && e.code == 0xC0000094 && e.eip == CODE + 70 &&
          fx32_wow_reg(c, FX32_EAX) == 7, d);
    fx32_wow_set_eip(c, CODE + 72);

    // 5. A host fault on unmapped guest 0x10: the exact faulting instruction.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    if (!sigsetjmp(g_fault_jb, 1)) {
        why = fx32_wow_run(c);
        check("fault on unmapped guest memory", 0, "no fault");
    } else {
        int fetch;
        uint32_t feip = fx32_wow_fault_eip(c, &fetch);
        snprintf(d, sizeof d, "eip %#x fetch %d address guest %#llx", feip, fetch,
                 (unsigned long long)(g_fault_addr - (uint64_t)(uintptr_t)g_base));
        check("fault on unmapped guest memory", feip == CODE + 72 && !fetch && g_fault_addr == (uint64_t)(uintptr_t)G(0x10), d);
    }
    signal(SIGSEGV, SIG_DFL);
    fx32_wow_set_eip(c, CODE + 77);

    // 6. ud2: an illegal instruction.
    why = fx32_wow_run(c);
    if (why == FX32_RUN_EXCEPTION) fx32_wow_exception(c, &e); else memset(&e, 0, sizeof e);
    snprintf(d, sizeof d, "why %d code %#x eip %#x", why, e.code, e.eip);
    check("ud2", why == FX32_RUN_EXCEPTION && e.code == 0xC000001D && e.eip == CODE + 77, d);

    // 7. Code that changes (a DLL unloaded and another loaded at its address): a jmp at +0x200
    //    to "mov eax, 1; ud2" at +0x100. Rewritten to "mov eax, 2", the old block runs on until
    //    fx32_wow_invalidate drops it; then the jmp's chained link re-translates the new code.
    const uint8_t mov1[] = { 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x0b };
    const uint8_t jmp[] = { 0xe9, 0xfb, 0xfe, 0xff, 0xff };   // jmp CODE + 0x100
    put(CODE + 0x100, mov1, sizeof mov1);
    put(CODE + 0x200, jmp, sizeof jmp);
    uint32_t eax[4];
    fx32_wow_set_eip(c, CODE + 0x200);
    fx32_wow_run(c);
    eax[0] = fx32_wow_reg(c, FX32_EAX);
    G(CODE + 0x101)[0] = 2;
    fx32_wow_set_eip(c, CODE + 0x200);
    fx32_wow_run(c);
    eax[1] = fx32_wow_reg(c, FX32_EAX);
    unsigned missed = fx32_wow_invalidate((uint64_t)(uintptr_t)g_base, CODE + 0x300, 0x100);
    unsigned dropped = fx32_wow_invalidate((uint64_t)(uintptr_t)g_base, CODE + 0x104, 1);
    fx32_wow_set_eip(c, CODE + 0x200);
    why = fx32_wow_run(c);
    if (why == FX32_RUN_EXCEPTION) fx32_wow_exception(c, &e); else memset(&e, 0, sizeof e);
    eax[2] = fx32_wow_reg(c, FX32_EAX);
    G(CODE + 0x101)[0] = 3;
    unsigned all = fx32_wow_invalidate((uint64_t)(uintptr_t)g_base, 0, 0);
    fx32_wow_set_eip(c, CODE + 0x200);
    fx32_wow_run(c);
    eax[3] = fx32_wow_reg(c, FX32_EAX);
    snprintf(d, sizeof d, "eax %u %u %u %u, dropped %u (unrelated range %u, all %u), ud2 at %#x", eax[0], eax[1], eax[2],
             eax[3], dropped, missed, all, e.eip);
    check("changed code after invalidation", eax[0] == 1 && eax[1] == 1 && eax[2] == 2 && eax[3] == 3 && dropped == 1 &&
          missed == 0 && all >= 2 && why == FX32_RUN_EXCEPTION && e.eip == CODE + 0x105, d);

    printf("wow_test: %s\n", g_fail ? "FAILED" : "all checks passed");
    return g_fail;
}
