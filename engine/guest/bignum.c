// SPDX-License-Identifier: GPL-3.0-or-later
// Big-number arithmetic as TLS libraries run it: Montgomery multiplication with long mul/adc
// carry chains and a final subtraction whose sbb borrow crosses a dec + jne. Valve's Steam client
// (Madeira Dock) spends its HTTPS setup in exactly this code (OpenSSL's bn_mul_mont, generic
// path, which elliptic-curve and small moduli take), so FXI and FXR must compute it bit for bit.
// gmont below follows the instruction sequence of that routine (OpenSSL crypto/bn/asm/
// x86_64-mont.pl, its 1x loop) with System V arguments; ref_mont is the same product in plain C.
// Each size prints one hash of its products and how many differ from the C reference; the
// output must equal the native run's (engine/fxr/gates.sh, engine/fxi/ci-test.sh). Timings go
// to stderr only.

#include "guest_rt.h"

typedef unsigned long long u64;
typedef unsigned __int128 u128;

// gmont(rp, ap, bp, np, &n0, num): rp = ap * bp / 2^(64 num) mod np, for num >= 2, odd np,
// ap and bp below np, n0 = -np^-1 mod 2^64.
void gmont(u64 *rp, const u64 *ap, const u64 *bp, const u64 *np, const u64 *n0, u64 num);
__asm__(
    ".text\n.globl gmont\n.type gmont,@function\n"
    ".intel_syntax noprefix\n"
    "gmont:\n"
    "  mov r9d, r9d\n"
    "  mov rax, rsp\n"
    "  push rbx\n  push rbp\n  push r12\n  push r13\n  push r14\n  push r15\n"
    "  neg r9\n"
    "  lea r10, [rsp + r9*8 - 16]\n"
    "  neg r9\n"
    "  and r10, -1024\n"
    "  mov rsp, r10\n"
    "  mov [rsp + r9*8 + 8], rax\n"
    "  mov r12, rdx\n"
    "  mov r8, [r8]\n"
    "  mov rbx, [r12]\n"
    "  mov rax, [rsi]\n"
    "  xor r14, r14\n"
    "  xor r15, r15\n"
    "  mov rbp, r8\n"
    "  mul rbx\n"
    "  mov r10, rax\n"
    "  mov rax, [rcx]\n"
    "  imul rbp, r10\n"
    "  mov r11, rdx\n"
    "  mul rbp\n"
    "  add r10, rax\n"
    "  mov rax, [rsi + 8]\n"
    "  adc rdx, 0\n"
    "  mov r13, rdx\n"
    "  lea r15, [r15 + 1]\n"
    "  jmp 2f\n"
    "1:\n"
    "  add r13, rax\n"
    "  mov rax, [rsi + r15*8]\n"
    "  adc rdx, 0\n"
    "  add r13, r11\n"
    "  mov r11, r10\n"
    "  adc rdx, 0\n"
    "  mov [rsp + r15*8 - 16], r13\n"
    "  mov r13, rdx\n"
    "2:\n"
    "  mul rbx\n"
    "  add r11, rax\n"
    "  mov rax, [rcx + r15*8]\n"
    "  adc rdx, 0\n"
    "  lea r15, [r15 + 1]\n"
    "  mov r10, rdx\n"
    "  mul rbp\n"
    "  cmp r15, r9\n"
    "  jne 1b\n"
    "  add r13, rax\n"
    "  mov rax, [rsi]\n"
    "  adc rdx, 0\n"
    "  add r13, r11\n"
    "  adc rdx, 0\n"
    "  mov [rsp + r15*8 - 16], r13\n"
    "  mov r13, rdx\n"
    "  mov r11, r10\n"
    "  xor rdx, rdx\n"
    "  add r13, r11\n"
    "  adc rdx, 0\n"
    "  mov [rsp + r9*8 - 8], r13\n"
    "  mov [rsp + r9*8], rdx\n"
    "  lea r14, [r14 + 1]\n"
    "3:\n"
    "  mov rbx, [r12 + r14*8]\n"
    "  xor r15, r15\n"
    "  mov rbp, r8\n"
    "  mov r10, [rsp]\n"
    "  mul rbx\n"
    "  add r10, rax\n"
    "  mov rax, [rcx]\n"
    "  adc rdx, 0\n"
    "  imul rbp, r10\n"
    "  mov r11, rdx\n"
    "  mul rbp\n"
    "  add r10, rax\n"
    "  mov rax, [rsi + 8]\n"
    "  adc rdx, 0\n"
    "  mov r10, [rsp + 8]\n"
    "  mov r13, rdx\n"
    "  lea r15, [r15 + 1]\n"
    "  jmp 5f\n"
    "4:\n"
    "  add r13, rax\n"
    "  mov rax, [rsi + r15*8]\n"
    "  adc rdx, 0\n"
    "  add r13, r10\n"
    "  mov r10, [rsp + r15*8]\n"
    "  adc rdx, 0\n"
    "  mov [rsp + r15*8 - 16], r13\n"
    "  mov r13, rdx\n"
    "5:\n"
    "  mul rbx\n"
    "  add r11, rax\n"
    "  mov rax, [rcx + r15*8]\n"
    "  adc rdx, 0\n"
    "  add r10, r11\n"
    "  mov r11, rdx\n"
    "  adc r11, 0\n"
    "  lea r15, [r15 + 1]\n"
    "  mul rbp\n"
    "  cmp r15, r9\n"
    "  jne 4b\n"
    "  add r13, rax\n"
    "  mov rax, [rsi]\n"
    "  adc rdx, 0\n"
    "  add r13, r10\n"
    "  mov r10, [rsp + r15*8]\n"
    "  adc rdx, 0\n"
    "  mov [rsp + r15*8 - 16], r13\n"
    "  mov r13, rdx\n"
    "  xor rdx, rdx\n"
    "  add r13, r11\n"
    "  adc rdx, 0\n"
    "  add r13, r10\n"
    "  adc rdx, 0\n"
    "  mov [rsp + r9*8 - 8], r13\n"
    "  mov [rsp + r9*8], rdx\n"
    "  lea r14, [r14 + 1]\n"
    "  cmp r14, r9\n"
    "  jb 3b\n"
    // tp - np with the borrow carried across dec + jne, then pick tp or the difference.
    "  xor r14, r14\n"
    "  mov rax, [rsp]\n"
    "  mov r15, r9\n"
    "6:\n"
    "  sbb rax, [rcx + r14*8]\n"
    "  mov [rdi + r14*8], rax\n"
    "  mov rax, [rsp + r14*8 + 8]\n"
    "  lea r14, [r14 + 1]\n"
    "  dec r15\n"
    "  jne 6b\n"
    "  sbb rax, 0\n"
    "  mov rbx, -1\n"
    "  xor rbx, rax\n"
    "  xor r14, r14\n"
    "  mov r15, r9\n"
    "7:\n"
    "  mov rcx, [rdi + r14*8]\n"
    "  mov rdx, [rsp + r14*8]\n"
    "  and rcx, rbx\n"
    "  and rdx, rax\n"
    "  mov [rsp + r14*8], r9\n"
    "  or rdx, rcx\n"
    "  mov [rdi + r14*8], rdx\n"
    "  lea r14, [r14 + 1]\n"
    "  sub r15, 1\n"
    "  jne 7b\n"
    "  mov rsi, [rsp + r9*8 + 8]\n"
    "  mov eax, 1\n"
    "  mov r15, [rsi - 48]\n"
    "  mov r14, [rsi - 40]\n"
    "  mov r13, [rsi - 32]\n"
    "  mov r12, [rsi - 24]\n"
    "  mov rbp, [rsi - 16]\n"
    "  mov rbx, [rsi - 8]\n"
    "  lea rsp, [rsi]\n"
    "  ret\n"
    ".att_syntax prefix\n"
    ".size gmont, .-gmont\n");

