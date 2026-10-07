// SPDX-License-Identifier: GPL-3.0-or-later
// x87 FPU and the 0F AE group: engine/fxi/ci-test.sh requires FXI's output to equal the native
// run's. FXI keeps x87 values as double, so every result printed here is exact in double
// precision (or rounded to an integer) and the output does not depend on 80-bit precision.
// Built with -mno-red-zone (pushfq inside inline asm).

#include "guest_rt.h"

typedef unsigned long long u64;
typedef long long i64;

static u64 g_hash = 1469598103934665603ull;
static void report(const char *name, u64 v) {
    g_puts(name); g_puts(": "); g_putu(v); g_puts("\n");
    g_hash ^= v; g_hash *= 1099511628211ull;
}
static u64 bits(double d) { u64 v; __builtin_memcpy(&v, &d, 8); return v; }

// The long double arithmetic gcc emits (fld/fadd/fmul/fdiv/fsqrt/fistp/...): values chosen
// so that every result is exact in a double.
static volatile long double lv[4] = { 1.5L, -2.25L, 1024.0L, 0.125L };
static volatile double dv[3] = { 3.0, 0.5, -7.0 };
static volatile int iv = -12345;

static void t_compiler(void) {
    long double a = lv[0], b = lv[1], c = lv[2], d = lv[3];
    report("ld add", bits((double)(a + b)));
    report("ld mul", bits((double)(a * c)));
    report("ld div", bits((double)(c / d)));
    report("ld sub", bits((double)(d - c)));
    report("ld from int", bits((double)((long double)iv * a)));
    report("ld to i64", (u64)(i64)(b * c));
    report("ld cmp", (u64)((a > b) | (b < d) << 1 | (c == 1024.0L) << 2));
    report("ld from double", bits((double)((long double)dv[0] / (long double)dv[1])));
    long double m80 = (long double)dv[2] * 0.5L;   // stored and reloaded as 80-bit
    volatile long double keep = m80;
    report("ld 80-bit round trip", bits((double)keep));
}

