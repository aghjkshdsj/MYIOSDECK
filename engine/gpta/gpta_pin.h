// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA's pinned handlers, shared definitions (gpta_pin.c, gpta_pin_alu.c, gpta_pin_br.c, gpta_pin_sse.c):
// the handler signature's macros, lazy-flag conditions, chaining, XMM operand access, and the
// handler tables the lowering (gpta_pin.c) picks from. See gpta_pin.c for the design.

#ifndef GPTA_PIN_H
#define GPTA_PIN_H

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "fxi_internal.h"

// Handler replicas: gpta_pin_*_r1.c compile a hot handler file a second time with GPTA_R1, which
// gives its tables (and the SSE op lists) an _r1 suffix: the same handlers at other addresses,
// for the lowering to give a handler used at two places different copies (gpta_pin.c, replicas).
#ifdef GPTA_R1
#define GPTA_RSUF _r1
#define gpta_xops gpta_xops_r1
#define gpta_n_xops gpta_n_xops_r1
#define gpta_xshift gpta_xshift_r1
#define gpta_n_xshift gpta_n_xshift_r1
#define gpta_xshuffle_index gpta_xshuffle_index_r1
#else
#define GPTA_RSUF
#endif
#define GPTA_T3(N, S) gpta_##N##S
#define GPTA_T2(N, S) GPTA_T3(N, S)
#define GPTA_T(N) GPTA_T2(N, GPTA_RSUF)

// The pinned handlers live in their own code section, so a host pc (a fault, a stopped thread)
// can be told apart from the rest of the program (fxi_win_host_state, gpta_pin.c).
#if defined(__APPLE__)
#define GPTA_SECTION __attribute__((section("__TEXT,__gpta_h,regular,pure_instructions")))
#elif defined(__ELF__)
#define GPTA_SECTION __attribute__((section("gpta_h")))
#else
#define GPTA_SECTION
#endif
#define PH static GPTA_SECTION GPTA_CC void
// Dispatch: the handler pointer is the uop's first field, so stepping to the next uop is one
// load with writeback and an indirect branch.
#ifdef GPTA_PROFILE   // diagnostic build: count every dispatch (gpta_profile_dump)
#define PGO(nu) do { Uop *n_ = (nu); n_->prof++; __attribute__((musttail)) return n_->p GPTA_CALL(n_); } while (0)
#else
#define PGO(nu) do { Uop *n_ = (nu); __attribute__((musttail)) return n_->p GPTA_CALL(n_); } while (0)
#endif
#define PNEXT() PGO(u + 1)
#define PTAIL(fn) do { __attribute__((musttail)) return fn GPTA_CALL(u); } while (0)
#define gZ ((uint64_t)0)

#define R16(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R16B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R17(M, ...) R16(M, __VA_ARGS__) M(Z, __VA_ARGS__)
#define R17B(M, ...) R16B(M, __VA_ARGS__) M(Z, __VA_ARGS__)
// A third level, for handlers specialised on three registers (base, index, data)
#define R16C(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R17C(M, ...) R16C(M, __VA_ARGS__) M(Z, __VA_ARGS__)
// The 16 x86 condition codes (a third expansion level, for handlers specialised on cc and registers)
#define C16(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)

#define M8 0xffull
#define M16 0xffffull
#define M32 0xffffffffull
#define M64 0xffffffffffffffffull
#define SI8 0
#define SI16 1
#define SI32 2
#define SI64 3
#define ST16 int16_t
#define ST32 int32_t
#define ST64 int64_t
#define FLV_F 1   // handler variants: F records the flags, N (flags dead) does not
#define FLV_N 0

#define SPILL_R() do { c->r[0] = g0; c->r[1] = g1; c->r[2] = g2; c->r[3] = g3; c->r[4] = g4; c->r[5] = g5; \
    c->r[6] = g6; c->r[7] = g7; c->r[8] = g8; c->r[9] = g9; c->r[10] = g10; c->r[11] = g11; c->r[12] = g12; \
    c->r[13] = g13; c->r[14] = g14; c->r[15] = g15; } while (0)
#define RELOAD_R() do { g0 = c->r[0]; g1 = c->r[1]; g2 = c->r[2]; g3 = c->r[3]; g4 = c->r[4]; g5 = c->r[5]; \
    g6 = c->r[6]; g7 = c->r[7]; g8 = c->r[8]; g9 = c->r[9]; g10 = c->r[10]; g11 = c->r[11]; g12 = c->r[12]; \
    g13 = c->r[13]; g14 = c->r[14]; g15 = c->r[15]; } while (0)
// Lazy flags: F0 = FXI's lf_op | lf_cin << 32, F1 = a, F2 = b, F3 = result.
#define SPILL_F() do { c->lf_op = (uint32_t)F0; c->lf_cin = (uint32_t)(F0 >> 32); c->lf_a = F1; c->lf_b = F2; c->lf_res = F3; } while (0)
#define RELOAD_F() do { F0 = (uint64_t)c->lf_op | (uint64_t)c->lf_cin << 32; F1 = c->lf_a; F2 = c->lf_b; F3 = c->lf_res; } while (0)
#define SETF(kind, si, a, b, r, cin) do { F0 = (uint64_t)LF(kind, si) | (uint64_t)(uint32_t)(cin) << 32; F1 = (a); F2 = (b); F3 = (r); } while (0)
// XMM0-7 (x0..x7) to and from c->xmm
FXI_INLINE GptaV xld(const void *p) { GptaV v; memcpy(&v, p, 16); return v; }
FXI_INLINE void xst(void *p, GptaV v) { memcpy(p, &v, 16); }
#define SPILL_X() do { xst(&c->xmm[0], x0); xst(&c->xmm[1], x1); xst(&c->xmm[2], x2); xst(&c->xmm[3], x3); \
    xst(&c->xmm[4], x4); xst(&c->xmm[5], x5); xst(&c->xmm[6], x6); xst(&c->xmm[7], x7); } while (0)