#define MAXN 40

// The same product, word by word (CIOS), with a final conditional subtraction.
static void ref_mont(u64 *r, const u64 *a, const u64 *b, const u64 *n, u64 n0, int num) {
    u64 t[MAXN + 2];
    for (int i = 0; i < num + 2; i++) t[i] = 0;
    for (int i = 0; i < num; i++) {
        u128 c = 0;
        for (int j = 0; j < num; j++) {
            c += (u128)a[j] * b[i] + t[j];
            t[j] = (u64)c; c >>= 64;
        }
        c += t[num]; t[num] = (u64)c; t[num + 1] = (u64)(c >> 64);
        u64 m = t[0] * n0;
        c = (u128)m * n[0] + t[0];
        c >>= 64;
        for (int j = 1; j < num; j++) {
            c += (u128)m * n[j] + t[j];
            t[j - 1] = (u64)c; c >>= 64;
        }
        c += t[num]; t[num - 1] = (u64)c; c >>= 64;
        t[num] = t[num + 1] + (u64)c;
    }
    // t (num + 1 words) >= n: subtract.
    u64 d[MAXN];
    u64 borrow = 0;
    for (int j = 0; j < num; j++) {
        u128 s = (u128)t[j] - n[j] - borrow;
        d[j] = (u64)s; borrow = (u64)(s >> 64) & 1;
    }
    int ge = t[num] || !borrow;
    for (int j = 0; j < num; j++) r[j] = ge ? d[j] : t[j];
}

