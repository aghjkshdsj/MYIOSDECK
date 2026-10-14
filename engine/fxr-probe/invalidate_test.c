// SPDX-License-Identifier: GPL-3.0-or-later
// Program after program in one Windows-mode block cache (CI, Linux): what "play another game
// without restarting" (docs/SESSION_REUSE.md) asks of the x64 CPU. Program A runs, a different
// program B is written at the same addresses, fxi_win_invalidate_all drops the blocks, and B must
// give B's result; then A again, and so on. Meanwhile a second thread (an ended game's thread that
// has not exited yet) loops inside one block the whole time, so every flush drops a block a
// thread is running: it must translate again and go on.
//   invalidate_test        built with FXR's objects (-Iengine/fxr) or FXI's (-Iengine/fxi)
// Only the public Windows-mode API (fxi.h): the same source tests both CPUs.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fxi.h"

// Guest memory at fixed addresses (read/write, never executable: the CPU interprets it).
#define CODE 0x10000000ull     // A or B: main at +0, f at +0x40
#define LOOP 0x10001000ull     // the other thread's loop
#define DATA 0x10002000ull     // its stop flag
#define NATIVE 0x20000000ull   // "native ARM64EC code": a run ends when control gets here

// Windows mode ends a run at native code, which PEB->EcCodeBitMap names (TEB+0x60 -> PEB,
// PEB+0x368 -> the bitmap, one bit per 4 KB page; fxi_win.c fxi_win_is_ec).
static uint64_t g_bitmap[(NATIVE >> 18) + 1];
static uint8_t g_peb[0x400], g_teb[0x100];

// main: xor eax,eax; mov ecx,N; L: call f; dec ecx; jnz L; mov rdx,NATIVE; jmp rdx
// f:    add rax,rcx (A) | sub rax,rcx (B); ret
static void write_program(int b) {
    static const uint8_t main_code[] = {
        0x31, 0xc0,                               // xor eax, eax
        0xb9, 0, 0, 0, 0,                         // mov ecx, N
        0xe8, 0x34, 0, 0, 0,                      // call f (CODE+0x40)
        0xff, 0xc9,                               // dec ecx
        0x75, 0xf7,                               // jnz L (CODE+7)
        0x48, 0xba, 0, 0, 0, 0x20, 0, 0, 0, 0,    // mov rdx, NATIVE
        0xff, 0xe2,                               // jmp rdx
    };
    uint8_t *p = (uint8_t *)(uintptr_t)CODE;
    memcpy(p, main_code, sizeof main_code);
    uint32_t n = b ? 500 : 1000;
    memcpy(p + 3, &n, 4);
    const uint8_t f[] = { 0x48, (uint8_t)(b ? 0x29 : 0x01), 0xc8, 0xc3 };   // add/sub rax, rcx; ret
    memcpy(p + 0x40, f, sizeof f);
}
static uint64_t expected(int b) { return b ? (uint64_t)-(int64_t)(500 * 501 / 2) : 1000 * 1001 / 2; }

// A CONTEXT with rip and the given registers (rsp on a stack of its own), the rest zero.
static void start(FxiCpu *c, uint64_t rip, uint64_t rbx, uint64_t *stack_top) {
    static _Thread_local uint8_t ctx[0x4d0] __attribute__((aligned(16)));
    memset(ctx, 0, sizeof ctx);
    uint32_t mxcsr = 0x1f80, eflags = 0x202;
    uint16_t fcw = 0x37f;
    uint64_t rsp = (uint64_t)(uintptr_t)stack_top - 8;   // as after a call
    memcpy(ctx + 0x34, &mxcsr, 4);
    memcpy(ctx + 0x44, &eflags, 4);
    memcpy(ctx + 0x78 + 8 * 3, &rbx, 8);
    memcpy(ctx + 0x78 + 8 * 4, &rsp, 8);
    memcpy(ctx + 0xf8, &rip, 8);
    memcpy(ctx + 0x100, &fcw, 2);
    memcpy(ctx + 0x100 + 24, &mxcsr, 4);
    fxi_win_load_context(c, ctx);
    fxi_win_set_teb(c, (uint64_t)(uintptr_t)g_teb);
}
static uint64_t rax_of(FxiCpu *c) {
    static _Thread_local uint8_t ctx[0x4d0] __attribute__((aligned(16)));
    fxi_win_save_context(c, ctx, 0);
    uint64_t v;
    memcpy(&v, ctx + 0x78, 8);
    return v;
}
// Runs from rip until native code; the native address (0 on an error, printed).
static uint64_t run(FxiCpu *c, const char *what) {
    uint64_t t = fxi_win_run(c);
    if (!t) printf("  %s: the CPU stopped with an error: %s\n", what, fxi_win_error(c));
    return t;
}