#define RELOAD_X() do { x0 = xld(&c->xmm[0]); x1 = xld(&c->xmm[1]); x2 = xld(&c->xmm[2]); x3 = xld(&c->xmm[3]); \
    x4 = xld(&c->xmm[4]); x5 = xld(&c->xmm[5]); x6 = xld(&c->xmm[6]); x7 = xld(&c->xmm[7]); } while (0)

// Out-of-line handlers every file tail-calls (defined in gpta_pin.c)
#define PX GPTA_CC void
PX gpta_p_slow(GPTA_PARAMS);
PX gpta_p_jcc_slow(GPTA_PARAMS);
PX gpta_p_miss_t(GPTA_PARAMS);
PX gpta_p_miss_f(GPTA_PARAMS);
PX gpta_p_miss_g(GPTA_PARAMS);
PX gpta_p_miss_ind(GPTA_PARAMS);
#define p_slow gpta_p_slow
#define p_jcc_slow gpta_p_jcc_slow
#define p_miss_t gpta_p_miss_t
#define p_miss_f gpta_p_miss_f
#define p_miss_g gpta_p_miss_g
#define p_miss_ind gpta_p_miss_ind

static const uint64_t kMask[4] = { M8, M16, M32, M64 };
static const uint64_t kSign[4] = { 0x80ull, 0x8000ull, 0x80000000ull, 0x8000000000000000ull };
#define PAR(r) (!__builtin_parity((unsigned)((r) & 0xff)))   // PF: an even number of set bits in the low byte

// ---- conditions from the operands of the last flag writer. With a constant cc (the handlers
// below are specialised on it) each folds to one comparison; a runtime cc is the slow path. ----
FXI_INLINE int cc_flags(unsigned cc, int o, int cf, int z, int s, int p) {
    int v;
    switch (cc >> 1) {
    case 0: v = o; break;
    case 1: v = cf; break;
    case 2: v = z; break;
    case 3: v = cf | z; break;
    case 4: v = s; break;
    case 5: v = p; break;
    case 6: v = s ^ o; break;
    default: v = z | (s ^ o); break;
    }
    return v ^ (int)(cc & 1);
}
FXI_INLINE int cc_sub(unsigned cc, uint64_t a, uint64_t b, unsigned si) {
    uint64_t m = kMask[si], sb = kSign[si];
    a &= m; b &= m;
    unsigned sh = 64 - (8u << si);
    int64_t sa = (int64_t)(a << sh) >> sh, sbv = (int64_t)(b << sh) >> sh;
    uint64_t r = (a - b) & m;
    switch (cc) {
    case 0: return (((a ^ b) & (a ^ r)) & sb) != 0;
    case 1: return (((a ^ b) & (a ^ r)) & sb) == 0;
    case 2: return a < b;
    case 3: return a >= b;
    case 4: return a == b;
    case 5: return a != b;
    case 6: return a <= b;
    case 7: return a > b;
    case 8: return (r & sb) != 0;
    case 9: return (r & sb) == 0;
    case 10: return PAR(r);
    case 11: return !PAR(r);
    case 12: return sa < sbv;
    case 13: return sa >= sbv;
    case 14: return sa <= sbv;
    default: return sa > sbv;
    }
}
FXI_INLINE int cc_logic(unsigned cc, uint64_t r, unsigned si) {
    r &= kMask[si];
    int z = r == 0, s = (r & kSign[si]) != 0;
    switch (cc) {
    case 0: case 2: return 0;
    case 1: case 3: return 1;
    case 4: case 6: return z;
    case 5: case 7: return !z;
    case 8: case 12: return s;
    case 9: case 13: return !s;
    case 10: return PAR(r);
    case 11: return !PAR(r);
    case 14: return z | s;
    default: return !z & !s;
    }
}
// Any condition from the lazy flags; *slow = 1 for a kind left to FXI's flag code.
FXI_INLINE int pcond(unsigned cc, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, int *slow) {
    unsigned op = (uint32_t)F0, k = op >> 2, si = op & 3;
    // ZF/SF/PF depend only on the saved result (or the raw flags word),
    // regardless of the operation that produced them. Match fxi_flag_zf/sf/pf
    // without walking the arithmetic-kind cascade or spilling to p_jcc_slow.
    if ((cc >> 1) == 2) {
        int v = k == LF_RAW ? (int)((F0 >> 38) & 1) : (F3 & kMask[si]) == 0;
        return v ^ (int)(cc & 1);
    }
    if ((cc >> 1) == 4) {
        int v = k == LF_RAW ? (int)((F0 >> 39) & 1) : (F3 & kSign[si]) != 0;
        return v ^ (int)(cc & 1);
    }
    if ((cc >> 1) == 5) {
        int v = k == LF_RAW ? (int)((F0 >> 34) & 1) : PAR(F3);
        return v ^ (int)(cc & 1);
    }
    if (FXI_LIKELY(k == LF_SUB)) return cc_sub(cc, F1, F2, si);
    if (k == LF_LOGIC) return cc_logic(cc, F3, si);
    uint64_t m = kMask[si], sb = kSign[si], a = F1 & m, b = F2 & m, r = F3 & m;
    int z = r == 0, s = (r & sb) != 0, ci = (int)(F0 >> 32) & 1;
    if (k == LF_ADD) return cc_flags(cc, (((a ^ r) & (b ^ r)) & sb) != 0, r < a, z, s, PAR(r));
    if (k == LF_INC) return cc_flags(cc, r == sb, ci, z, s, PAR(r));
    if (k == LF_DEC) return cc_flags(cc, r == sb - 1, ci, z, s, PAR(r));
    if (k == LF_RAW) { uint64_t f = F0 >> 32; return cc_flags(cc, (f >> 11) & 1, f & 1, (f >> 6) & 1, (f >> 7) & 1, (f >> 2) & 1); }
    *slow = 1;
    return 0;
}
// CF alone (adc, sbb, inc/dec keep it); *slow = 1 for a kind left to FXI.
FXI_INLINE uint64_t pcf(uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, int *slow) {
    unsigned op = (uint32_t)F0, k = op >> 2, si = op & 3;
    uint64_t m = kMask[si], a = F1 & m, b = F2 & m, r = F3 & m, ci = (F0 >> 32) & 1;
    if (k == LF_SUB) return a < b;
    if (k == LF_ADD) return r < a;
    if (k == LF_LOGIC) return 0;
    if (k == LF_ADC) return ci ? r <= a : r < a;
    if (k == LF_SBB) return ci ? a <= b : a < b;
    if (k == LF_INC || k == LF_DEC || k == LF_MUL || k == LF_RAW) return ci;
    *slow = 1;
    return 0;
}
// cmov's condition for any cc without a branch: a truth table (u->imm, from k_cctab) indexed by
// ZF | CF << 1 | SF << 2 | OF << 3. Parity conditions never get here (the lowering).
FXI_INLINE int pcond_tab(uint64_t tab, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, int *slow) {
    unsigned op = (uint32_t)F0, k = op >> 2, si = op & 3;
    uint64_t m = kMask[si], sb = kSign[si], a = F1 & m, b = F2 & m, r = F3 & m;
    unsigned z = r == 0, s = (r & sb) != 0, cf, o;
    if (FXI_LIKELY(k == LF_SUB)) { cf = a < b; o = (((a ^ b) & (a ^ r)) & sb) != 0; }
    else if (k == LF_LOGIC) { cf = 0; o = 0; }
    else if (k == LF_ADD) { cf = r < a; o = (((a ^ r) & (b ^ r)) & sb) != 0; }
    else { *slow = 1; return 0; }
    return (int)((tab >> (z | cf << 1 | s << 2 | o << 3)) & 1);
}

