// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA Windows-mode state test (CI, ARM64 Linux): runs engine/guest/winstate.elf in GPTA and checks
// the x64 state gpta_win_host_state rebuilds from the host's registers.
//   winstate_test <winstate.elf> loop          a timer thread stops the guest thread with a signal,
//                                              tens of thousands of times; every state reported
//                                              exact must match the loop's known values
//   winstate_test <winstate.elf> fault <site>  the guest faults (an unmapped address): the rip,
//                                              registers, flags and XMM0-7 must be the ones before
//                                              the faulting instruction
// Built with GPTA's objects (engine/gpta/build.sh) minus fxi_main.o.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "../gpta/fxi_internal.h"

// The kernel's FP/SIMD record in the signal frame (asm/sigcontext.h, without its clashes).
struct ctx_head { uint32_t magic, size; };
struct fpsimd_rec { struct ctx_head head; uint32_t fpsr, fpcr; __uint128_t vregs[32]; };
#define FPSIMD_MAGIC_ 0x46508001u

static void host_state(const ucontext_t *uc, gpta_host_state *h) {
    for (int i = 0; i < 31; i++) h->x[i] = uc->uc_mcontext.regs[i];
    h->sp = uc->uc_mcontext.sp;
    h->pc = uc->uc_mcontext.pc;
    memset(h->q, 0, sizeof h->q);
    const struct ctx_head *p = (const struct ctx_head *)uc->uc_mcontext.__reserved;
    while (p->magic && p->size) {
        if (p->magic == FPSIMD_MAGIC_) { memcpy(h->q, ((const struct fpsimd_rec *)p)->vregs, sizeof h->q); break; }
        p = (const struct ctx_head *)((const char *)p + p->size);
    }
}

static unsigned char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*len);
    if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { perror(path); exit(2); }
    fclose(f);
    return b;
}

// ---- stops: the loop (winstate.c) ----
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, XMM1 = 16 };
// Instruction k sets register reg to f(i) (winstate.c, loop): k, reg, multiplier, addend, xor-i.
static const struct { int k, reg; uint64_t mul, add; int xi; } k_set[] = {
    { 0, RBX, 2, 0, 0 }, { 1, RCX, 3, 0, 0 }, { 3, RDX, 3, 0, 0 }, { 4, RDX, 4, 0, 0 }, { 5, RSI, 4, 0, 0 },
    { 6, RSI, 8, 0, 0 }, { 7, XMM1, 8, 0, 0 }, { 8, XMM1, 16, 0, 0 }, { 9, R8, 16, 0, 0 }, { 10, R9, 16, 7, 0 },
    { 11, R10, 16, 7, 0 }, { 12, R10, 16, 7, 1 }, { 13, R11, 5, 0, 0 }, { 15, R12, 5, 0, 0 },
};
#define NSET (sizeof k_set / sizeof k_set[0])
static uint64_t expect(int reg, int k, uint64_t i) {   // reg's value before instruction k of iteration i
    int last = -1, prev = -1;
    for (unsigned j = 0; j < NSET; j++) {
        if (k_set[j].reg != reg) continue;
        if (k_set[j].k < k) last = (int)j;
        prev = (int)j;   // the last setter in the whole body
    }
    int j = last >= 0 ? last : prev;
    uint64_t n = last >= 0 ? i : i - 1;
    if (last < 0 && i == 0) return 0;
    return (n * k_set[j].mul + k_set[j].add) ^ (k_set[j].xi ? n : 0);
}

static volatile int g_done;
static pthread_t g_guest;
static unsigned long g_samples, g_exact, g_checked, g_bad, g_ks[32];
static char g_first_bad[512];
static uint64_t g_rsp0;

static void on_stop(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    gpta_host_state h;
    host_state(ctx, &h);
    g_samples++;
    uint64_t rip;
    FxiCpu *c = (FxiCpu *)(uintptr_t)h.x[10];   // GPTA keeps the CPU pointer in x10 (fxi_internal.h)
    if (!gpta_win_host_state(c, &h, 0, &rip)) return;
    g_exact++;
    uint64_t base = c->r[R15];
    const uint64_t *tab = (const uint64_t *)(uintptr_t)c->r[R14];
    if (!base || !tab || rip < base || rip > base + 256) return;   // before or after the loop
    int k = -1;
    for (int j = 0; j < 19; j++) if (tab[j] == rip - base) k = j;
    if (k < 0) return;
    g_checked++;
    g_ks[k]++;
    uint64_t i = k <= 16 ? c->r[RAX] : c->r[RAX] - 1;
    char why[160] = "";
    static const int regs[] = { RBX, RCX, RDX, RSI, R8, R9, R10, R11, R12, XMM1 };
    for (unsigned j = 0; j < sizeof regs / sizeof regs[0] && !why[0]; j++) {
        int r = regs[j];
        uint64_t got = r == XMM1 ? c->xmm[1].q[0] : c->r[r], want = expect(r, k, i);
        if (got != want) snprintf(why, sizeof why, "reg %d = %#llx, want %#llx", r, (unsigned long long)got, (unsigned long long)want);
    }
    uint64_t rsp = c->r[RSP] + (k == 15 ? 8 : 0);   // the push at 14 is still on the stack at 15
    if (!why[0]) { if (!g_rsp0) g_rsp0 = rsp; else if (rsp != g_rsp0) snprintf(why, sizeof why, "rsp %#llx", (unsigned long long)rsp); }
    if (why[0]) {
        if (!g_bad++) snprintf(g_first_bad, sizeof g_first_bad, "k=%d i=%llu: %s", k, (unsigned long long)i, why);
    }
}

