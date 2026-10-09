// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal x86-64 Linux runtime for MYIOSDECK's guest test programs:
// raw syscalls, _start, string output. No libc, so the binaries are tiny and
// only need the syscalls MYIOSDECK's FEX host layer implements.

#ifndef MYIOSDECK_GUEST_RT_H
#define MYIOSDECK_GUEST_RT_H

#if !defined(__x86_64__) && !defined(__i386__)
#error guest programs are built for x86-64 (and i386: FXI32, docs/NO_JIT_WOW64.md) only
#endif

typedef unsigned long g_ulong;

#ifdef __i386__
// i386 Linux: int 0x80, number in EAX, arguments in EBX ECX EDX (fxi32 runs these, stage 2).
static inline long g_syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
#define G_SYS_WRITE 4
#define G_SYS_EXIT_GROUP 252
#define G_SYS_CLOCK_GETTIME 265
#else
static inline long g_syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
#define G_SYS_WRITE 1
#define G_SYS_EXIT_GROUP 231
#define G_SYS_CLOCK_GETTIME 228
#endif

static inline long g_write(int fd, const void *buf, g_ulong len) { return g_syscall3(G_SYS_WRITE, fd, (long)buf, (long)len); }

__attribute__((noreturn)) static inline void g_exit(int code) {
    g_syscall3(G_SYS_EXIT_GROUP, code, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

struct g_timespec { long tv_sec, tv_nsec; };

static inline unsigned long long g_now_ns(void) {
    struct g_timespec ts;
    g_syscall3(G_SYS_CLOCK_GETTIME, 1 /* CLOCK_MONOTONIC */, (long)&ts, 0);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static g_ulong g_strlen(const char *s) {
    g_ulong n = 0;
    while (s[n]) n++;
    return n;
}

static int g_streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static void g_puts(const char *s) { g_write(1, s, g_strlen(s)); }

static void g_putu(unsigned long long v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    g_puts(buf + i);
}

static unsigned g_atou(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

/* gcc may still emit calls to these for struct copies even with
 * -fno-tree-loop-distribute-patterns. */
void *memcpy(void *d, const void *s, g_ulong n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, g_ulong n) {
    unsigned char *dd = (unsigned char *)d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}

int guest_main(int argc, char **argv);

#ifdef __i386__
/* i386 guests are plain static executables at their link address: fxi32 maps them inside a
 * 4 GB guest window, where guest addresses below 4 GB exist. */
__attribute__((used)) static void g_start_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    g_exit(guest_main(argc, argv));
}

__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "  xor %ebp, %ebp\n"
        "  mov %esp, %eax\n"
        "  and $-16, %esp\n"
        "  sub $12, %esp\n"
        "  push %eax\n"
        "  call g_start_c\n"
        "  hlt\n");
#else
/* The programs are static-PIE: iOS reserves the low 4 GB (__PAGEZERO), so the
 * host cannot place a guest at the classic 0x400000 and loads it wherever
 * mmap answers. Apply our own R_X86_64_RELATIVE relocations before anything
 * reads a pointer from data. Idempotent, so a loader may do it too. */
typedef struct { long d_tag; g_ulong d_val; } g_dyn;
typedef struct { g_ulong r_offset, r_info; long r_addend; } g_rela;
extern g_dyn _DYNAMIC[] __attribute__((visibility("hidden")));
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

static void g_self_relocate(void) {
    g_ulong base = (g_ulong)__ehdr_start;
    g_rela *rela = 0;
    g_ulong relasz = 0;
    for (g_dyn *d = _DYNAMIC; d->d_tag != 0; d++) {
        if (d->d_tag == 7 /* DT_RELA */) rela = (g_rela *)(base + d->d_val);
        if (d->d_tag == 8 /* DT_RELASZ */) relasz = d->d_val;
    }
    for (g_ulong i = 0; rela && i < relasz / sizeof(g_rela); i++)
        if ((rela[i].r_info & 0xffffffffu) == 8 /* R_X86_64_RELATIVE */)
            *(g_ulong *)(base + rela[i].r_offset) = base + (g_ulong)rela[i].r_addend;
}

__attribute__((used)) static void g_start_c(long *sp) {
    g_self_relocate();
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    g_exit(guest_main(argc, argv));
}

__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "  xor %rbp, %rbp\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call g_start_c\n"
        "  hlt\n");
#endif

#endif