// ---- chaining ----
// Direct successors: the link holds the target block's first uop once known. The first time,
// an out-of-line handler looks the block up and fills the link (release: Windows threads).
#define PCHAIN(LINK, MISS) do { Uop *l_ = (LINK); if (FXI_LIKELY(l_ != 0)) PGO(l_); PTAIL(MISS); } while (0)
// A conditional branch's two successors (imm: taken target, aux: fallthrough)
// The direction is a host conditional branch (the conditional predictor), each side chains to
// its link. Measured: selecting the link with csel and one indirect jump instead was much slower
// on Neoverse N2 (the indirect predictor then predicts every guest branch's direction).
// An edge that leads to the next uop (a loop trace, gpta_lower) steps there with an add, as PNEXT
// does: the next dispatch then does not wait for the link load, which only feeds this predicted
// compare. (Measured on Neoverse N2: a dispatch whose uop pointer comes from a load costs 4
// cycles, one from an add about 1.3; engine/gpta-probe/dispatch_probe.c.)
#define PCHAIN_T(LINK, MISS) do {                                                          \
        Uop *lk_ = (LINK), *nx_ = u + 1;                                                   \
        if (lk_ == nx_) { __asm__("" : "+r"(nx_)); PGO(nx_); }                             \
        if (FXI_LIKELY(lk_ != 0)) PGO(lk_);                                                \
        PTAIL(MISS);                                                                       \
    } while (0)
#define FJ_BR(TAKEN) do { if (TAKEN) PCHAIN_T(u->ulink, p_miss_t); PCHAIN_T(u->ulink2, p_miss_f); } while (0)
// Indirect branches (ret, jmp/call through a register or memory): a per-thread direct-mapped
// cache of target -> first uop; a miss looks the block up (target in T) and fills the entry.
#define IBTC_MASK 1023u
#define IBTC(t) (&c->gpta_ibtc[((t) ^ ((t) >> 10)) & IBTC_MASK])
#define PIND(target) do {                                                                  \
        uint64_t t_ = (target);                                                            \
        __typeof__(c->gpta_ibtc[0]) *e_ = IBTC(t_);                                         \
        if (FXI_LIKELY(e_->rip == t_)) PGO(e_->u);                                         \
        T = t_; PTAIL(p_miss_ind);                                                         \
    } while (0)

