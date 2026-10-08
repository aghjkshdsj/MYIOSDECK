// SPDX-License-Identifier: GPL-3.0-or-later
// Held-out benchmark for the no-JIT interpreters (docs/FAST_INTERPRETER.md).
//
// FROZEN: this file is committed once and never edited, and no engine may reference,
// detect or special-case these programs (their code, loop shapes, constants, addresses or
// checksums). The standard kernels (bench_kernels.h) are what an engine could end up tuned
// for; these eleven realistic kernels check that a speed-up is general.
//
// One source, two builds:
//   x86-64 guest:  gcc -O2 -march=x86-64, guest_rt.h (engine/guest/build.sh, like bench_sse2)
//   native:        clang -O2 -ffp-contract=off -DHELDOUT_HOSTED (libc for the clock and output)
// Usage: heldout <kernel> [scale]. One warm-up pass at scale/8, then one timed pass:
//   RESULT kernel=<name> ns=<time> sum=<checksum>
// The checksum is the same on every host: unsigned integer arithmetic, explicit
// signed/unsigned chars, no libm (only IEEE + - * /, and no FMA contraction may be enabled).

typedef unsigned long long hu64;
typedef long long hi64;
typedef unsigned int hu32;
typedef int hi32;
typedef unsigned short hu16;
typedef unsigned char hu8;

#ifdef HELDOUT_HOSTED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#else
#include "guest_rt.h"
#endif

// ---- shared input: 64 KB of pseudo-English text ----
#define HK_TEXT (1u << 16)
static hu8 hk_text[HK_TEXT + 64];
static int hk_text_ready;