// ---- the thread that stays inside a block: L: add rax,1; mov rdx,[rbx]; test rdx,rdx; jz L ----
static volatile int g_loop_started;
static uint64_t g_loop_rax, g_loop_exit;
static void *looper(void *arg) {
    (void)arg;
    static uint64_t stack[256] __attribute__((aligned(16)));
    FxiCpu *c = fxi_win_cpu_new();
    start(c, LOOP, DATA, &stack[256]);
    g_loop_started = 1;
    g_loop_exit = run(c, "the looping thread");
    g_loop_rax = rax_of(c);
    return NULL;
}

int main(void) {
    void *m = mmap((void *)(uintptr_t)CODE, 0x3000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    void *nat = mmap((void *)(uintptr_t)NATIVE, 0x1000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(uintptr_t)CODE || nat != (void *)(uintptr_t)NATIVE) { perror("mmap at the fixed guest addresses"); return 2; }
    g_bitmap[NATIVE >> 18] |= 1ull << ((NATIVE >> 12) & 63);   // the native page (fxi_win_is_ec)
    uint64_t bm = (uint64_t)(uintptr_t)g_bitmap, peb = (uint64_t)(uintptr_t)g_peb;
    memcpy(g_peb + 0x368, &bm, 8);
    memcpy(g_teb + 0x60, &peb, 8);
    static const uint8_t loop_code[] = {
        0x48, 0x83, 0xc0, 0x01,                   // add rax, 1
        0x48, 0x8b, 0x13,                         // mov rdx, [rbx]
        0x48, 0x85, 0xd2,                         // test rdx, rdx
        0x74, 0xf4,                               // jz L
        0x48, 0xba, 0, 0, 0, 0x20, 0, 0, 0, 0,    // mov rdx, NATIVE
        0xff, 0xe2,                               // jmp rdx
    };
    memcpy((void *)(uintptr_t)LOOP, loop_code, sizeof loop_code);
    printf("%s\n", fxi_version());

    static uint64_t stack[256] __attribute__((aligned(16)));
    FxiCpu *c = fxi_win_cpu_new();
    int bad = 0;

    // 1. A, then B at the same addresses without a flush: the cache still runs A (the check is real).
    write_program(0);
    start(c, CODE, 0, &stack[256]);
    uint64_t t = run(c, "A"), r = rax_of(c);
    printf("A: rax %lld (want %lld)%s\n", (long long)r, (long long)expected(0), t == NATIVE && r == expected(0) ? "" : "  WRONG");
    bad |= t != NATIVE || r != expected(0);
    write_program(1);
    start(c, CODE, 0, &stack[256]);
    t = run(c, "B unflushed"), r = rax_of(c);
    printf("B without a flush: rax %lld (A's blocks still run: %s)\n", (long long)r, r == expected(0) ? "yes, as expected" : "NO, the test proves nothing");
    bad |= t != NATIVE || r != expected(0);

    // 2. Flush, B; then A and B in turn, each after a flush, while the other thread loops.
    pthread_t th;
    pthread_create(&th, NULL, looper, NULL);
    while (!g_loop_started) ;
    unsigned dropped_first = 0, rounds = 400, wrong = 0;
    for (unsigned i = 0; i < rounds; i++) {
        int b = !(i & 1);   // B first (it is in memory), then A, B, ...
        write_program(b);
        unsigned d = fxi_win_invalidate_all();
        if (!i) dropped_first = d;
        start(c, CODE, 0, &stack[256]);
        t = run(c, b ? "B" : "A");
        r = rax_of(c);
        if (t != NATIVE || r != expected(b)) {
            if (!wrong++) printf("round %u (%c): rax %lld, want %lld, stop at %#llx\n", i, b ? 'B' : 'A', (long long)r,
                                 (long long)expected(b), (unsigned long long)t);
        }
    }
    printf("flushes: %u rounds of A/B at the same addresses after fxi_win_invalidate_all, %u wrong; the first dropped %u blocks\n",
           rounds, wrong, dropped_first);
    bad |= wrong || !dropped_first;
    uint64_t one = 1;
    __atomic_store_n((uint64_t *)(uintptr_t)DATA, one, __ATOMIC_RELEASE);
    pthread_join(th, NULL);
    printf("the thread inside a dropped block: %s (%llu iterations)\n",
           g_loop_exit == NATIVE && g_loop_rax ? "went on and ended normally" : "FAILED", (unsigned long long)g_loop_rax);
    bad |= g_loop_exit != NATIVE || !g_loop_rax;
    printf("invalidate: %s\n", bad ? "FAILED" : "every program ran its own code");
    return bad;
}