// ===========================================================================
// SSE. XMM0-7 are pinned in host vector registers (x0..x7, passed along like the GPRs); XMM8-15
// stay in c->xmm. Handlers are specialised on the class of each XMM operand: 0-7, or M (in
// memory, its index from the uop). The bodies are vector expressions with FXI's results (x86's);
// ops without a pinned form run FXI's handler through p_slow, which spills x0..x7 too.
// ===========================================================================
typedef float VF __attribute__((vector_size(16)));
typedef double VD __attribute__((vector_size(16)));
typedef uint32_t VU4 __attribute__((vector_size(16)));
typedef int32_t VS4 __attribute__((vector_size(16)));
typedef int64_t VS2 __attribute__((vector_size(16)));
typedef uint16_t VU8 __attribute__((vector_size(16)));
typedef int16_t VS8 __attribute__((vector_size(16)));
typedef uint8_t VU16 __attribute__((vector_size(16)));
typedef int8_t VS16 __attribute__((vector_size(16)));

// The 9 XMM operand classes (0-7 pinned, M in memory); F is the macro applied to each.
#define X9(F, ...) F(0, __VA_ARGS__) F(1, __VA_ARGS__) F(2, __VA_ARGS__) F(3, __VA_ARGS__) F(4, __VA_ARGS__) \
    F(5, __VA_ARGS__) F(6, __VA_ARGS__) F(7, __VA_ARGS__) F(M, __VA_ARGS__)
#define X9B(F, ...) F(0, __VA_ARGS__) F(1, __VA_ARGS__) F(2, __VA_ARGS__) F(3, __VA_ARGS__) F(4, __VA_ARGS__) \
    F(5, __VA_ARGS__) F(6, __VA_ARGS__) F(7, __VA_ARGS__) F(M, __VA_ARGS__)
// Operand access by class: XD_ the destination's value, XS_ the source's, XW_ writes the destination.
#define XD_0 x0
#define XD_1 x1
#define XD_2 x2
#define XD_3 x3
#define XD_4 x4
#define XD_5 x5
#define XD_6 x6
#define XD_7 x7
#define XD_8 xld(&c->xmm[8])
#define XD_9 xld(&c->xmm[9])
#define XD_10 xld(&c->xmm[10])
#define XD_11 xld(&c->xmm[11])
#define XD_12 xld(&c->xmm[12])
#define XD_13 xld(&c->xmm[13])
#define XD_14 xld(&c->xmm[14])
#define XD_15 xld(&c->xmm[15])
#define XD_M xld(&c->xmm[u->dst])
#define XS_0 x0
#define XS_1 x1
#define XS_2 x2
#define XS_3 x3
#define XS_4 x4
#define XS_5 x5
#define XS_6 x6
#define XS_7 x7
#define XS_8 XD_8
#define XS_9 XD_9
#define XS_10 XD_10
#define XS_11 XD_11
#define XS_12 XD_12
#define XS_13 XD_13
#define XS_14 XD_14
#define XS_15 XD_15
#define XS_M xld(&c->xmm[u->src])
#define XW_0(v) (x0 = (v))
#define XW_1(v) (x1 = (v))
#define XW_2(v) (x2 = (v))
#define XW_3(v) (x3 = (v))
#define XW_4(v) (x4 = (v))
#define XW_5(v) (x5 = (v))
#define XW_6(v) (x6 = (v))
#define XW_7(v) (x7 = (v))
#define XW_8(v) xst(&c->xmm[8], (v))
#define XW_9(v) xst(&c->xmm[9], (v))
#define XW_10(v) xst(&c->xmm[10], (v))
#define XW_11(v) xst(&c->xmm[11], (v))
#define XW_12(v) xst(&c->xmm[12], (v))
#define XW_13(v) xst(&c->xmm[13], (v))
#define XW_14(v) xst(&c->xmm[14], (v))
#define XW_15(v) xst(&c->xmm[15], (v))
#define XW_M(v) xst(&c->xmm[u->dst], (v))
// Exact faults (gpta_win_host_state): after a guest memory access, these keep a pinned value the
// handler is about to overwrite (the flag words, an XMM destination) in its register until there,
// so the compiler does not use that register as scratch before the access. They emit nothing.
// With preserve_none on ARM64 they name the very host register (fxi_internal.h, GPTA_PARAMS: the
// flag words x12 x13 x14 x9, XMM0-7 v0-v7, the guest GPRs HREG_n): a value merely kept alive
// could sit in another register while its own register served as scratch.
#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none)
#define KEEP_IN(T, V, REG, C) do { register T k_ __asm__(REG) = (V); __asm__ volatile("" : "+" C(k_) :: "memory"); (V) = k_; } while (0)
#define KEEP_X(X, REG) __asm__ volatile("" : "+w"(X) :: "memory")   /* (the unbound form sufficed here) */
#define KEEP_F() do {                                                                      \
        register uint64_t f0_ __asm__("x12") = F0, f1_ __asm__("x13") = F1, f2_ __asm__("x14") = F2, f3_ __asm__("x9") = F3; \
        __asm__ volatile("" : "+r"(f0_), "+r"(f1_), "+r"(f2_), "+r"(f3_) :: "memory");    \
        F0 = f0_; F1 = f1_; F2 = f2_; F3 = f3_;                                            \
    } while (0)
