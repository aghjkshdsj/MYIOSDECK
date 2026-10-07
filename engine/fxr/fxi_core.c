// SPDX-License-Identifier: GPL-3.0-or-later
// FXI core: block cache, ELF loading, the guest's Linux syscalls, the run loop.

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fxi_internal.h"

#define FXI_VERSION "fxr 0.1 (FXI 0.3 + pinned registers) (integer + SSE2 core, atomics, x87)"

Block *fxi_stop;
static int g_echo_fd = -1;
static pthread_mutex_t g_run_lock = PTHREAD_MUTEX_INITIALIZER;

const char *fxi_version(void) { return FXI_VERSION; }
void fxi_set_echo_fd(int fd) { g_echo_fd = fd; }

void fxi_fail(FxiCpu *c, const char *fmt, ...) {
    c->stop = FXI_STOP_ERROR;
    if (c->err && !c->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, 256, fmt, ap);
        va_end(ap);
    }
}

void fxi_raise(FxiCpu *c, uint64_t rip, uint32_t code, uint32_t flags, uint32_t nparams, uint64_t i0, uint64_t i1) {
    c->rip = rip;
    if (!c->vm->windows) {
        fxi_fail(c, "CPU exception %#x at %#llx (int3/ud2/hlt/int n/divide error)", code, (unsigned long long)rip);
        return;
    }
    c->exc_code = code; c->exc_flags = flags; c->exc_nparams = nparams;
    c->exc_info[0] = i0; c->exc_info[1] = i1;
    c->stop = FXI_STOP_EXCEPTION;
}

// ---- block cache: open addressing on the guest rip ----
// Lookups are lock-free; translation and insertion take g_translate_lock (Windows guests
// run several threads on one cache). A grown table replaces the old one, which is never
// freed, so a reader that loaded the old pointer still reads valid memory.
static pthread_mutex_t g_translate_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t hash_rip(uint64_t rip) { return (rip * 0x9E3779B97F4A7C15ull) >> 20; }

static BlockTable *table_new(uint64_t slots) {
    BlockTable *t = calloc(1, sizeof *t + slots * sizeof(Block *));
    t->mask = slots - 1;
    return t;
}

static Block *table_find(BlockTable *t, uint64_t rip) {
    uint64_t h = hash_rip(rip) & t->mask;
    for (Block *b; (b = __atomic_load_n(&t->slot[h], __ATOMIC_ACQUIRE)); h = (h + 1) & t->mask)
        if (b->rip == rip) return b;
    return NULL;
}

static void table_put(BlockTable *t, Block *b) {
    uint64_t h = hash_rip(b->rip) & t->mask;
    while (t->slot[h]) h = (h + 1) & t->mask;
    __atomic_store_n(&t->slot[h], b, __ATOMIC_RELEASE);
    t->count++;
}

static void table_insert(struct Fxi *vm, Block *b) {   // under g_translate_lock
    BlockTable *t = vm->table;
    if ((t->count + 1) * 2 > t->mask + 1) {
        BlockTable *n = table_new((t->mask + 1) * 2);
        for (uint64_t i = 0; i <= t->mask; i++)
            if (t->slot[i]) table_put(n, t->slot[i]);
        __atomic_store_n(&vm->table, n, __ATOMIC_RELEASE);
        t = n;
    }
    table_put(t, b);
}

struct Fxi *fxi_vm_new(void) {
    pthread_mutex_lock(&g_translate_lock);
    if (!fxi_stop) {
        fxi_stop = calloc(1, sizeof(Block) + sizeof(Uop));
        fxi_stop->n = 1;
        fxi_stop->u[0].fn = fxi_named("stop");
        fxr_init_stop(fxi_stop);
    }
    pthread_mutex_unlock(&g_translate_lock);
    struct Fxi *vm = calloc(1, sizeof *vm);
    vm->table = table_new(4096);
    return vm;
}