// ---- native functions GPTA runs itself (gpta_pin.c, p_nat_*) ----
// Each runs as the exit block of its native target (kind_of names three fake ones), after the x64
// fast-forward thunk: rcx the argument, rsp at the return address. The fast paths return there
// (an exit block seeded in the thread's indirect-branch cache) with the result in rax; when Wine's
// code must run, the thread stops at the native function with nothing changed.
#define NAT_TLS 0x7ffe00001000ull
#define NAT_ENTER 0x7ffe00002000ull
#define NAT_LEAVE 0x7ffe00003000ull
#define NAT_RET 0x7ffe00004000ull
static int kind_of(uint64_t t) { return t == NAT_TLS ? 1 : t == NAT_ENTER ? 2 : t == NAT_LEAVE ? 3 : 0; }
static uint8_t g_teb[0x2000] __attribute__((aligned(16)));
static uint64_t g_stack[64], g_tls_ext[1024];
typedef struct { uint64_t debug; int32_t lock, rec; uint64_t owner, sem, spin; } crit;
static int g_nat_bad;
static void nat_check(int ok, const char *what) {
    printf("  %s: %s\n", what, ok ? "ok" : "WRONG");
    if (!ok) g_nat_bad++;
}
// Runs the native block b with rcx = arg; 1 when it returned to NAT_RET (rax, rsp popped), 0 when it
// stopped at its native target with the registers as they were; -1 otherwise.
static int nat_call(FxiCpu *c, Block *b, uint64_t arg, Block *ret) {
    for (int i = 0; i < 16; i++) c->r[i] = 0x1000 * (uint64_t)(i + 1) + 0x11;
    c->r[RCX] = arg;
    g_stack[32] = NAT_RET;
    c->r[RSP] = (uint64_t)(uintptr_t)&g_stack[32];
    c->gpta_ibtc[(NAT_RET ^ (NAT_RET >> 10)) & 1023u].rip = NAT_RET;   // gpta_pin.h IBTC()
    c->gpta_ibtc[(NAT_RET ^ (NAT_RET >> 10)) & 1023u].u = ret->u;
    c->stop = 0;
    gpta_enter(c, b);
    if (c->stop != FXI_STOP_EC) return -1;
    for (int i = 0; i < 16; i++)
        if (i != RAX && i != RSP && i != RCX && c->r[i] != 0x1000 * (uint64_t)(i + 1) + 0x11) return -1;
    if (c->rip == NAT_RET && c->r[RSP] == (uint64_t)(uintptr_t)&g_stack[33] && c->r[RCX] == arg) return 1;
    if (c->rip == b->rip && c->r[RSP] == (uint64_t)(uintptr_t)&g_stack[32] && c->r[RAX] == 0x1011 && c->r[RCX] == arg) return 0;
    return -1;
}
static int winnative(void) {
    gpta_win_set_native(kind_of);
    FxiCpu *c = gpta_win_cpu_new();
    Block *tls = gpta_win_exit_block(NAT_TLS), *enter = gpta_win_exit_block(NAT_ENTER), *leave = gpta_win_exit_block(NAT_LEAVE);
    Block *ret = gpta_win_exit_block(NAT_RET);
    gpta_enter(c, ret);   // the first run sets the indirect-branch cache up
    c->r[R_GS] = (uint64_t)(uintptr_t)g_teb;
    uint32_t tid = 0x1234, err;
    memcpy(g_teb + 0x48, &tid, 4);
    uint64_t v = 0xfeedface12345678ull, ext = (uint64_t)(uintptr_t)g_tls_ext, z = 0;
    memcpy(g_teb + 0x1480 + 8 * 5, &v, 8);
    g_tls_ext[3] = 0xabcdef;
    memcpy(g_teb + 0x1780, &ext, 8);
    printf("winnative: TlsGetValue, RtlEnterCriticalSection, RtlLeaveCriticalSection run by GPTA\n");
    err = 0x99; memcpy(g_teb + 0x68, &err, 4);
    int r = nat_call(c, tls, 5, ret);
    memcpy(&err, g_teb + 0x68, 4);
    nat_check(r == 1 && c->r[RAX] == v && err == 0, "TlsGetValue(5): the slot, LastError 0");
    r = nat_call(c, tls, 64 + 3, ret);
    nat_check(r == 1 && c->r[RAX] == 0xabcdef, "TlsGetValue(67): an expansion slot");
    r = nat_call(c, tls, 64 + 1024, ret);
    memcpy(&err, g_teb + 0x68, 4);
    nat_check(r == 1 && c->r[RAX] == 0 && err == 87, "TlsGetValue(1088): NULL, ERROR_INVALID_PARAMETER");
    memcpy(g_teb + 0x1780, &z, 8);
    err = 0x99; memcpy(g_teb + 0x68, &err, 4);
    r = nat_call(c, tls, 70, ret);
    memcpy(&err, g_teb + 0x68, 4);
    nat_check(r == 1 && c->r[RAX] == 0 && err == 0, "TlsGetValue(70) without expansion slots: NULL, LastError 0");
    crit cs = { 0x5eb, -1, 0, 0, 0, 0 };
    uint64_t a = (uint64_t)(uintptr_t)&cs;
    r = nat_call(c, enter, a, ret);
    nat_check(r == 1 && c->r[RAX] == 0 && cs.lock == 0 && cs.rec == 1 && cs.owner == tid, "enter a free section");
    r = nat_call(c, enter, a, ret);
    nat_check(r == 1 && cs.lock == 1 && cs.rec == 2 && cs.owner == tid, "enter it again (nested)");
    r = nat_call(c, leave, a, ret);
    nat_check(r == 1 && c->r[RAX] == 0 && cs.lock == 0 && cs.rec == 1 && cs.owner == tid, "leave the nested level");
    r = nat_call(c, leave, a, ret);
    nat_check(r == 1 && cs.lock == -1 && cs.rec == 0 && cs.owner == 0, "leave the last level (no waiter)");
    cs = (crit){ 0x5eb, 0, 1, 0x999, 0, 0 };
    r = nat_call(c, enter, a, ret);
    nat_check(r == 0 && cs.lock == 0 && cs.rec == 1 && cs.owner == 0x999, "enter one another thread holds: Wine's code, nothing changed");
    cs = (crit){ 0x5eb, 1, 1, tid, 0, 0 };
    r = nat_call(c, leave, a, ret);
    nat_check(r == 0 && cs.lock == 1 && cs.rec == 1 && cs.owner == tid, "leave with a waiter: Wine's code, nothing changed");
    cs = (crit){ 0x5eb, -1, 0, 0, 0, 0 };
    r = nat_call(c, leave, a, ret);
    nat_check(r == 0 && cs.lock == -1 && cs.rec == 0 && cs.owner == 0, "leave a section not acquired: Wine's code");
    uint64_t k[4];
    gpta_win_counters(c, k);
    nat_check(k[3] == 8, "8 calls run by GPTA (counter)");
    printf("winnative: %s\n", g_nat_bad ? "FAILED" : "all exact");
    return g_nat_bad != 0;
}

