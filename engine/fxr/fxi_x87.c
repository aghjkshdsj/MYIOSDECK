// SPDX-License-Identifier: GPL-3.0-or-later
// FXI x87 FPU, plus the 0F AE group (fxsave/fxrstor, ldmxcsr/stmxcsr, fences).
//
// Values are kept as double (the trade FEX's "reduced precision" preset makes): 80-bit loads
// and stores convert, and long double arithmetic gets a 53-bit mantissa instead of 64. That
// covers what compilers emit x87 for in x64 code (long double in the mingw CRT, FPU resets,
// old math code). Precision control is ignored; rounding control applies to integer stores
// and frndint. Exceptions are never raised (all masked, as Windows sets them).
//
// One handler interprets the opcode group (d8-df) and ModRM byte stored in u->aux at run
// time: x87 is rare in x64 code and does not get specialised handlers.

#include <math.h>
#include <stdio.h>

#include "fxi_internal.h"

#define ST(i) c->st[(c->fpu_top + (i)) & 7]
enum { C0 = 0x100, C1 = 0x200, C2 = 0x400, C3 = 0x4000, CC = C0 | C1 | C2 | C3 };

void fxi_x87_init(FxiCpu *c) { c->fpu_top = 0; c->fpu_tags = 0xff; c->fcw = 0x37f; c->fsw = 0; }

static void push(FxiCpu *c, double v) {
    c->fpu_top = (c->fpu_top - 1) & 7;
    c->st[c->fpu_top] = v;
    c->fpu_tags &= ~(1u << c->fpu_top);
}
static void pop(FxiCpu *c) { c->fpu_tags |= 1u << c->fpu_top; c->fpu_top = (c->fpu_top + 1) & 7; }
static int is_empty(FxiCpu *c, unsigned i) { return (c->fpu_tags >> ((c->fpu_top + i) & 7)) & 1; }
static void set_st(FxiCpu *c, unsigned i, double v) {
    unsigned p = (c->fpu_top + i) & 7;
    c->st[p] = v;
    c->fpu_tags &= ~(1u << p);
}
static uint16_t status_word(FxiCpu *c) { return (uint16_t)((c->fsw & ~0x3800u) | (c->fpu_top & 7) << 11); }
static void set_cc(FxiCpu *c, unsigned cc) { c->fsw = (uint16_t)((c->fsw & ~CC) | cc); }

// ---- 80-bit extended <-> double ----
double fxi_f80_load(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int e = se & 0x7fff;
    double v;
    if (e == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else if (!mant) v = 0.0;
    else v = ldexp((double)mant, e - 16383 - 63);
    return (se >> 15) ? -v : v;
}
void fxi_f80_store(uint8_t *p, double v) {
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);           // m in [0.5, 1)
        mant = (uint64_t)ldexp(m, 64);           // explicit integer bit set
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}