Block *fxi_lookup(FxiCpu *c, uint64_t rip) {
    struct Fxi *vm = c->vm;
    Block *b = table_find(__atomic_load_n(&vm->table, __ATOMIC_ACQUIRE), rip);
    if (b) return b;
    if (c->stop) return fxi_stop;
    // Translation reads the guest code: a host fault from here on is an execute fault at rip.
    // Touch it before taking the translation lock, which a fault would leave held.
    c->cur = NULL;
    c->rip = rip;
    if (vm->windows) (void)*(volatile const uint8_t *)(uintptr_t)rip;
    if (!vm->windows &&
        (rip < (uint64_t)(uintptr_t)vm->image || rip >= (uint64_t)(uintptr_t)vm->image + vm->image_size)) {
        c->rip = rip;
        fxi_fail(c, "jump outside the guest image to %#llx", (unsigned long long)rip);
        return fxi_stop;
    }
    pthread_mutex_lock(&g_translate_lock);
    if (!(b = table_find(vm->table, rip))) {
        // Windows: a jump into native ARM64EC code (an import, a return into an exit thunk)
        // becomes a cached one-uop block that leaves the run for the transition glue.
        b = (vm->windows && fxi_win_is_ec(c, rip)) ? fxi_win_exit_block(rip) : fxi_translate(vm, rip);
        table_insert(vm, b);
    }
    pthread_mutex_unlock(&g_translate_lock);
    return b;
}

// ---- Linux syscalls the guest test programs use ----
static void capture(struct Fxi *vm, const void *buf, size_t len) {
    fxi_result *o = vm->out;
    size_t room = sizeof o->output - 1 - o->output_len;
    size_t n = len < room ? len : room;
    memcpy(o->output + o->output_len, buf, n);
    o->output_len += n;
    o->output[o->output_len] = 0;
    if (g_echo_fd >= 0) { ssize_t w = write(g_echo_fd, buf, len); (void)w; }
}