#define HREG_0 "x22"
#define HREG_1 "x23"
#define HREG_2 "x24"
#define HREG_3 "x28"
#define HREG_4 "x27"
#define HREG_5 "x0"
#define HREG_6 "x25"
#define HREG_7 "x26"
#define HREG_8 "x1"
#define HREG_9 "x2"
#define HREG_10 "x3"
#define HREG_11 "x4"
#define HREG_12 "x5"
#define HREG_13 "x6"
#define HREG_14 "x7"
#define HREG_15 "x20"
#define KEEP_R(D) KEEP_IN(uint64_t, g##D, HREG_##D, "r")
#else
#define KEEP_X(X, REG) __asm__ volatile("" : "+x"(X) :: "memory")
#define KEEP_F() __asm__ volatile("" : "+r"(F0), "+r"(F1), "+r"(F2), "+r"(F3) :: "memory")
#define KEEP_R(D) __asm__ volatile("" : "+r"(g##D) :: "memory")
#endif
#define XKEEP_0 KEEP_X(x0, "v0")
#define XKEEP_1 KEEP_X(x1, "v1")
#define XKEEP_2 KEEP_X(x2, "v2")
#define XKEEP_3 KEEP_X(x3, "v3")
#define XKEEP_4 KEEP_X(x4, "v4")
#define XKEEP_5 KEEP_X(x5, "v5")
#define XKEEP_6 KEEP_X(x6, "v6")
#define XKEEP_7 KEEP_X(x7, "v7")
#define XKEEP_M ((void)0)   // XMM8-15 are in the CPU structure

#define LF0(v) (((VF)(v))[0])
#define LD0(v) (((VD)(v))[0])
FXI_INLINE GptaV xlo_f(GptaV d, float v) { VF r = (VF)d; r[0] = v; return (GptaV)r; }
FXI_INLINE GptaV xlo_d(GptaV d, double v) { VD r = (VD)d; r[0] = v; return (GptaV)r; }
FXI_INLINE GptaV xlo_u32(GptaV d, uint32_t v) { VU4 r = (VU4)d; r[0] = v; return (GptaV)r; }
#define XSEL(m, a, b) ((((GptaV)(m)) & (a)) | (~((GptaV)(m)) & (b)))   // per lane: m ? a : b
// (u)comiss/(u)comisd flags: ZF PF CF from the compare, OF SF AF clear, as raw lazy flags (F0 only)
#define XCOMIS_SET(a, b) do { double a_ = (a), b_ = (b);                                   \
        F0 = (uint64_t)LF(LF_RAW, 3) | ((uint64_t)((a_ != a_) | (b_ != b_)) * 0x45 | (uint64_t)(a_ < b_) | (uint64_t)(a_ == b_) << 6) << 32;   /* branch-free */ \
    } while (0)
// A memory operand of W bytes, zero-extended to 16
FXI_INLINE GptaV xldn(uint64_t a, int w) {
    if (w == 16) return xld((const void *)(uintptr_t)a);
    if (w == 8) return (GptaV){ ld64(a), 0 };
    return (GptaV){ ld32(a), 0 };
}
// Byte shuffles: NEON tbl with the indices the lowering stored in u->xidx (>= 16 / 32: zero)
#if defined(__aarch64__)
#include <arm_neon.h>
FXI_INLINE GptaV xtbl1(GptaV t, GptaV i) { return (GptaV)vqtbl1q_u8((uint8x16_t)t, (uint8x16_t)i); }
FXI_INLINE GptaV xtbl2(GptaV a, GptaV b, GptaV i) {
    uint8x16x2_t t = { { (uint8x16_t)a, (uint8x16_t)b } };
    return (GptaV)vqtbl2q_u8(t, (uint8x16_t)i);
}
#else
FXI_INLINE GptaV xtbl1(GptaV t, GptaV i) {
    VU16 a = (VU16)t, x = (VU16)i, r;
    for (int k = 0; k < 16; k++) r[k] = x[k] < 16 ? a[x[k]] : 0;
    return (GptaV)r;
}
FXI_INLINE GptaV xtbl2(GptaV a, GptaV b, GptaV i) {
    VU16 p = (VU16)a, q = (VU16)b, x = (VU16)i, r;
    for (int k = 0; k < 16; k++) r[k] = x[k] < 16 ? p[x[k]] : x[k] < 32 ? q[x[k] - 16] : 0;
    return (GptaV)r;
}
#endif
#define XIDX xld(u->xidx)