static double ld_f32(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static double ld_f64(uint64_t a) { double d; memcpy(&d, (const void *)(uintptr_t)a, 8); return d; }
static void st_f32(uint64_t a, double v) { float f = (float)v; memcpy((void *)(uintptr_t)a, &f, 4); }
static void st_f64(uint64_t a, double v) { memcpy((void *)(uintptr_t)a, &v, 8); }

// Rounding by the control word's RC field (bits 10-11). Round-to-nearest is done by hand so
// the host's rounding mode never matters.
static double round_rc(FxiCpu *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: {
        double r = floor(v + 0.5);
        if (r - v == 0.5 && fmod(r, 2.0) != 0) r -= 1.0;   // ties to even
        return r;
    }
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
// Integer conversion; out of range or NaN gives the "integer indefinite" (most negative value).
static uint64_t to_int(FxiCpu *c, double v, int bits, int truncate) {
    double r = truncate ? trunc(v) : round_rc(c, v), lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return 1ull << (bits - 1);
    return (uint64_t)(int64_t)r;
}

static unsigned cmp_cc(double a, double b) {
    if (isnan(a) || isnan(b)) return C3 | C2 | C0;
    return a > b ? 0 : a < b ? C0 : C3;
}
// fcomi/fucomi: ZF PF CF like the C3 C2 C0 of fcom; OF SF AF cleared.
static void cmp_eflags(FxiCpu *c, double a, double b) {
    uint64_t f = fxi_rflags(c) & ~0x8d5ull;
    if (isnan(a) || isnan(b)) f |= 0x45; else if (a < b) f |= 1; else if (a == b) f |= 0x40;
    fxi_set_rflags(c, f);
}

// ModRM reg field of the arithmetic groups: add mul com comp sub subr div divr.
static double arith(unsigned op, double a, double b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a * b;
    case 4: return a - b;
    case 5: return b - a;
    case 6: return a / b;
    default: return b / a;
    }
}
static void arith_st0(FxiCpu *c, unsigned op, double v) {
    if (op == 2 || op == 3) { set_cc(c, cmp_cc(ST(0), v)); if (op == 3) pop(c); }
    else set_st(c, 0, arith(op, ST(0), v));
}
// DC/DE register forms name sub/subr and div/divr the other way round (dest = ST(i)).
static unsigned swap_r(unsigned op) { return op >= 4 ? op ^ 1 : op; }

// Full tag word: 00 valid, 01 zero, 10 special, 11 empty (per physical register).
static uint16_t full_tags(FxiCpu *c) {
    uint16_t t = 0;
    for (unsigned p = 0; p < 8; p++) {
        unsigned tag = (c->fpu_tags >> p) & 1 ? 3 : c->st[p] == 0 ? 1 : !isfinite(c->st[p]) ? 2 : 0;
        t |= (uint16_t)(tag << (2 * p));
    }
    return t;
}
static void load_full_tags(FxiCpu *c, uint16_t t) {
    c->fpu_tags = 0;
    for (unsigned p = 0; p < 8; p++) if (((t >> (2 * p)) & 3) == 3) c->fpu_tags |= 1u << p;
}

// fnstenv/fldenv: the 28-byte protected-mode environment.
static void store_env(FxiCpu *c, uint64_t a) {
    uint8_t e[28] = { 0 };
    uint16_t sw = status_word(c), tw = full_tags(c);
    memcpy(e, &c->fcw, 2); memcpy(e + 4, &sw, 2); memcpy(e + 8, &tw, 2);
    memcpy((void *)(uintptr_t)a, e, 28);
}
static void load_env(FxiCpu *c, uint64_t a) {
    uint16_t cw = (uint16_t)ld16(a), sw = (uint16_t)ld16(a + 4), tw = (uint16_t)ld16(a + 8);
    c->fcw = cw; c->fsw = sw & ~0x3800; c->fpu_top = (sw >> 11) & 7;
    load_full_tags(c, tw);
}

// fxsave layout (also the CONTEXT's FltSave): fcw 0, fsw 2, abridged tags 4, mxcsr 24,
// ST(i) at 32 + 16 i, xmm at 160 + 16 i.
void fxi_x87_fxsave(FxiCpu *c, uint8_t *p, int with_xmm) {
    memset(p, 0, 160);
    uint16_t sw = status_word(c);
    uint32_t mask = 0xffff;
    memcpy(p, &c->fcw, 2); memcpy(p + 2, &sw, 2);
    p[4] = (uint8_t)~c->fpu_tags;
    memcpy(p + 24, &c->mxcsr, 4); memcpy(p + 28, &mask, 4);
    for (unsigned i = 0; i < 8; i++) fxi_f80_store(p + 32 + 16 * i, ST(i));
    if (with_xmm) memcpy(p + 160, c->xmm, 256);
}
void fxi_x87_fxrstor(FxiCpu *c, const uint8_t *p, int with_xmm) {
    uint16_t sw;
    memcpy(&c->fcw, p, 2); memcpy(&sw, p + 2, 2);
    c->fsw = sw & ~0x3800; c->fpu_top = (sw >> 11) & 7;
    c->fpu_tags = (uint8_t)~p[4];
    memcpy(&c->mxcsr, p + 24, 4);
    for (unsigned i = 0; i < 8; i++) c->st[(c->fpu_top + i) & 7] = fxi_f80_load(p + 32 + 16 * i);
    if (with_xmm) memcpy(c->xmm, p + 160, 256);
}

static int bad(FxiCpu *c, Uop *u, const char *what) {
    c->rip = u->imm;
    fxi_fail(c, "unimplemented x87 instruction at %#llx: %s (%02x %02x)", (unsigned long long)u->imm, what,
             (unsigned)(0xd8 + ((u->aux >> 8) & 7)), (unsigned)(u->aux & 0xff));
    return 0;
}

static int x87_mem(FxiCpu *c, Uop *u, unsigned grp, unsigned reg) {
    uint64_t a = fxi_ea(c, u);
    switch (grp) {
    case 0: arith_st0(c, reg, ld_f32(a)); return 1;
    case 2: arith_st0(c, reg, (double)(int32_t)ld32(a)); return 1;
    case 4: arith_st0(c, reg, ld_f64(a)); return 1;
    case 6: arith_st0(c, reg, (double)(int16_t)ld16(a)); return 1;
    case 1:
        switch (reg) {
        case 0: push(c, ld_f32(a)); return 1;
        case 2: st_f32(a, ST(0)); return 1;
        case 3: st_f32(a, ST(0)); pop(c); return 1;
        case 4: load_env(c, a); return 1;
        case 5: c->fcw = (uint16_t)ld16(a); return 1;
        case 6: store_env(c, a); c->fcw |= 0x3f; return 1;
        case 7: st16(a, c->fcw); return 1;
        }
        break;
    case 3:
        switch (reg) {
        case 0: push(c, (double)(int32_t)ld32(a)); return 1;
        case 1: st32(a, to_int(c, ST(0), 32, 1)); pop(c); return 1;
        case 2: st32(a, to_int(c, ST(0), 32, 0)); return 1;
        case 3: st32(a, to_int(c, ST(0), 32, 0)); pop(c); return 1;
        case 5: push(c, fxi_f80_load((const uint8_t *)(uintptr_t)a)); return 1;
        case 7: fxi_f80_store((uint8_t *)(uintptr_t)a, ST(0)); pop(c); return 1;
        }
        break;
    case 5:
        switch (reg) {
        case 0: push(c, ld_f64(a)); return 1;
        case 1: st64(a, to_int(c, ST(0), 64, 1)); pop(c); return 1;
        case 2: st_f64(a, ST(0)); return 1;
        case 3: st_f64(a, ST(0)); pop(c); return 1;
        case 4:   // frstor: environment + ST(0..7), 10 bytes each
            load_env(c, a);
            for (unsigned i = 0; i < 8; i++) c->st[(c->fpu_top + i) & 7] = fxi_f80_load((const uint8_t *)(uintptr_t)(a + 28 + 10 * i));
            return 1;
        case 6:   // fnsave, then fninit
            store_env(c, a);
            for (unsigned i = 0; i < 8; i++) fxi_f80_store((uint8_t *)(uintptr_t)(a + 28 + 10 * i), ST(i));
            fxi_x87_init(c);
            return 1;
        case 7: st16(a, status_word(c)); return 1;
        }
        break;
    case 7:
        switch (reg) {
        case 0: push(c, (double)(int16_t)ld16(a)); return 1;
        case 1: st16(a, to_int(c, ST(0), 16, 1)); pop(c); return 1;
        case 2: st16(a, to_int(c, ST(0), 16, 0)); return 1;
        case 3: st16(a, to_int(c, ST(0), 16, 0)); pop(c); return 1;
        case 5: push(c, (double)(int64_t)ld64(a)); return 1;
        case 7: st64(a, to_int(c, ST(0), 64, 0)); pop(c); return 1;
        }
        break;
    }
    return bad(c, u, "memory form");
}

// fxam classes in C3 C2 C0; C1 = sign.
static unsigned fxam(FxiCpu *c) {
    double v = ST(0);
    unsigned cc = signbit(v) ? C1 : 0;
    if (is_empty(c, 0)) return cc | C3 | C0;
    switch (fpclassify(v)) {
    case FP_NAN: return cc | C0;
    case FP_INFINITE: return cc | C2 | C0;
    case FP_ZERO: return cc | C3;
    case FP_SUBNORMAL: return cc | C3 | C2;
    default: return cc | C2;
    }
}

static int x87_reg(FxiCpu *c, Uop *u, unsigned grp, unsigned reg, unsigned i) {
    unsigned m = u->aux & 0xff;
    switch (grp) {
    case 0:
        if (reg == 2 || reg == 3) { set_cc(c, cmp_cc(ST(0), ST(i))); if (reg == 3) pop(c); }
        else set_st(c, 0, arith(reg, ST(0), ST(i)));
        return 1;
    case 1:
        switch (reg) {
        case 0: { double v = ST(i); push(c, v); return 1; }                                  // fld st(i)
        case 1: { double t = ST(0); set_st(c, 0, ST(i)); set_st(c, i, t); return 1; }        // fxch
        case 2: if (m == 0xd0) return 1; break;                                              // fnop
        case 3: set_st(c, i, ST(0)); pop(c); return 1;                                       // fstp1
        case 4:
            switch (m) {
            case 0xe0: set_st(c, 0, -ST(0)); return 1;
            case 0xe1: set_st(c, 0, fabs(ST(0))); return 1;
            case 0xe4: set_cc(c, cmp_cc(ST(0), 0.0)); return 1;
            case 0xe5: set_cc(c, fxam(c)); return 1;
            }
            break;
        case 5: {
            static const double k[7] = { 1.0, 3.321928094887362348, 1.442695040888963407, 3.141592653589793116,
                                         0.301029995663981195, 0.693147180559945309, 0.0 };
            if (i < 7) { push(c, k[i]); return 1; }
            break;
        }
        case 6: case 7: {
            double x = ST(0);
            switch (m) {
            case 0xf0: set_st(c, 0, exp2(x) - 1.0); return 1;                                  // f2xm1
            case 0xf1: set_st(c, 1, ST(1) * log2(x)); pop(c); return 1;                        // fyl2x
            case 0xf2: set_st(c, 0, tan(x)); push(c, 1.0); set_cc(c, 0); return 1;             // fptan
            case 0xf3: set_st(c, 1, atan2(ST(1), x)); pop(c); return 1;                        // fpatan
            case 0xf4: {                                                                       // fxtract
                if (x == 0) { set_st(c, 0, -INFINITY); push(c, x); return 1; }
                double e = logb(x);
                set_st(c, 0, e); push(c, ldexp(x, -(int)e));
                return 1;
            }
            case 0xf5: case 0xf8: {                                                            // fprem1, fprem
                double y = ST(1), q = m == 0xf8 ? trunc(x / y) : round_rc(c, x / y);
                double r = m == 0xf8 ? fmod(x, y) : remainder(x, y);
                uint64_t qi = isfinite(q) ? (uint64_t)fabs(q) : 0;
                set_st(c, 0, r);
                set_cc(c, ((qi & 1) ? C1 : 0) | ((qi & 2) ? C3 : 0) | ((qi & 4) ? C0 : 0));   // C2 = 0: done
                return 1;
            }
            case 0xf6: c->fpu_top = (c->fpu_top - 1) & 7; return 1;                            // fdecstp
            case 0xf7: c->fpu_top = (c->fpu_top + 1) & 7; return 1;                            // fincstp
            case 0xf9: set_st(c, 1, ST(1) * (log1p(x) / M_LN2)); pop(c); return 1;            // fyl2xp1
            case 0xfa: set_st(c, 0, sqrt(x)); return 1;
            case 0xfb: set_st(c, 0, sin(x)); push(c, cos(x)); set_cc(c, 0); return 1;          // fsincos
            case 0xfc: set_st(c, 0, round_rc(c, x)); return 1;                                 // frndint
            case 0xfd: set_st(c, 0, ldexp(x, (int)fmax(fmin(trunc(ST(1)), 1e6), -1e6))); return 1;   // fscale
            case 0xfe: set_st(c, 0, sin(x)); set_cc(c, 0); return 1;
            case 0xff: set_st(c, 0, cos(x)); set_cc(c, 0); return 1;
            }
            break;
        }
        }
        break;
    case 2: case 3:
        if (reg <= 3) {   // fcmovcc: b e be u (DA) and their negations (DB)
            static const uint8_t ccs[4] = { 2, 4, 6, 10 };
            if (fxi_cond(c, ccs[reg] ^ (grp == 3))) set_st(c, 0, ST(i));
            return 1;
        }
        if (grp == 2 && m == 0xe9) { set_cc(c, cmp_cc(ST(0), ST(1))); pop(c); pop(c); return 1; }   // fucompp
        if (grp == 3) {
            if (m == 0xe2 || m == 0xe4 || m == 0xe0 || m == 0xe1) { c->fsw &= CC; return 1; }  // fnclex (+ legacy no-ops)
            if (m == 0xe3) { fxi_x87_init(c); return 1; }                                      // fninit
            if (reg == 5 || reg == 6) { cmp_eflags(c, ST(0), ST(i)); return 1; }               // fucomi, fcomi
        }
        break;
    case 4:
        if (reg == 2 || reg == 3) { set_cc(c, cmp_cc(ST(0), ST(i))); if (reg == 3) pop(c); return 1; }
        set_st(c, i, arith(swap_r(reg), ST(i), ST(0)));
        return 1;
    case 5:
        switch (reg) {
        case 0: c->fpu_tags |= 1u << ((c->fpu_top + i) & 7); return 1;                       // ffree
        case 1: { double t = ST(0); set_st(c, 0, ST(i)); set_st(c, i, t); return 1; }        // fxch4
        case 2: set_st(c, i, ST(0)); return 1;                                               // fst st(i)
        case 3: set_st(c, i, ST(0)); pop(c); return 1;                                       // fstp st(i)
        case 4: case 5: set_cc(c, cmp_cc(ST(0), ST(i))); if (reg == 5) pop(c); return 1;     // fucom(p)
        }
        break;
    case 6:
        if (m == 0xd9) { set_cc(c, cmp_cc(ST(0), ST(1))); pop(c); pop(c); return 1; }        // fcompp
        if (reg == 2 || reg == 3) { set_cc(c, cmp_cc(ST(0), ST(i))); pop(c); return 1; }
        set_st(c, i, arith(swap_r(reg), ST(i), ST(0))); pop(c);                              // faddp, fsubp, ...
        return 1;
    case 7:
        if (m == 0xe0) { wr16(c, R_AX * 8, status_word(c)); return 1; }                      // fnstsw ax
        if (reg == 0) { c->fpu_tags |= 1u << ((c->fpu_top + i) & 7); pop(c); return 1; }     // ffreep
        if (reg == 5 || reg == 6) { cmp_eflags(c, ST(0), ST(i)); pop(c); return 1; }         // fucomip, fcomip
        if (reg == 1) { double t = ST(0); set_st(c, 0, ST(i)); set_st(c, i, t); return 1; }  // fxch7
        if (reg == 2 || reg == 3) { set_st(c, i, ST(0)); pop(c); return 1; }                 // fstp8/9
        break;
    }
    return bad(c, u, "register form");
}

// u->aux = (opcode - 0xd8) << 8 | ModRM, u->imm = instruction address (for errors).
static void op_x87(FxiCpu *c, Uop *u) {
    unsigned grp = (u->aux >> 8) & 7, m = u->aux & 0xff, reg = (m >> 3) & 7;
    int ok = (m >> 6) != 3 ? x87_mem(c, u, grp, reg) : x87_reg(c, u, grp, reg, m & 7);
    if (FXI_UNLIKELY(!ok)) return;
    FXI_NEXT(c, u);
}

// ---- 0F AE ----
static void op_fxsave(FxiCpu *c, Uop *u) { fxi_x87_fxsave(c, (uint8_t *)(uintptr_t)fxi_ea(c, u), 1); FXI_NEXT(c, u); }
static void op_fxrstor(FxiCpu *c, Uop *u) { fxi_x87_fxrstor(c, (const uint8_t *)(uintptr_t)fxi_ea(c, u), 1); FXI_NEXT(c, u); }
static void op_ldmxcsr(FxiCpu *c, Uop *u) { c->mxcsr = (uint32_t)ld32(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void op_stmxcsr(FxiCpu *c, Uop *u) { st32(fxi_ea(c, u), c->mxcsr); FXI_NEXT(c, u); }
static void op_fence(FxiCpu *c, Uop *u) { __atomic_thread_fence(__ATOMIC_SEQ_CST); FXI_NEXT(c, u); }

static const struct { const char *name; OpFn fn; } kX87[] = {
    { "x87", op_x87 }, { "fxsave", op_fxsave }, { "fxrstor", op_fxrstor },
    { "ldmxcsr", op_ldmxcsr }, { "stmxcsr", op_stmxcsr }, { "fence", op_fence },
};

OpFn fxi_x87_named(const char *name) {
    for (size_t i = 0; i < sizeof kX87 / sizeof kX87[0]; i++)
        if (!strcmp(kX87[i].name, name)) return kX87[i].fn;
    return 0;
}