static u64 g_rng = 0x2545F4914F6CDD1Dull;
static u64 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static u64 mix(u64 h, u64 v) { h ^= v; h *= 1099511628211ull; return h ^ (h >> 29); }

// A random odd modulus of num words (top word nonzero), with a few all-ones and sparse ones,
// and operands below it.
static void make(u64 *n, u64 *a, u64 *b, int num, int k) {
    for (int j = 0; j < num; j++) n[j] = rnd();
    if (k % 7 == 1) for (int j = 0; j < num; j++) n[j] = ~0ull;          // 2^(64 num) - 1
    if (k % 7 == 2) { for (int j = 0; j < num; j++) n[j] = 0; n[num - 1] = 1ull << 63; }
    n[0] |= 1;
    if (!n[num - 1]) n[num - 1] = 1;
    for (int j = 0; j < num; j++) { a[j] = rnd(); b[j] = rnd(); }
    if (k % 5 == 3) for (int j = 0; j < num; j++) a[j] = n[j] - (j == 0);  // n - 1
    a[num - 1] = n[num - 1] > 1 ? a[num - 1] % n[num - 1] : 0;
    b[num - 1] = n[num - 1] > 1 ? b[num - 1] % n[num - 1] : 0;
}

static u64 neg_inv(u64 x) {
    u64 inv = x;                            // x * x = 1 mod 8 for odd x
    for (int i = 0; i < 6; i++) inv *= 2 - x * inv;
    return (u64)0 - inv;
}

static void puts_err(const char *s) { g_write(2, s, g_strlen(s)); }
static void putu_err(unsigned long long v) {
    char buf[24]; int i = 23; buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    puts_err(buf + i);
}

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    static const int sizes[] = {2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 32};
    u64 n[MAXN], a[MAXN], b[MAXN], r[MAXN], q[MAXN];
    int bad_total = 0;
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int num = sizes[s];
        u64 h = 1469598103934665603ull;
        int bad = 0;
        for (int k = 0; k < 200; k++) {
            make(n, a, b, num, k);
            u64 n0 = neg_inv(n[0]);
            gmont(r, a, b, n, &n0, (u64)num);
            ref_mont(q, a, b, n, n0, num);
            for (int j = 0; j < num; j++) { h = mix(h, r[j]); if (r[j] != q[j]) { bad++; break; } }
            // A short exponentiation chain: square the product 20 times, both ways.
            for (int e = 0; e < 20; e++) {
                gmont(r, r, r, n, &n0, (u64)num);
                ref_mont(q, q, q, n, n0, num);
            }
            for (int j = 0; j < num; j++) { h = mix(h, r[j]); if (r[j] != q[j]) { bad++; break; } }
        }
        g_puts("mont "); g_putu((unsigned long long)num); g_puts(" hash "); g_putu(h);
        g_puts(bad ? " DIFFERS-FROM-C " : " ok "); g_putu((unsigned long long)bad); g_puts("\n");
        bad_total += bad;
    }
    // Speed (stderr): 4-word (P-256 sized) and 9-word (P-521 sized) squarings.
    static const int timed[] = {4, 9};
    for (unsigned s = 0; s < 2; s++) {
        int num = timed[s];
        make(n, a, b, num, 0);
        u64 n0 = neg_inv(n[0]);
        unsigned long long t0 = g_now_ns();
        for (int e = 0; e < 20000; e++) gmont(a, a, a, n, &n0, (u64)num);
        unsigned long long t1 = g_now_ns();
        puts_err("[bignum] "); putu_err((unsigned long long)num); puts_err("-word squarings: ");
        putu_err((t1 - t0) / 20000); puts_err(" ns each\n");
    }
    g_puts("bignum sizes "); g_putu(sizeof sizes / sizeof sizes[0]);
    g_puts(bad_total ? " FAILED\n" : " all ok\n");
    return 0;
}