FXI_INLINE GptaV xsqrtps(GptaV s) {
    VF v = (VF)s;
    VF r = { __builtin_sqrtf(v[0]), __builtin_sqrtf(v[1]), __builtin_sqrtf(v[2]), __builtin_sqrtf(v[3]) };
    return (GptaV)r;
}
FXI_INLINE GptaV xsqrtpd(GptaV s) { VD v = (VD)s; VD r = { __builtin_sqrt(v[0]), __builtin_sqrt(v[1]) }; return (GptaV)r; }
// cmpps/cmppd predicate (imm & 7): eq lt le unord, then their negations
FXI_INLINE GptaV xcmpps(GptaV d, GptaV s, unsigned p) {
    VF a = (VF)d, b = (VF)s;
    VS4 m;
    switch (p & 3) {
    case 0: m = (VS4)(a == b); break;
    case 1: m = (VS4)(a < b); break;
    case 2: m = (VS4)(a <= b); break;
    default: m = (VS4)(a != a) | (VS4)(b != b); break;
    }
    if (p & 4) m = ~m;
    return (GptaV)m;
}
FXI_INLINE GptaV xcmppd(GptaV d, GptaV s, unsigned p) {
    VD a = (VD)d, b = (VD)s;
    VS2 m;
    switch (p & 3) {
    case 0: m = (VS2)(a == b); break;
    case 1: m = (VS2)(a < b); break;
    case 2: m = (VS2)(a <= b); break;
    default: m = (VS2)(a != a) | (VS2)(b != b); break;
    }
    if (p & 4) m = ~m;
    return (GptaV)m;
}
// Float -> int32/int64: out of range or NaN gives the "integer indefinite" (FXI's cvt_i32/i64)
FXI_INLINE int32_t xcvt32(double v, int trunc) {
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return INT32_MIN;
    return (int32_t)(trunc ? v : nearbyint(v));
}
FXI_INLINE int64_t xcvt64(double v, int trunc) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)(trunc ? v : nearbyint(v));
}
FXI_INLINE GptaV xcvtps(GptaV s, int t) {
    VF v = (VF)s;
    VS4 r = { xcvt32(v[0], t), xcvt32(v[1], t), xcvt32(v[2], t), xcvt32(v[3], t) };
    return (GptaV)r;
}
FXI_INLINE GptaV xcvtpd(GptaV s, int t) { VD v = (VD)s; VS4 r = { xcvt32(v[0], t), xcvt32(v[1], t), 0, 0 }; return (GptaV)r; }
FXI_INLINE uint64_t xmovmskb(GptaV s) {
    VU16 m = (VU16)((VS16)s < (VS16){ 0 }) & (VU16){ 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
#if defined(__aarch64__)
    return (uint64_t)vaddv_u8(vget_low_u8((uint8x16_t)m)) | (uint64_t)vaddv_u8(vget_high_u8((uint8x16_t)m)) << 8;
#else
    uint64_t r = 0;
    for (int i = 0; i < 16; i++) r |= (uint64_t)(m[i] != 0) << i;
    return r;
#endif
}
FXI_INLINE uint64_t xmovmskps(GptaV s) { VU4 v = (VU4)s >> 31; return v[0] | v[1] << 1 | v[2] << 2 | v[3] << 3; }
FXI_INLINE uint64_t xmovmskpd(GptaV s) { return (s[0] >> 63) | (s[1] >> 63) << 1; }


// ---- the handler tables (defined next to their handlers) ----
enum { KL32, KL64, KZ8, KZ16, KS8_32, KS8_64, KS16_32, KS16_64, KS32_64, K_COUNT };   // load kinds
enum { PS_SHL, PS_SHR, PS_SAR, PS_ROL, PS_ROR };
enum { XG_SI2SS32, XG_SI2SS64, XG_SI2SD32, XG_SI2SD64, XG_MOVD32, XG_MOVD64, XG_COUNT };
enum { GX_TSS32, GX_SS32, GX_TSD32, GX_SD32, GX_TSS64, GX_SS64, GX_TSD64, GX_SD64, GX_MOVD32, GX_MOVD64,
       GX_PMOVMSKB, GX_MOVMSKPS, GX_MOVMSKPD, GX_COUNT };
enum { FL_LD32, FL_LD64, FL_LDZ8 };   // t_ldi kinds; t_stx: 8, 32, 64-bit stores
// Register-register operations also specialize memory-backed XMM8-15: their
// offsets are constants, so no runtime operand-index loads are needed.
typedef struct { const char *name; PFn rr[16][16]; PFn rt[9]; } XOp;
typedef struct { const char *name; PFn h[9]; } XShift;
#define GPTA_TABLES(X) \
    X(t_setcc, [16][16]) X(t_alu_rr, [2][ALU_COUNT][2][16][16]) X(t_alu_ri, [2][ALU_COUNT][2][16]) \
    X(t_alu_rt, [2][ALU_COUNT][2][16]) X(t_alu_tr, [2][ALU_COUNT][4][16]) X(t_alu_ti, [2][ALU_COUNT][4]) \
    X(t_stti, [4]) X(t_ld, [2][17][16]) X(t_st, [4][17][16]) X(t_lea, [2][17][16]) X(t_sti, [4][17]) \
    X(t_sti_bi, [4][17][17]) X(t_ea, [17][17]) X(t_eaz, [17][17]) X(t_mov_rr, [2][16][16]) X(t_mov_ri, [2][16]) X(t_movt, [2][16]) \
    X(t_ldt, [K_COUNT][16]) X(t_stt, [4][16]) X(t_ext, [K_COUNT][16][16]) X(t_sh, [2][5][2][2][16]) \
    X(t_un, [2][2][4][16]) X(t_imul2, [2][2][16][16]) X(t_imul3, [2][2][16][16]) X(t_imul2t, [2][2][16]) \
    X(t_imul3t, [2][2][16]) X(t_cmov, [2][16][16]) X(t_cmovt, [2][16]) \
    X(t_fjc_rr, [2][16][16][16]) X(t_fjc_ri, [2][16][16]) X(t_fjt_ri, [2][16][16]) X(t_fjt_rr, [2][16][16]) \
    X(t_fjt_rrx, [2][16][16]) \
    X(t_xl_movx, [9][17][17]) X(t_xs_movx, [9][17][17]) X(t_xl_movxz, [9][17][17]) X(t_xs_movxz, [9][17][17]) X(t_xl_movss, [9][17]) X(t_xl_movsd, [9][17]) \
    X(t_xs_movss, [9][17]) X(t_xs_movsd, [9][17]) X(t_xlt_movss, [9]) X(t_xlt_movsd, [9]) X(t_xlt_movlps, [9]) \
    X(t_xlt_movhps, [9]) X(t_xst_movss, [9]) X(t_xst_movsd, [9]) X(t_xst_movhps, [9]) X(t_xst_movx, [9]) \
    X(t_xg, [XG_COUNT][9][16]) X(t_xgt, [4][9]) X(t_gx, [GX_COUNT][9][16]) \
    X(t_ldi, [3][17][16][16]) X(t_stx, [3][17][16][16]) X(t_ldz8s, [17][16]) \
    X(t_fmi, [2][4][16][17]) X(t_fmit, [2][4][16]) X(t_fmr, [3][4][16][16]) X(t_fjs_ri, [2][2][16][16]) \
    X(t_fjs_rr, [2][2][16][16]) X(t_fjs_rrx, [2][2][16][16]) X(t_fcj, [2][16][9][9]) X(t_fcjt, [2][16][9]) \
    X(t_fai, [5][2][16][16]) X(t_fid, [2][2][4][16]) X(t_amr, [2][16][16]) X(t_alea, [17][16]) X(t_ami, [2][16]) \
    X(t_axz, [16]) X(t_pop2, [16][16]) X(t_push2, [16][16]) X(t_popret, [16]) X(t_ltj, [2][6][17][16]) \
    X(t_sic, [2][2][10][16][16]) X(t_sicr, [2][2][10][16][16]) X(t_sii, [2][2][10][16]) X(t_sri, [2][2][10][16][16]) \
    X(t_3op, [10][2][16][16]) \
    X(t_fs_sub, [4][16][16]) X(t_fs_and, [4][16][16]) X(t_fs_subi, [4][16]) X(t_fs_andi, [4][16]) X(t_fs_log, [4][16]) \
    X(t_fs_addr, [4][16]) X(t_fs_subr, [4][16]) X(t_fs_comis, [2][9][9]) \
    X(t_fs_addrr, [4][16][16]) X(t_fs_subrr, [4][16][16]) \
    X(t_x3, [20][9][9][9]) X(t_arj, [5][2][8][16][16]) X(t_lcj, [2][16][17][16]) \
    X(t_xmemi, [12][9][17][17]) X(t_xmemb, [12][9][17])
#define GPTA_DECL_TABLE(NAME, DIMS) extern const PFn GPTA_T(NAME) DIMS; extern const PFn gpta_##NAME##_r1 DIMS;
GPTA_TABLES(GPTA_DECL_TABLE)
#define t_setcc GPTA_T(t_setcc)
#define t_xmemi GPTA_T(t_xmemi)
#define t_xmemb GPTA_T(t_xmemb)
#define t_alu_rr GPTA_T(t_alu_rr)
#define t_alu_ri GPTA_T(t_alu_ri)
#define t_alu_rt GPTA_T(t_alu_rt)
#define t_alu_tr GPTA_T(t_alu_tr)
#define t_alu_ti GPTA_T(t_alu_ti)
#define t_stti GPTA_T(t_stti)
#define t_ld GPTA_T(t_ld)
#define t_st GPTA_T(t_st)
#define t_lea GPTA_T(t_lea)
#define t_sti GPTA_T(t_sti)
#define t_sti_bi GPTA_T(t_sti_bi)
#define t_ea GPTA_T(t_ea)
#define t_eaz GPTA_T(t_eaz)
#define t_mov_rr GPTA_T(t_mov_rr)
#define t_mov_ri GPTA_T(t_mov_ri)
#define t_movt GPTA_T(t_movt)
#define t_ldt GPTA_T(t_ldt)
#define t_stt GPTA_T(t_stt)
#define t_ext GPTA_T(t_ext)
#define t_sh GPTA_T(t_sh)
#define t_un GPTA_T(t_un)
#define t_imul2 GPTA_T(t_imul2)
#define t_imul3 GPTA_T(t_imul3)
#define t_imul2t GPTA_T(t_imul2t)
#define t_imul3t GPTA_T(t_imul3t)
#define t_cmov GPTA_T(t_cmov)
#define t_cmovt GPTA_T(t_cmovt)
#define t_fjc_rr GPTA_T(t_fjc_rr)
#define t_fjc_ri GPTA_T(t_fjc_ri)
#define t_fjt_ri GPTA_T(t_fjt_ri)
#define t_fjt_rr GPTA_T(t_fjt_rr)
#define t_fjt_rrx GPTA_T(t_fjt_rrx)
#define t_xl_movx GPTA_T(t_xl_movx)
#define t_xs_movx GPTA_T(t_xs_movx)
#define t_xl_movxz GPTA_T(t_xl_movxz)
#define t_xs_movxz GPTA_T(t_xs_movxz)
#define t_xl_movss GPTA_T(t_xl_movss)
#define t_xl_movsd GPTA_T(t_xl_movsd)
#define t_xs_movss GPTA_T(t_xs_movss)
#define t_xs_movsd GPTA_T(t_xs_movsd)
#define t_xlt_movss GPTA_T(t_xlt_movss)
#define t_xlt_movsd GPTA_T(t_xlt_movsd)
#define t_xlt_movlps GPTA_T(t_xlt_movlps)
#define t_xlt_movhps GPTA_T(t_xlt_movhps)
#define t_xst_movss GPTA_T(t_xst_movss)
#define t_xst_movsd GPTA_T(t_xst_movsd)
#define t_xst_movhps GPTA_T(t_xst_movhps)
#define t_xst_movx GPTA_T(t_xst_movx)
#define t_xg GPTA_T(t_xg)
#define t_gx GPTA_T(t_gx)
#define t_xgt GPTA_T(t_xgt)
#define t_ldi GPTA_T(t_ldi)
#define t_ldz8s GPTA_T(t_ldz8s)
#define t_stx GPTA_T(t_stx)
#define t_fmi GPTA_T(t_fmi)
#define t_fmit GPTA_T(t_fmit)
#define t_fmr GPTA_T(t_fmr)
#define t_fjs_ri GPTA_T(t_fjs_ri)
#define t_fjs_rr GPTA_T(t_fjs_rr)
#define t_fjs_rrx GPTA_T(t_fjs_rrx)
#define t_fcj GPTA_T(t_fcj)
#define t_fcjt GPTA_T(t_fcjt)
#define t_fai GPTA_T(t_fai)
#define t_fid GPTA_T(t_fid)
#define t_amr GPTA_T(t_amr)
#define t_alea GPTA_T(t_alea)
#define t_ami GPTA_T(t_ami)
#define t_axz GPTA_T(t_axz)
#define t_pop2 GPTA_T(t_pop2)
#define t_push2 GPTA_T(t_push2)
#define t_popret GPTA_T(t_popret)
#define t_ltj GPTA_T(t_ltj)
#define t_sic GPTA_T(t_sic)
#define t_sicr GPTA_T(t_sicr)
#define t_sii GPTA_T(t_sii)
#define t_sri GPTA_T(t_sri)
#define t_3op GPTA_T(t_3op)
#define t_fs_sub GPTA_T(t_fs_sub)
#define t_fs_and GPTA_T(t_fs_and)
#define t_fs_subi GPTA_T(t_fs_subi)
#define t_fs_andi GPTA_T(t_fs_andi)
#define t_fs_log GPTA_T(t_fs_log)
#define t_fs_addr GPTA_T(t_fs_addr)
#define t_fs_subr GPTA_T(t_fs_subr)
#define t_fs_comis GPTA_T(t_fs_comis)
#define t_fs_addrr GPTA_T(t_fs_addrr)
#define t_fs_subrr GPTA_T(t_fs_subrr)
#define t_x3 GPTA_T(t_x3)
#define t_arj GPTA_T(t_arj)
#define t_lcj GPTA_T(t_lcj)
// The SSE ops with a 3-operand fused form (copy ; op), in t_x3 order; -1 when not one of them
static inline int x3_index(const char *n) {
    static const char *const k[20] = { "addps", "subps", "mulps", "divps", "addpd", "subpd", "mulpd", "divpd",
        "addss", "subss", "mulss", "divss", "addsd", "subsd", "mulsd", "divsd", "pand", "pandn", "por", "pxor" };
    for (int i = 0; i < 20; i++) if (!strcmp(n, k[i])) return i;
    return -1;
}
// The 8 conditions ALU results are most often tested for (b ae e ne s ns l ge), for t_arj
#define C8(M, ...) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(8, __VA_ARGS__) \
    M(9, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__)
static inline int cc8(unsigned cc) {
    static const signed char k[16] = { -1, -1, 0, 1, 2, 3, -1, -1, 4, 5, -1, -1, 6, 7, -1, -1 };
    return k[cc & 15];
}
// A third XMM operand's class macros (index in u->base), for 3-operand SSE pairs
#define X9C(F, ...) F(0, __VA_ARGS__) F(1, __VA_ARGS__) F(2, __VA_ARGS__) F(3, __VA_ARGS__) F(4, __VA_ARGS__) \
    F(5, __VA_ARGS__) F(6, __VA_ARGS__) F(7, __VA_ARGS__) F(M, __VA_ARGS__)
#define XX_0 x0
#define XX_1 x1
#define XX_2 x2
#define XX_3 x3
#define XX_4 x4
#define XX_5 x5
#define XX_6 x6
#define XX_7 x7
#define XX_M xld(&c->xmm[u->base])
// The 10 conditions counted loops exit on (b ae e ne be a l ge le g), for the loop-step fusions
#define C10(M, ...) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) \
    M(7, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
static inline int cc10(unsigned cc) {   // index into a C10 table, -1 for the others
    static const signed char k[16] = { -1, -1, 0, 1, 2, 3, 4, 5, -1, -1, -1, -1, 6, 7, 8, 9 };
    return k[cc & 15];
}
extern const XOp gpta_xops[];
extern const size_t gpta_n_xops;
extern const XShift gpta_xshift[];
extern const size_t gpta_n_xshift;
void gpta_xshuffle_index(const char *op, unsigned imm, uint8_t *x);
#ifndef GPTA_R1
extern const XOp gpta_xops_r1[];       // the replicas (gpta_pin_sse_r1.c)
extern const XShift gpta_xshift_r1[];
#endif
#define E1(R, NAME) p_##NAME##_##R,
#define E_RR(S, D, NAME) p_##NAME##_##D##_##S,
#define ROW_RR(D, NAME) { R16B(E_RR, D, NAME) },
static inline int xcls(unsigned r) { return r < 8 ? (int)r : 8; }   // XMM operand class: 0-7 pinned, 8 = M

#endif