static hu64 hk_next(hu64 *s) {   // splitmix64
    hu64 z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static const char *const hk_words[64] = {
    "the", "of", "and", "to", "in", "a", "is", "that", "for", "it", "as", "was", "with", "be", "by", "on",
    "not", "he", "this", "are", "or", "his", "from", "at", "which", "but", "have", "an", "had", "they", "you", "were",
    "their", "one", "all", "we", "can", "her", "has", "there", "been", "if", "more", "when", "will", "would", "who", "so",
    "no", "player", "texture", "render", "vertex", "shader", "buffer", "frame", "physics", "collision", "update", "sound",
    "network", "input", "level", "score",
};

static void hk_text_init(void) {
    if (hk_text_ready) return;
    hu64 s = 0x1234567ull;
    hu32 n = 0, w = 0;
    while (n < HK_TEXT) {
        hu64 r = hk_next(&s);
        const char *word = hk_words[(r & 63) & ((r >> 6) & 63)];   // skewed: low indices are common
        for (hu32 i = 0; word[i] && n < HK_TEXT; i++) hk_text[n++] = (hu8)word[i];
        if (n < HK_TEXT) hk_text[n++] = (hu8)((++w % 11 == 0) ? '\n' : ((r >> 12) % 7 == 0 ? ',' : ' '));
    }
    hk_text_ready = 1;
}

// ---- 1. CRC-32 (IEEE, reflected, byte-wise table) ----
static hu32 hk_crc_tab[256];

static hu64 k_crc32(hu32 scale) {
    for (hu32 i = 0; i < 256; i++) {
        hu32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        hk_crc_tab[i] = c;
    }
    hk_text_init();
    hu64 sum = 0;
    for (hu32 p = 0; p < scale; p++) {
        hu32 crc = 0xFFFFFFFFu ^ p;
        for (hu32 i = 0; i < HK_TEXT; i++) crc = hk_crc_tab[(crc ^ hk_text[i]) & 0xff] ^ (crc >> 8);
        sum += crc ^ 0xFFFFFFFFu;
    }
    return sum;
}

// ---- 2. Quicksort (Hoare partition, insertion sort below 17 elements) ----
#define HK_SORT_N (1u << 15)
static hu32 hk_sort_a[HK_SORT_N];

static void hk_insertion(hu32 *a, hi64 n) {
    for (hi64 i = 1; i < n; i++) {
        hu32 v = a[i];
        hi64 j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static void hk_quicksort(hu32 *a, hi64 n) {
    while (n > 16) {
        hu32 pivot = a[(n - 1) / 2];
        hi64 i = -1, j = n;
        for (;;) {
            do i++; while (a[i] < pivot);
            do j--; while (a[j] > pivot);
            if (i >= j) break;
            hu32 t = a[i]; a[i] = a[j]; a[j] = t;
        }
        // [0, j] and [j + 1, n): recurse into the smaller part, loop on the larger
        hi64 left = j + 1, right = n - left;
        if (left < right) { hk_quicksort(a, left); a += left; n = right; }
        else { hk_quicksort(a + left, right); n = left; }
    }
    hk_insertion(a, n);
}

static hu64 k_sort(hu32 scale) {
    hu64 sum = 0;
    for (hu32 p = 0; p < scale; p++) {
        hu64 s = 0xC0FFEEull + p;
        for (hu32 i = 0; i < HK_SORT_N; i++) hk_sort_a[i] = (hu32)(hk_next(&s) >> 40);   // 24 bits: some duplicates
        hk_quicksort(hk_sort_a, HK_SORT_N);
        hu64 h = 0;
        for (hu32 i = 0; i < HK_SORT_N; i++) {
            h = h * 31 + hk_sort_a[i];
            if (i && hk_sort_a[i - 1] > hk_sort_a[i]) h ^= 0xBAD;   // never: sorted
        }
        sum += h;
    }
    return sum;
}

// ---- 3. Hash table: open addressing, linear probing; inserts, then hits and misses ----
#define HK_HT_BITS 16
#define HK_HT_MASK ((1u << HK_HT_BITS) - 1)
#define HK_HT_N 40000u
static hu64 hk_ht_key[1u << HK_HT_BITS];   // 0 = empty
static hu32 hk_ht_val[1u << HK_HT_BITS];
static hu64 hk_ht_src[HK_HT_N];

static hu32 hk_ht_slot(hu64 key) { return (hu32)((key * 0x9E3779B97F4A7C15ull) >> (64 - HK_HT_BITS)); }

static void hk_ht_put(hu64 key, hu32 val) {
    hu32 i = hk_ht_slot(key);
    while (hk_ht_key[i] && hk_ht_key[i] != key) i = (i + 1) & HK_HT_MASK;
    hk_ht_key[i] = key;
    hk_ht_val[i] = val;
}

static int hk_ht_get(hu64 key, hu32 *val) {
    hu32 i = hk_ht_slot(key);
    while (hk_ht_key[i]) {
        if (hk_ht_key[i] == key) { *val = hk_ht_val[i]; return 1; }
        i = (i + 1) & HK_HT_MASK;
    }
    return 0;
}

static hu64 k_hash(hu32 scale) {
    hu64 sum = 0;
    for (hu32 p = 0; p < scale; p++) {
        for (hu32 i = 0; i <= HK_HT_MASK; i++) hk_ht_key[i] = 0;
        hu64 s = 0xABCDEFull + p;
        for (hu32 i = 0; i < HK_HT_N; i++) {
            hu64 k = hk_next(&s) | 1;
            hk_ht_src[i] = k;
            hk_ht_put(k, i);
        }
        for (hu32 i = 0; i < 2 * HK_HT_N; i++) {
            hu64 k = (i & 1) ? hk_ht_src[(i * 7919u) % HK_HT_N] : (hk_next(&s) | 1);
            hu32 v;
            if (hk_ht_get(k, &v)) sum += v; else sum += 1;
        }
    }
    return sum;
}

// ---- 4. A small register bytecode VM (switch dispatch) running a loop with a call ----
enum { V_LI, V_ADD, V_ADDI, V_SUB, V_MUL, V_XOR, V_AND, V_SHR, V_SHL, V_LD, V_ST, V_JLT, V_JNZ, V_CALL, V_RET, V_HALT };
typedef struct { hu8 op, a, b, c; hi32 imm; } hk_ins;

// r1 = iterations and r3 = seed come from C. acc (r2) mixes a table the loop keeps updating.
static const hk_ins hk_prog[] = {
    { V_LI, 0, 0, 0, 0 },            //  0: r0 = i = 0
    { V_LI, 2, 0, 0, 0 },            //  1: r2 = acc = 0
    { V_LI, 4, 0, 0, 1103515245 },   //  2: r4 = multiplier
    { V_LI, 8, 0, 0, 255 },          //  3: r8 = mask
    { V_JNZ, 1, 0, 0, 6 },           //  4: n != 0: run the loop
    { V_HALT, 0, 0, 0, 0 },          //  5
    { V_MUL, 3, 3, 4, 0 },           //  6: loop: x = x * mult
    { V_ADDI, 3, 3, 0, 12345 },      //  7:   x += 12345
    { V_SHR, 5, 3, 0, 16 },          //  8:   t = x >> 16
    { V_AND, 6, 5, 8, 0 },           //  9:   j = t & 255
    { V_LD, 7, 6, 0, 0 },            // 10:   v = m[j]
    { V_ADD, 7, 7, 5, 0 },           // 11:   v += t
    { V_ST, 7, 6, 0, 0 },            // 12:   m[j] = v
    { V_SHR, 9, 5, 0, 8 },           // 13:   k = (t >> 8) & 255
    { V_AND, 9, 9, 8, 0 },           // 14
    { V_LD, 10, 9, 0, 0 },           // 15:   w = m[k]
    { V_CALL, 0, 0, 0, 20 },         // 16:   mix()
    { V_ADDI, 0, 0, 0, 1 },          // 17:   i++
    { V_JLT, 0, 1, 0, 6 },           // 18:   i < n: loop
    { V_HALT, 0, 0, 0, 0 },          // 19
    { V_SHL, 11, 2, 0, 5 },          // 20: mix: acc = ((acc << 5) ^ w) - i
    { V_XOR, 2, 11, 10, 0 },         // 21
    { V_SUB, 2, 2, 0, 0 },           // 22
    { V_RET, 0, 0, 0, 0 },           // 23
};

static hu64 hk_vm_run(const hk_ins *code, hu64 *r, hu64 *m) {
    hu32 pc = 0, sp = 0, stack[16];
    for (;;) {
        const hk_ins *in = &code[pc++];
        hu64 *d = &r[in->a & 15];
        hu64 b = r[in->b & 15], c = r[in->c & 15];
        switch (in->op) {
        case V_LI: *d = (hu64)(hi64)in->imm; break;
        case V_ADD: *d = b + c; break;
        case V_ADDI: *d = b + (hu64)(hi64)in->imm; break;
        case V_SUB: *d = b - c; break;
        case V_MUL: *d = b * c; break;
        case V_XOR: *d = b ^ c; break;
        case V_AND: *d = b & c; break;
        case V_SHR: *d = b >> (in->imm & 63); break;
        case V_SHL: *d = b << (in->imm & 63); break;
        case V_LD: *d = m[b & 255]; break;
        case V_ST: m[b & 255] = *d; break;
        case V_JLT: if (*d < b) pc = (hu32)in->imm; break;
        case V_JNZ: if (*d) pc = (hu32)in->imm; break;
        case V_CALL: stack[sp++ & 15] = pc; pc = (hu32)in->imm; break;
        case V_RET: pc = stack[--sp & 15]; break;
        case V_HALT: return r[2];
        default: return 0;
        }
    }
}

static hu64 k_vm(hu32 scale) {
    hu64 sum = 0, r[16], m[256];
    for (hu32 p = 0; p < scale; p++) {
        for (int i = 0; i < 16; i++) r[i] = 0;
        for (int i = 0; i < 256; i++) m[i] = (hu64)i * 0x0101010101ull;
        r[1] = 20000;
        r[3] = 12345 + p;
        sum = sum * 3 + hk_vm_run(hk_prog, r, m);
    }
    return sum;
}

// ---- 5. SHA-256 compression over 64 KB ----
static const hu32 hk_k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define HK_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void hk_sha256_block(hu32 *h, const hu8 *p) {
    hu32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (hu32)p[4 * i] << 24 | (hu32)p[4 * i + 1] << 16 | (hu32)p[4 * i + 2] << 8 | (hu32)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        hu32 s0 = HK_ROR(w[i - 15], 7) ^ HK_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        hu32 s1 = HK_ROR(w[i - 2], 17) ^ HK_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    hu32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        hu32 S1 = HK_ROR(e, 6) ^ HK_ROR(e, 11) ^ HK_ROR(e, 25);
        hu32 ch = (e & f) ^ (~e & g);
        hu32 t1 = hh + S1 + ch + hk_k256[i] + w[i];
        hu32 S0 = HK_ROR(a, 2) ^ HK_ROR(a, 13) ^ HK_ROR(a, 22);
        hu32 maj = (a & b) ^ (a & c) ^ (b & c);
        hu32 t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static hu64 k_sha256(hu32 scale) {
    hk_text_init();
    hu64 sum = 0;
    for (hu32 p = 0; p < scale; p++) {
        hu32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        h[0] ^= p;
        for (hu32 off = 0; off < HK_TEXT; off += 64) hk_sha256_block(h, hk_text + off);
        for (int i = 0; i < 8; i++) sum = sum * 1000003u + h[i];
    }
    return sum;
}

// ---- 6. LZ77 decompression (LZ4-like token format; the text is compressed once) ----
#define HK_LZ_CAP (2 * HK_TEXT + 64)
static hu8 hk_lz_comp[HK_LZ_CAP];
static hu8 hk_lz_out[HK_TEXT + 64];
static hu32 hk_lz_size;
static hu32 hk_lz_head[1u << 12];

static hu32 hk_lz_putlen(hu8 *o, hu32 op, hu32 len) {   // the part of a length beyond 15
    while (len >= 255) { o[op++] = 255; len -= 255; }
    o[op++] = (hu8)len;
    return op;
}

static hu32 hk_lz_compress(const hu8 *in, hu32 n, hu8 *o) {
    for (hu32 i = 0; i < (1u << 12); i++) hk_lz_head[i] = 0;
    hu32 ip = 0, anchor = 0, op = 0;
    while (ip + 4 <= n) {
        hu32 seq = (hu32)in[ip] | (hu32)in[ip + 1] << 8 | (hu32)in[ip + 2] << 16 | (hu32)in[ip + 3] << 24;
        hu32 h = (seq * 2654435761u) >> 20;
        hu32 cand = hk_lz_head[h];   // position + 1, 0 = none
        hk_lz_head[h] = ip + 1;
        if (cand && ip - (cand - 1) <= 65535) {
            hu32 m = cand - 1, len = 0;
            while (ip + len < n && in[m + len] == in[ip + len]) len++;
            if (len >= 4) {
                hu32 lit = ip - anchor, ml = len - 4, off = ip - m;
                o[op++] = (hu8)((lit < 15 ? lit : 15) << 4 | (ml < 15 ? ml : 15));
                if (lit >= 15) op = hk_lz_putlen(o, op, lit - 15);
                for (hu32 k = 0; k < lit; k++) o[op++] = in[anchor + k];
                o[op++] = (hu8)off;
                o[op++] = (hu8)(off >> 8);
                if (ml >= 15) op = hk_lz_putlen(o, op, ml - 15);
                ip += len;
                anchor = ip;
                continue;
            }
        }
        ip++;
    }
    hu32 lit = n - anchor;   // last sequence: literals only
    o[op++] = (hu8)((lit < 15 ? lit : 15) << 4);
    if (lit >= 15) op = hk_lz_putlen(o, op, lit - 15);
    for (hu32 k = 0; k < lit; k++) o[op++] = in[anchor + k];
    return op;
}

static hu32 hk_lz_decompress(const hu8 *in, hu32 n, hu8 *o) {
    hu32 ip = 0, op = 0;
    for (;;) {
        hu32 token = in[ip++];
        hu32 lit = token >> 4;
        if (lit == 15) { hu32 b; do { b = in[ip++]; lit += b; } while (b == 255); }
        for (hu32 k = 0; k < lit; k++) o[op++] = in[ip++];
        if (ip >= n) break;
        hu32 off = (hu32)in[ip] | (hu32)in[ip + 1] << 8;
        ip += 2;
        hu32 ml = token & 15;
        if (ml == 15) { hu32 b; do { b = in[ip++]; ml += b; } while (b == 255); }
        ml += 4;
        for (hu32 k = 0; k < ml; k++, op++) o[op] = o[op - off];   // may overlap: byte by byte
    }
    return op;
}

static hu64 k_lz77(hu32 scale) {
    hk_text_init();
    if (!hk_lz_size) hk_lz_size = hk_lz_compress(hk_text, HK_TEXT, hk_lz_comp);
    hu64 sum = hk_lz_size;
    for (hu32 p = 0; p < scale; p++) {
        hu32 n = hk_lz_decompress(hk_lz_comp, hk_lz_size, hk_lz_out);
        hu64 h = n;
        for (hu32 i = p & 15; i < n; i += 16) h = h * 131 + (hu32)(hk_lz_out[i] ^ hk_text[i]) * 1000 + hk_lz_out[i];
        sum += h;
    }
    return sum;
}

// ---- 7. N-body (double precision; square root by Newton's method, no libm) ----
typedef struct { double x, y, z, vx, vy, vz, m; } hk_body;
#define HK_PI 3.141592653589793
#define HK_SOLAR (4 * HK_PI * HK_PI)
#define HK_DPY 365.24

static double hk_sqrt(double x) {   // x > 0
    union { double d; hu64 u; } v;
    v.d = x;
    v.u = (v.u >> 1) + 0x1FF8000000000000ull;   // halve the exponent: within a few percent
    double y = v.d;
    for (int i = 0; i < 5; i++) y = 0.5 * (y + x / y);
    return y;
}

static hu64 k_nbody(hu32 scale) {
    hk_body b[5] = {
        { 0, 0, 0, 0, 0, 0, HK_SOLAR },
        { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
          1.66007664274403694e-03 * HK_DPY, 7.69901118419740425e-03 * HK_DPY, -6.90460016972063023e-05 * HK_DPY,
          9.54791938424326609e-04 * HK_SOLAR },
        { 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
          -2.76742510726862411e-03 * HK_DPY, 4.99852801234917238e-03 * HK_DPY, 2.30417297573763929e-05 * HK_DPY,
          2.85885980666130812e-04 * HK_SOLAR },
        { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
          2.96460137564761618e-03 * HK_DPY, 2.37847173959480950e-03 * HK_DPY, -2.96589568540237556e-05 * HK_DPY,
          4.36624404335156298e-05 * HK_SOLAR },
        { 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
          2.68067772490389322e-03 * HK_DPY, 1.62824170038242295e-03 * HK_DPY, -9.51592254519715870e-05 * HK_DPY,
          5.15138902046611451e-05 * HK_SOLAR },
    };
    double px = 0, py = 0, pz = 0;
    for (int i = 0; i < 5; i++) { px += b[i].vx * b[i].m; py += b[i].vy * b[i].m; pz += b[i].vz * b[i].m; }
    b[0].vx = -px / HK_SOLAR;
    b[0].vy = -py / HK_SOLAR;
    b[0].vz = -pz / HK_SOLAR;
    const double dt = 0.01;
    for (hu32 s = 0; s < scale * 1000u; s++) {
        for (int i = 0; i < 5; i++)
            for (int j = i + 1; j < 5; j++) {
                double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
                double d2 = dx * dx + dy * dy + dz * dz;
                double mag = dt / (d2 * hk_sqrt(d2));
                double mi = b[i].m * mag, mj = b[j].m * mag;
                b[i].vx -= dx * mj; b[i].vy -= dy * mj; b[i].vz -= dz * mj;
                b[j].vx += dx * mi; b[j].vy += dy * mi; b[j].vz += dz * mi;
            }
        for (int i = 0; i < 5; i++) { b[i].x += dt * b[i].vx; b[i].y += dt * b[i].vy; b[i].z += dt * b[i].vz; }
    }
    double e = 0;
    for (int i = 0; i < 5; i++) {
        e += 0.5 * b[i].m * (b[i].vx * b[i].vx + b[i].vy * b[i].vy + b[i].vz * b[i].vz);
        for (int j = i + 1; j < 5; j++) {
            double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
            e -= b[i].m * b[j].m / hk_sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    union { double d; hu64 u; } v;
    v.d = e;
    hu64 sum = v.u;
    for (int i = 0; i < 5; i++) { v.d = b[i].x; sum = sum * 31 + v.u; v.d = b[i].vz; sum = sum * 31 + v.u; }
    return sum;
}

// ---- 8. Huffman decoding (canonical code, 12-bit lookup table, MSB-first bit reader) ----
#define HK_HUF_BITS 12
static hu8 hk_huf_len[256];
static hu16 hk_huf_code[256];
static hu16 hk_huf_tab[1u << HK_HUF_BITS];   // symbol << 4 | length
static hu8 hk_huf_bits[2 * HK_TEXT + 64];
static hu8 hk_huf_out[HK_TEXT + 64];
static hu32 hk_huf_nbytes;
static int hk_huf_ready;

static void hk_huf_lengths(const hu32 *freq_in) {
    hu32 freq[256], w[512];
    hi32 parent[512], leaf[256];
    hu8 live[512];
    for (int s = 0; s < 256; s++) freq[s] = freq_in[s];
    for (;;) {
        hu32 nn = 0;
        for (int s = 0; s < 256; s++) {
            if (freq[s]) { leaf[s] = (hi32)nn; w[nn] = freq[s]; parent[nn] = -1; live[nn] = 1; nn++; }
            else leaf[s] = -1;
        }
        hu32 nleaves = nn;
        for (hu32 k = 1; k < nleaves; k++) {   // merge the two lightest live nodes (ties: lowest index)
            hi32 a = -1, b = -1;
            for (hu32 i = 0; i < nn; i++) {
                if (!live[i]) continue;
                if (a < 0 || w[i] < w[a]) { b = a; a = (hi32)i; }
                else if (b < 0 || w[i] < w[b]) b = (hi32)i;
            }
            live[a] = live[b] = 0;
            w[nn] = w[a] + w[b]; parent[nn] = -1; live[nn] = 1;
            parent[a] = parent[b] = (hi32)nn;
            nn++;
        }
        int maxlen = 0;
        for (int s = 0; s < 256; s++) {
            int len = 0;
            if (leaf[s] >= 0) {
                for (hi32 x = leaf[s]; parent[x] >= 0; x = parent[x]) len++;
                if (nleaves == 1) len = 1;
            }
            hk_huf_len[s] = (hu8)len;
            if (len > maxlen) maxlen = len;
        }
        if (maxlen <= HK_HUF_BITS) return;
        for (int s = 0; s < 256; s++) if (freq[s]) freq[s] = (freq[s] >> 1) | 1;   // flatten and retry
    }
}

static void hk_huf_build(void) {
    hu32 freq[256], count[16], next[16];
    for (int s = 0; s < 256; s++) freq[s] = 0;
    for (hu32 i = 0; i < HK_TEXT; i++) freq[hk_text[i]]++;
    hk_huf_lengths(freq);
    for (int l = 0; l < 16; l++) count[l] = 0;
    for (int s = 0; s < 256; s++) count[hk_huf_len[s]]++;
    count[0] = 0;
    hu32 code = 0;
    for (int l = 1; l < 16; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
    for (int s = 0; s < 256; s++) {
        hu32 l = hk_huf_len[s];
        if (!l) continue;
        hk_huf_code[s] = (hu16)next[l]++;
        hu32 base = (hu32)hk_huf_code[s] << (HK_HUF_BITS - l);
        for (hu32 k = 0; k < (1u << (HK_HUF_BITS - l)); k++) hk_huf_tab[base + k] = (hu16)((hu32)s << 4 | l);
    }
    hu64 acc = 0;
    hu32 nb = 0, op = 0;
    for (hu32 i = 0; i < HK_TEXT; i++) {
        hu32 s = hk_text[i];
        acc = acc << hk_huf_len[s] | hk_huf_code[s];
        nb += hk_huf_len[s];
        while (nb >= 8) { nb -= 8; hk_huf_bits[op++] = (hu8)(acc >> nb); }
    }
    if (nb) hk_huf_bits[op++] = (hu8)(acc << (8 - nb));
    hk_huf_nbytes = op;
    for (hu32 k = 0; k < 64; k++) hk_huf_bits[op + k] = 0;
}

static void hk_huf_decode(hu32 n, hu8 *out) {
    const hu8 *in = hk_huf_bits;
    hu32 ip = 0;
    hu64 buf = 0;
    int cnt = 0;
    for (hu32 i = 0; i < n; i++) {
        while (cnt <= 56) { buf |= (hu64)in[ip++] << (56 - cnt); cnt += 8; }
        hu32 e = hk_huf_tab[buf >> (64 - HK_HUF_BITS)];
        out[i] = (hu8)(e >> 4);
        buf <<= (e & 15);
        cnt -= (int)(e & 15);
    }
}

static hu64 k_huffman(hu32 scale) {
    hk_text_init();
    if (!hk_huf_ready) { hk_huf_build(); hk_huf_ready = 1; }
    hu64 sum = hk_huf_nbytes;
    for (hu32 p = 0; p < scale; p++) {
        hk_huf_decode(HK_TEXT, hk_huf_out);
        hu64 h = 0;
        for (hu32 i = p & 7; i < HK_TEXT; i += 8) h = h * 131 + hk_huf_out[i] + (hk_huf_out[i] != hk_text[i]) * 1000u;
        sum += h;
    }
    return sum;
}

// ---- 9. String search (Boyer-Moore-Horspool), several patterns ----
static const char *const hk_patterns[8] = {
    "the player", "shader", "frame and", "collision", "not the", "network input", "zzz", "update the level",
};

static hu32 hk_horspool(const hu8 *t, hu32 n, const hu8 *p, hu32 m) {
    hu32 shift[256], count = 0;
    for (int i = 0; i < 256; i++) shift[i] = m;
    for (hu32 i = 0; i + 1 < m; i++) shift[p[i]] = m - 1 - i;
    hu32 pos = 0;
    while (pos + m <= n) {
        hu8 last = t[pos + m - 1];
        if (last == p[m - 1]) {
            hu32 k = 0;
            while (k + 1 < m && t[pos + k] == p[k]) k++;
            if (k + 1 == m) count++;
        }
        pos += shift[last];
    }
    return count;
}

static hu64 k_search(hu32 scale) {
    hk_text_init();
    hu64 sum = 0;
    for (hu32 p = 0; p < scale; p++) {
        hu32 skip = p & 63;
        for (int i = 0; i < 8; i++) {
            const hu8 *pat = (const hu8 *)hk_patterns[i];
            hu32 m = 0;
            while (pat[m]) m++;
            sum = sum * 7 + hk_horspool(hk_text + skip, HK_TEXT - skip, pat, m) * (hu32)(i + 1);
        }
    }
    return sum;
}

// ---- 10. Binary search tree: inserts, lookups (pointer chasing), in-order walk ----
typedef struct hk_node { hu64 key, val; struct hk_node *l, *r; } hk_node;
#define HK_TREE_N (1u << 14)
static hk_node hk_pool[HK_TREE_N];
static hk_node *hk_tree_stack[HK_TREE_N];

static hu64 k_tree(hu32 scale) {
    hu64 sum = 0;
    for (hu32 p = 0; p < scale; p++) {
        hu64 s = 0x7EE5ull + p;
        hk_node *root = 0;
        hu32 used = 0;
        for (hu32 i = 0; i < HK_TREE_N; i++) {
            hu64 key = hk_next(&s) >> 20;
            hk_node **link = &root;
            while (*link) {
                if (key < (*link)->key) link = &(*link)->l;
                else if (key > (*link)->key) link = &(*link)->r;
                else break;
            }
            if (*link) { (*link)->val += i; continue; }
            hk_node *nd = &hk_pool[used++];
            nd->key = key; nd->val = i; nd->l = nd->r = 0;
            *link = nd;
        }
        hu64 s2 = 0x7EE5ull + p;   // replays the inserted keys: odd lookups hit, even ones (new keys) mostly miss
        for (hu32 i = 0; i < HK_TREE_N; i++) {
            hu64 key = (i & 1) ? (hk_next(&s2) >> 20) : (hk_next(&s) >> 20);
            hk_node *nd = root;
            while (nd && nd->key != key) nd = key < nd->key ? nd->l : nd->r;
            sum += nd ? nd->val : 3;
        }
        hu32 sp = 0;
        hu64 rank = 0;
        hk_node *nd = root;
        while (nd || sp) {
            while (nd) { hk_tree_stack[sp++] = nd; nd = nd->l; }
            nd = hk_tree_stack[--sp];
            sum += nd->key * (++rank & 7);
            nd = nd->r;
        }
    }
    return sum;
}

// ---- 11. Particle system (single precision): gravity, damping, bounces ----
#define HK_PART_N 4096
static float hk_px[HK_PART_N], hk_py[HK_PART_N], hk_pz[HK_PART_N];
static float hk_vx[HK_PART_N], hk_vy[HK_PART_N], hk_vz[HK_PART_N];

static hu64 k_particles(hu32 scale) {
    hu64 s = 0x5EEDull;
    for (int i = 0; i < HK_PART_N; i++) {
        hu64 r = hk_next(&s);
        hk_px[i] = (float)(r & 1023) * 0.05f - 25.0f;
        hk_py[i] = (float)((r >> 10) & 1023) * 0.02f;
        hk_pz[i] = (float)((r >> 20) & 1023) * 0.05f - 25.0f;
        hk_vx[i] = (float)((r >> 30) & 255) * 0.1f - 12.8f;
        hk_vy[i] = (float)((r >> 38) & 255) * 0.1f;
        hk_vz[i] = (float)((r >> 46) & 255) * 0.1f - 12.8f;
    }
    const float dt = 1.0f / 60.0f, gdt = -9.81f / 60.0f, damp = 0.995f;
    for (hu32 step = 0; step < scale * 16u; step++)
        for (int i = 0; i < HK_PART_N; i++) {
            float vx = hk_vx[i] * damp, vy = (hk_vy[i] + gdt) * damp, vz = hk_vz[i] * damp;
            float x = hk_px[i] + vx * dt, y = hk_py[i] + vy * dt, z = hk_pz[i] + vz * dt;
            if (y < 0.0f) { y = -y; vy = -vy * 0.8f; }
            if (x < -50.0f || x > 50.0f) vx = -vx;
            if (z < -50.0f || z > 50.0f) vz = -vz;
            hk_px[i] = x; hk_py[i] = y; hk_pz[i] = z;
            hk_vx[i] = vx; hk_vy[i] = vy; hk_vz[i] = vz;
        }
    hu64 sum = 0;
    for (int i = 0; i < HK_PART_N; i++) {
        union { float f; hu32 u; } a, b, c;
        a.f = hk_px[i]; b.f = hk_py[i]; c.f = hk_vz[i];
        sum = sum * 31 + a.u + ((hu64)b.u << 7) + ((hu64)c.u << 13);
    }
    return sum;
}

// ---- driver ----
typedef hu64 (*hk_fn)(hu32 scale);
typedef struct { const char *name; hk_fn fn; hu32 scale; } hk_kernel;
static const hk_kernel hk_kernels[] = {   // default scales: roughly 200 ms natively on a phone core
    { "crc32", k_crc32, 1500 },
    { "sort", k_sort, 100 },
    { "hash", k_hash, 200 },
    { "vm", k_vm, 300 },
    { "sha256", k_sha256, 800 },
    { "lz77", k_lz77, 2000 },
    { "nbody", k_nbody, 1000 },
    { "huffman", k_huffman, 1300 },
    { "search", k_search, 2000 },
    { "tree", k_tree, 60 },
    { "particles", k_particles, 2000 },
};
#define HK_COUNT (sizeof hk_kernels / sizeof hk_kernels[0])

#ifdef HELDOUT_HOSTED
static hu64 hk_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (hu64)ts.tv_sec * 1000000000ull + (hu64)ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: heldout <kernel> [scale]\n"); return 2; }
    for (unsigned i = 0; i < HK_COUNT; i++) {
        const hk_kernel *k = &hk_kernels[i];
        if (strcmp(argv[1], k->name)) continue;
        hu32 scale = argc > 2 ? (hu32)atoi(argv[2]) : k->scale;
        if (!scale) scale = k->scale;
        k->fn(scale / 8 ? scale / 8 : 1);   // warm-up
        hu64 t0 = hk_now_ns();
        hu64 sum = k->fn(scale);
        hu64 t1 = hk_now_ns();
        printf("RESULT kernel=%s ns=%llu sum=%llu\n", k->name, t1 - t0, sum);
        return 0;
    }
    fprintf(stderr, "unknown kernel\n");
    return 3;
}
#else
int guest_main(int argc, char **argv) {
    if (argc < 2) { g_puts("usage: heldout <kernel> [scale]\n"); return 2; }
    for (unsigned i = 0; i < HK_COUNT; i++) {
        const hk_kernel *k = &hk_kernels[i];
        if (!g_streq(argv[1], k->name)) continue;
        hu32 scale = argc > 2 ? g_atou(argv[2]) : k->scale;
        if (!scale) scale = k->scale;
        k->fn(scale / 8 ? scale / 8 : 1);   // warm-up
        unsigned long long t0 = g_now_ns();
        hu64 sum = k->fn(scale);
        unsigned long long t1 = g_now_ns();
        g_puts("RESULT kernel=");
        g_puts(k->name);
        g_puts(" ns=");
        g_putu(t1 - t0);
        g_puts(" sum=");
        g_putu(sum);
        g_puts("\n");
        return 0;
    }
    g_puts("unknown kernel\n");
    return 3;
}
#endif
