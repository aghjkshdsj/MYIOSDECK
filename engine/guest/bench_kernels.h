// SPDX-License-Identifier: GPL-3.0-or-later
// CPU benchmark kernels shared by the native ARM64 build (inside the app) and
// the x86-64 guest build (run through FEX). Same source, same inputs, so the
// ratio of the two times is FEX's translation efficiency on this device.
//
// Freestanding: no libc, no headers beyond compiler builtins, so the guest can
// be linked with -nostdlib and run on MYIOSDECK's minimal syscall layer.

#ifndef MYIOSDECK_BENCH_KERNELS_H
#define MYIOSDECK_BENCH_KERNELS_H

typedef unsigned long long bk_u64;
typedef unsigned int bk_u32;

#define BK_MEM_BYTES (1u << 20)
#define BK_SIEVE_N 2000000u
#define BK_MAT_N 96

static unsigned char bk_mem_a[BK_MEM_BYTES] __attribute__((aligned(64)));
static unsigned char bk_mem_b[BK_MEM_BYTES] __attribute__((aligned(64)));
static unsigned char bk_sieve[BK_SIEVE_N + 1];
static float bk_ma[BK_MAT_N * BK_MAT_N], bk_mb[BK_MAT_N * BK_MAT_N], bk_mc[BK_MAT_N * BK_MAT_N];

/* Integer ALU: xorshift64* mixing, rotates, multiplies, dependent chains. */
static bk_u64 bk_integer(bk_u32 scale) {
    bk_u64 x = 0x9E3779B97F4A7C15ull, acc = 0;
    bk_u64 n = (bk_u64)scale * 2000000ull;
    for (bk_u64 i = 0; i < n; i++) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        bk_u64 m = x * 0x2545F4914F6CDD1Dull;
        acc += (m >> 7) ^ ((m << 13) | (m >> 51));
    }
    return acc;
}

/* Scalar double precision: Mandelbrot escape counts. */
static bk_u64 bk_float(bk_u32 scale) {
    bk_u64 total = 0;
    const int W = 160, H = 120;
    for (bk_u32 pass = 0; pass < scale; pass++) {
        for (int py = 0; py < H; py++) {
            for (int px = 0; px < W; px++) {
                double cx = -2.2 + 3.0 * px / W + pass * 1e-9;
                double cy = -1.2 + 2.4 * py / H;
                double zx = 0, zy = 0;
                int it = 0;
                while (it < 64 && zx * zx + zy * zy < 4.0) {
                    double t = zx * zx - zy * zy + cx;
                    zy = 2.0 * zx * zy + cy;
                    zx = t;
                    it++;
                }
                total += (bk_u64)it;
            }
        }
    }
    return total;
}

/* Memory bandwidth: 1 MB copy + checksum, word-sized (vectorised by the compiler:
 * SSE2 on the guest, NEON natively). */
static bk_u64 bk_memory(bk_u32 scale) {
    bk_u64 *a = (bk_u64 *)bk_mem_a, *b = (bk_u64 *)bk_mem_b;
    const bk_u32 words = BK_MEM_BYTES / 8;
    for (bk_u32 i = 0; i < words; i++) a[i] = (bk_u64)i * 0x100000001B3ull;
    bk_u64 sum = 0;
    for (bk_u32 r = 0; r < scale * 8; r++) {
        for (bk_u32 i = 0; i < words; i++) b[i] = a[i] + r;
        for (bk_u32 i = 0; i < words; i += 8) sum += b[i] ^ b[i + 3];
    }
    return sum;
}

/* Branchy + byte memory: sieve of Eratosthenes. */
static bk_u64 bk_branch(bk_u32 scale) {
    bk_u64 count = 0;
    for (bk_u32 r = 0; r < scale; r++) {
        for (bk_u32 i = 0; i <= BK_SIEVE_N; i++) bk_sieve[i] = 1;
        bk_sieve[0] = bk_sieve[1] = 0;
        for (bk_u32 i = 2; (bk_u64)i * i <= BK_SIEVE_N; i++)
            if (bk_sieve[i])
                for (bk_u32 j = i * i; j <= BK_SIEVE_N; j += i) bk_sieve[j] = 0;
        for (bk_u32 i = 0; i <= BK_SIEVE_N; i++) count += bk_sieve[i];
    }
    return count;
}

/* Single-precision SIMD: dense matrix multiply (what game math looks like). */
static bk_u64 bk_simd(bk_u32 scale) {
    const int N = BK_MAT_N;
    for (int i = 0; i < N * N; i++) {
        bk_ma[i] = (float)((i * 7) % 13) * 0.25f;
        bk_mb[i] = (float)((i * 3) % 11) * 0.5f;
    }
    float acc = 0;
    for (bk_u32 r = 0; r < scale; r++) {
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) bk_mc[i * N + j] = 0;
            for (int k = 0; k < N; k++) {
                float aik = bk_ma[i * N + k] + (float)r * 1e-6f;
                for (int j = 0; j < N; j++) bk_mc[i * N + j] += aik * bk_mb[k * N + j];
            }
        }
        acc += bk_mc[(r * 31) % (N * N)];
    }
    return (bk_u64)acc;
}

typedef bk_u64 (*bk_kernel_fn)(bk_u32 scale);

typedef struct {
    const char *name;
    const char *what;
    bk_kernel_fn fn;
    bk_u32 scale; /* tuned so each kernel runs ~0.3-1 s natively on an A17 Pro */
} bk_kernel;

static const bk_kernel bk_kernels[] = {
    { "integer", "64-bit ALU, multiplies, rotates", bk_integer, 60 },
    { "float", "scalar double precision (Mandelbrot)", bk_float, 40 },
    { "memory", "1 MB copy + checksum", bk_memory, 40 },
    { "branch", "branches + byte stores (sieve)", bk_branch, 12 },
    { "simd", "float32 matrix multiply", bk_simd, 60 },
};

#define BK_KERNEL_COUNT (sizeof(bk_kernels) / sizeof(bk_kernels[0]))

#endif