long fxi_syscall(FxiCpu *c) {
    struct Fxi *vm = c->vm;
    vm->syscalls++;
    uint64_t nr = c->r[R_AX], a0 = c->r[R_DI], a1 = c->r[R_SI], a2 = c->r[R_DX];
    switch (nr) {
    case 1:   // write
        if (a0 == 1 || a0 == 2) { capture(vm, (const void *)(uintptr_t)a1, (size_t)a2); return (long)a2; }
        return -EBADF;
    case 60: case 231:   // exit, exit_group
        c->exit_code = (long long)(int)a0;
        c->stop = FXI_STOP_EXIT;
        return 0;
    case 228: {   // clock_gettime
        struct timespec ts;
        clock_gettime((a0 == 0) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
        int64_t v[2] = { ts.tv_sec, ts.tv_nsec };
        memcpy((void *)(uintptr_t)a1, v, sizeof v);
        return 0;
    }
    case 12: return (long)vm->brk;   // brk: report, never grow
    case 158: {  // arch_prctl
        if (a0 == 0x1002) { c->r[R_FS] = a1; return 0; }   // ARCH_SET_FS
        if (a0 == 0x1001) { c->r[R_GS] = a1; return 0; }
        return -EINVAL;
    }
    default:
        fxi_fail(c, "unimplemented syscall %llu", (unsigned long long)nr);
        return -ENOSYS;
    }
}

// ---- static-PIE ELF loader ----
typedef struct { unsigned char ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;

static int load_elf(struct Fxi *vm, const uint8_t *elf, size_t len, uint64_t *entry, char *err) {
    if (len < sizeof(Ehdr) || memcmp(elf, "\x7f" "ELF", 4) || elf[4] != 2) { snprintf(err, 256, "not a 64-bit ELF"); return 0; }
    Ehdr eh; memcpy(&eh, elf, sizeof eh);
    if (eh.machine != 62) { snprintf(err, 256, "not x86-64 (machine %u)", eh.machine); return 0; }
    uint64_t lo = ~0ull, hi = 0;
    for (unsigned i = 0; i < eh.phnum; i++) {
        Phdr ph; memcpy(&ph, elf + eh.phoff + (size_t)i * eh.phentsize, sizeof ph);
        if (ph.type != 1) continue;
        if (ph.vaddr < lo) lo = ph.vaddr;
        if (ph.vaddr + ph.memsz > hi) hi = ph.vaddr + ph.memsz;
    }
    if (lo == ~0ull) { snprintf(err, 256, "no loadable segments"); return 0; }
    lo &= ~0xfffull;
    size_t span = (size_t)((hi - lo + 0x3fff) & ~0x3fffull);
    // Plain read/write memory: FXI never executes guest code natively.
    uint8_t *base = mmap(0, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { snprintf(err, 256, "mmap image failed"); return 0; }
    for (unsigned i = 0; i < eh.phnum; i++) {
        Phdr ph; memcpy(&ph, elf + eh.phoff + (size_t)i * eh.phentsize, sizeof ph);
        if (ph.type != 1) continue;
        if (ph.offset + ph.filesz > len) { snprintf(err, 256, "truncated ELF"); return 0; }
        memcpy(base + (ph.vaddr - lo), elf + ph.offset, ph.filesz);
    }
    vm->image = base;
    vm->image_size = span;
    vm->brk = (uint64_t)(uintptr_t)base + span;
    *entry = (uint64_t)(uintptr_t)base + (eh.entry - lo);
    return 1;
}

static uint64_t setup_stack(struct Fxi *vm, int argc, const char *const *argv) {
    vm->stack_size = 8u << 20;
    vm->stack = mmap(0, vm->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *top = vm->stack + vm->stack_size;
    uint64_t ptrs[64];
    int n = argc < 60 ? argc : 60;
    for (int i = n - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        top -= l; memcpy(top, argv[i], l);
        ptrs[i] = (uint64_t)(uintptr_t)top;
    }
    uint64_t *sp = (uint64_t *)((uintptr_t)top & ~(uintptr_t)15);
    size_t words = 1 + (size_t)n + 1 + 1 + 2;          // argc, argv..., NULL, envp NULL, auxv AT_NULL
    if (words & 1) sp--;                               // keep rsp 16-aligned at entry
    sp -= words;
    size_t k = 0;
    sp[k++] = (uint64_t)n;
    for (int i = 0; i < n; i++) sp[k++] = ptrs[i];
    sp[k++] = 0;   // argv end
    sp[k++] = 0;   // envp end
    sp[k++] = 0; sp[k++] = 0;   // AT_NULL
    return (uint64_t)(uintptr_t)sp;
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int fxi_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, fxi_result *out) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_run_lock);
    struct Fxi *vm = fxi_vm_new();
    vm->out = out;
    double t0 = now_s();
    uint64_t entry;
    if (!load_elf(vm, elf, len, &entry, out->error)) { pthread_mutex_unlock(&g_run_lock); return 0; }
    FxiCpu *c = &vm->cpu;
    c->vm = vm;
    c->err = out->error;
    c->r[R_SP] = setup_stack(vm, argc, argv);
    c->rip = entry;
    c->mxcsr = 0x1f80;
    fxi_x87_init(c);
    fxi_set_rflags(c, 0x202);

    Block *b = fxi_lookup(c, entry);
    if (!c->stop) fxr_enter(c, b);         // FXR: returns only when the guest stops
    if (!c->stop) fxi_fail(c, "dispatch chain returned unexpectedly");

    out->ok = c->stop == FXI_STOP_EXIT;
    out->exit_code = c->exit_code;
    out->seconds = now_s() - t0;
    out->syscalls = vm->syscalls;
    out->blocks = vm->blocks;
    // Blocks and the image are kept until the next run: a guest is short-lived
    // and freeing every block costs more than it saves here.
    munmap(vm->stack, vm->stack_size);
    pthread_mutex_unlock(&g_run_lock);
    return out->ok;
}