static void t_asm(void) {
    unsigned short cw, sw;
    u64 f;
    __asm__ volatile("fninit\n\tfnstcw %0" : "=m"(cw));
    report("fninit cw", cw);

    // fistp with each rounding mode (RC = bits 10-11) on 2.5 and -2.5
    static const double vals[2] = { 2.5, -2.5 };
    for (int rc = 0; rc < 4; rc++) {
        unsigned short ncw = (unsigned short)((cw & ~0xc00) | rc << 10);
        for (int k = 0; k < 2; k++) {
            i64 out;
            __asm__ volatile("fldcw %[n]\n\tfldl %[v]\n\tfistpq %[o]\n\tfldcw %[c]"
                             : [o] "=m"(out) : [n] "m"(ncw), [v] "m"(vals[k]), [c] "m"(cw));
            report("fistp rc", (u64)out);
        }
    }
    // integer indefinite on overflow
    { static const double big = 1e30; int out;
      __asm__ volatile("fldl %[v]\n\tfistpl %[o]" : [o] "=m"(out) : [v] "m"(big)); report("fistp overflow", (unsigned)out); }

    // fcomi / fucomip flags, fcom + fnstsw
    { static const double x = 1.0, y = 2.0; u64 g;
      __asm__ volatile("fldl %[y]\n\tfldl %[x]\n\tfcomi %%st(1), %%st\n\tpushfq\n\tpopq %[f]\n\t"
                       "fucomip %%st(1), %%st\n\tpushfq\n\tpopq %[g]\n\tfstp %%st(0)"
                       : [f] "=&r"(f), [g] "=&r"(g) : [x] "m"(x), [y] "m"(y) : "cc");
      report("fcomi flags", f & 0x8d5); report("fucomip flags", g & 0x8d5); }
    { static const double x = 3.0, y = 3.0;
      __asm__ volatile("fldl %[y]\n\tfldl %[x]\n\tfcompp\n\tfnstsw %%ax" : "=a"(sw) : [x] "m"(x), [y] "m"(y));
      report("fcompp sw cc", sw & 0x4700); }

    // fxam on a few classes, fnstsw ax (C3 C2 C1 C0)
    { static const double vs[4] = { 1.0, -0.0, 1.0 / 0.0, 0.0 / 0.0 };
      for (int k = 0; k < 4; k++) {
          __asm__ volatile("fldl %[v]\n\tfxam\n\tfnstsw %%ax\n\tfstp %%st(0)" : "=a"(sw) : [v] "m"(vs[k]));
          report("fxam", sw & 0x4700);
      }
      __asm__ volatile("fninit\n\tfxam\n\tfnstsw %%ax" : "=a"(sw));
      report("fxam empty", sw & 0x4700); }

    // stack: constants, fxch, faddp/fsubrp/fdivp, fchs/fabs, fsqrt, fscale, frndint, fprem
    { double r[8];
      __asm__ volatile(
          "fninit\n\t"
          // (AT&T's fsubp/fdivp spellings are reversed vs Intel: whatever bytes the assembler
          //  picks, native and FXI run the same ones)
          "fld1\n\tfldz\n\tfaddp\n\tfstpl 0(%[r])\n\t"
          "fld1\n\tfld1\n\tfaddp\n\tfld1\n\tfxch\n\tfsubrp\n\tfstpl 8(%[r])\n\t"
          "fld1\n\tfld1\n\tfaddp\n\tfld1\n\tfdivp\n\tfstpl 16(%[r])\n\t"
          "fld1\n\tfchs\n\tfabs\n\tfchs\n\tfstpl 24(%[r])\n\t"
          "fld1\n\tfld1\n\tfaddp\n\tfld1\n\tfld1\n\tfaddp\n\tfaddp\n\tfsqrt\n\tfstpl 32(%[r])\n\t"   // sqrt(4)
          "fld1\n\tfld1\n\tfaddp\n\tfld1\n\tfscale\n\tfstpl 40(%[r])\n\tfstp %%st(0)\n\t"           // 1 * 2^2
          "fldpi\n\tfrndint\n\tfstpl 48(%[r])\n\t"                                                   // 3
          "fld1\n\tfld1\n\tfaddp\n\tfld1\n\tfld1\n\tfaddp\n\tfld1\n\tfaddp\n\tfprem\n\tfstpl 56(%[r])\n\tfstp %%st(0)\n\t"   // fmod(3, 2)
          : : [r] "r"(r) : "memory");
      for (int k = 0; k < 8; k++) report("stack op", bits(r[k])); }

    // 80-bit store/load of doubles (exact round trip), fld/fstp tword
    { static const double vs[3] = { 3.141592653589793, -1e-300, 6.5e300 };
      for (int k = 0; k < 3; k++) {
          unsigned char t[10]; double back;
          __asm__ volatile("fldl %[v]\n\tfstpt %[t]\n\tfldt %[t]\n\tfstpl %[b]" : [t] "=m"(t), [b] "=m"(back) : [v] "m"(vs[k]));
          u64 lo; __builtin_memcpy(&lo, t, 8);
          report("tword mant", lo); report("tword se", (u64)t[8] | (u64)t[9] << 8); report("tword back", bits(back));
      } }

    // fcmov
    { static const double x = 5.0, y = 9.0; double r1, r2;
      __asm__ volatile("fldl %[y]\n\tfldl %[x]\n\tstc\n\tfcmovb %%st(1), %%st\n\tfstpl %[r1]\n\tfstp %%st(0)\n\t"
                       "fldl %[y]\n\tfldl %[x]\n\tstc\n\tfcmovnb %%st(1), %%st\n\tfstpl %[r2]\n\tfstp %%st(0)"
                       : [r1] "=m"(r1), [r2] "=m"(r2) : [x] "m"(x), [y] "m"(y) : "cc");
      report("fcmovb", bits(r1)); report("fcmovnb", bits(r2)); }

    // integer loads/stores: fild/fist 16/32/64
    { short s = -300; int i = 70000; i64 q = -5000000000ll; short so; int io; i64 qo;
      __asm__ volatile("filds %[s]\n\tfildl %[i]\n\tfaddp\n\tfildq %[q]\n\tfaddp\n\tfistpq %[qo]\n\t"
                       "filds %[s]\n\tfistps %[so]\n\tfildl %[i]\n\tfistpl %[io]"
                       : [qo] "=m"(qo), [so] "=m"(so), [io] "=m"(io) : [s] "m"(s), [i] "m"(i), [q] "m"(q));
      report("fild sum", (u64)qo); report("fist16", (unsigned short)so); report("fist32", (unsigned)io); }

    // 0F AE: stmxcsr/ldmxcsr, fxsave/fxrstor (control word, ST0), mfence
    { unsigned m, m2 = 0x1f80 | 0x6000;   // RC = toward zero
      __asm__ volatile("stmxcsr %0" : "=m"(m));
      report("stmxcsr", m);
      __asm__ volatile("ldmxcsr %1\n\tstmxcsr %0\n\tldmxcsr %2" : "=m"(m2) : "m"(m2), "m"(m));
      report("ldmxcsr", m2);
      static unsigned char area[512] __attribute__((aligned(16)));
      static const double v = 42.0; double back;
      __asm__ volatile("fninit\n\tfldl %[v]\n\tfxsave %[a]\n\tfninit\n\tfxrstor %[a]\n\tfstpl %[b]\n\tmfence\n\tlfence\n\tsfence"
                       : [a] "+m"(area), [b] "=m"(back) : [v] "m"(v) : "memory");
      report("fxsave fcw", (u64)area[0] | (u64)area[1] << 8);
      report("fxsave tags", area[4]);
      report("fxrstor st0", bits(back)); }
}

int guest_main(int argc, char **argv) {
    (void)argc; (void)argv;
    t_compiler();
    t_asm();
    g_puts("x87 hash ");
    g_putu(g_hash);
    g_puts("\n");
    return 0;
}