static void *stopper(void *arg) {
    (void)arg;
    struct timespec ts = { 0, 20000 };   // 20 us
    while (!g_done) { pthread_kill(g_guest, SIGUSR1); nanosleep(&ts, NULL); }
    return NULL;
}

// ---- faults: winstate.c's sites ----
static int g_site;
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    gpta_host_state h;
    host_state(ctx, &h);
    FxiCpu *c = (FxiCpu *)(uintptr_t)h.x[10];   // GPTA keeps the CPU pointer in x10 (fxi_internal.h)
    uint64_t rip = 0;
    int ok = gpta_win_host_state(c, &h, 1, &rip);
    char out[1536];
    int n = 0, bad = !ok;
    n += snprintf(out + n, sizeof out - (size_t)n, "fault site %d at %#llx (address %p): ", g_site, (unsigned long long)h.pc, si->si_addr);
    if (!ok) n += snprintf(out + n, sizeof out - (size_t)n, "NOT exact");
    else {
        static const uint64_t want[16] = { 0x1011, 0x2011, 0x3, 0x10, 0, 0x6011, 0x7011, 0x8011,
                                          0x9011, 0xa011, 0xb011, 0xc011, 0xd011, 0xe011, 0xf011, 0 };
        if (rip != c->r[R15]) { bad = 1; n += snprintf(out + n, sizeof out - (size_t)n, "rip %#llx, want %#llx; ", (unsigned long long)rip, (unsigned long long)c->r[R15]); }
        int stack = g_site == 3 || g_site == 4 || g_site == 7 || g_site == 8;
        for (int r = 0; r < 15; r++) {
            if (r == RSP && !stack) continue;
            uint64_t w = r == RSP ? 0x1008 : want[r];
            if (c->r[r] != w) { bad = 1; n += snprintf(out + n, sizeof out - (size_t)n, "r%d %#llx want %#llx; ", r, (unsigned long long)c->r[r], (unsigned long long)w); }
        }
        static const int xsrc[8] = { RAX, RCX, RDX, RSI, RDI, R8, R9, R10 };
        for (int x = 0; x < 8; x++)
            if (c->xmm[x].q[0] != want[xsrc[x]] || c->xmm[x].q[1]) {
                bad = 1; n += snprintf(out + n, sizeof out - (size_t)n, "xmm%d %#llx:%#llx; ", x, (unsigned long long)c->xmm[x].q[1], (unsigned long long)c->xmm[x].q[0]);
            }
        // The flags: exact unless the faulting instruction writes them itself (cmp/test/add with
        // memory: sites 5, 6, 9), where the earlier compare's flags are dead and never computed
        // (GPTA's and FXI's dead-flag elimination); re-running the instruction recomputes them.
        uint64_t fl = fxi_rflags(c) & 0x8d5;
        int flags_dead = g_site == 5 || g_site == 6 || g_site == 9;
        if (fl != 0x44 && !flags_dead) { bad = 1; n += snprintf(out + n, sizeof out - (size_t)n, "flags %#llx want 0x44; ", (unsigned long long)fl); }
        if (!bad) n += snprintf(out + n, sizeof out - (size_t)n, "exact (rip, GPRs, flags, XMM0-7)");
    }
    if (bad) {   // the host registers, to see where the guest state really is
        n += snprintf(out + n, sizeof out - (size_t)n, "\n  host:");
        for (int i = 0; i < 31 && n < (int)sizeof out - 24; i++) n += snprintf(out + n, sizeof out - (size_t)n, " x%d=%llx", i, (unsigned long long)h.x[i]);
    }
    out[n++] = '\n';
    (void)!write(1, out, (size_t)n);
    _exit(bad);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: winstate_test <winstate.elf> loop | fault <site>\n"); return 2; }
    long len;
    unsigned char *elf = read_file(argv[1], &len);
    gpta_win_index();
    static fxi_result r;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO;
    if (!strcmp(argv[2], "loop")) {
        sa.sa_sigaction = on_stop;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);
        g_guest = pthread_self();
        pthread_t t;
        pthread_create(&t, NULL, stopper, NULL);
        const char *gargv[] = { "winstate", "loop", "20000000" };
        int ok = fxi_run_elf(elf, (size_t)len, 3, gargv, &r);
        g_done = 1;
        pthread_join(t, NULL);
        printf("stops: %lu signals, %lu exact states, %lu inside the loop checked, %lu wrong%s%s\n", g_samples, g_exact,
               g_checked, g_bad, g_bad ? ": first " : "", g_first_bad);
        printf("  instructions seen:");
        for (int k = 0; k < 19; k++) printf(" %d:%lu", k, g_ks[k]);
        printf("\n  guest: ok=%d %.*s", ok, (int)r.output_len, r.output);
        return !ok || g_bad || g_checked < 1000;
    }
    if (!strcmp(argv[2], "winexit")) {
        // Windows mode: a jump into native ARM64EC code is a one-uop exit block built outside the
        // lowering (fxi_win.c); GPTA runs it through u->p, which must be set (build 117 jumped to
        // address 0 at the first call into a Windows DLL). It stops with the target as rip and
        // the registers spilled to the CPU structure for the transition glue.
        FxiCpu *c = fxi_win_cpu_new();
        Block *b = fxi_win_exit_block(0x7ffe12345678ull);
        c->r[RAX] = 0x1111; c->r[R15] = 0xf0f0;
        int ok = b->u[0].p != NULL;
        if (ok) {
            gpta_enter(c, b);
            ok = c->stop == FXI_STOP_EC && c->rip == 0x7ffe12345678ull && c->r[RAX] == 0x1111 && c->r[R15] == 0xf0f0;
        }
        printf("winexit: exit block %s\n", ok ? "stops at its native target, registers spilled" : "FAILED (no handler, or wrong stop)");
        return !ok;
    }
    if (!strcmp(argv[2], "winnative")) return winnative();
    if (!strcmp(argv[2], "fault") && argc > 3) {
        g_site = atoi(argv[3]);
        sa.sa_sigaction = on_fault;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        char site[16];
        snprintf(site, sizeof site, "%d", g_site);
        const char *gargv[] = { "winstate", "fault", site };
        fxi_run_elf(elf, (size_t)len, 3, gargv, &r);
        printf("fault site %d: the guest did not fault (%.*s)\n", g_site, (int)r.output_len, r.output);
        return 1;
    }
    return 2;
}
