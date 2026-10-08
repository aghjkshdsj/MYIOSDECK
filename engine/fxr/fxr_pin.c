// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: FXI with the guest's general registers pinned in host registers.
//
// FXI keeps the x86 register file in memory, so every instruction loads and stores it.
// Here the 16 guest GPRs, an effective-address temporary T and the four lazy-flag words
// are the arguments of every handler, and clang's preserve_none calling convention passes
// all of them in host registers (ARM64: 23 argument registers). Each hot instruction form
// has a handler specialised on its register operands, so `add rax, rbx` compiles to one
// ARM64 add plus the dispatch. Everything else runs FXI's own handler through p_slow,
// which spills the registers around the call: full coverage, FXI's exact semantics.
// No loop or idiom recognition: every specialisation is per instruction (or per
// compare-and-branch pair, which FXI's decoder fuses).
//
// Hot handlers never call a function: a call would give them a stack frame and spill
// pinned registers on the fast path. Rare cases tail-call an out-of-line handler instead:
// p_miss_* (first use of a block link, indirect-branch cache misses), p_jcc_slow (a
// condition on an unusual flag kind) and p_slow (redo the instruction with FXI's handler).
// Flags: the decoder's liveness pass, which also looks into the successor blocks, marks
// each uop with whether anything reads the flags after it (u->flive); dead flags are not
// recorded, and conditions are specialised on their condition code.

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "fxi_internal.h"

#define PH static FXR_CC void
// Dispatch: the handler pointer is the uop's first field, so stepping to the next uop is one
// load with writeback and an indirect branch.
#define PGO(nu) do { Uop *n_ = (nu); __attribute__((musttail)) return n_->p(c, n_, FXR_ARGS); } while (0)
#define PNEXT() PGO(u + 1)
#define PTAIL(fn) do { __attribute__((musttail)) return fn(c, u, FXR_ARGS); } while (0)
#define gZ ((uint64_t)0)

#define R16(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R16B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R17(M, ...) R16(M, __VA_ARGS__) M(Z, __VA_ARGS__)
#define R17B(M, ...) R16B(M, __VA_ARGS__) M(Z, __VA_ARGS__)
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
FXI_INLINE FxrV xld(const void *p) { FxrV v; memcpy(&v, p, 16); return v; }
FXI_INLINE void xst(void *p, FxrV v) { memcpy(p, &v, 16); }
#define SPILL_X() do { xst(&c->xmm[0], x0); xst(&c->xmm[1], x1); xst(&c->xmm[2], x2); xst(&c->xmm[3], x3); \
    xst(&c->xmm[4], x4); xst(&c->xmm[5], x5); xst(&c->xmm[6], x6); xst(&c->xmm[7], x7); } while (0)
#define RELOAD_X() do { x0 = xld(&c->xmm[0]); x1 = xld(&c->xmm[1]); x2 = xld(&c->xmm[2]); x3 = xld(&c->xmm[3]); \
    x4 = xld(&c->xmm[4]); x5 = xld(&c->xmm[5]); x6 = xld(&c->xmm[6]); x7 = xld(&c->xmm[7]); } while (0)

PH p_slow(FXR_PARAMS);
PH p_jcc_slow(FXR_PARAMS);
PH p_miss_t(FXR_PARAMS);
PH p_miss_f(FXR_PARAMS);
PH p_miss_g(FXR_PARAMS);
PH p_miss_ind(FXR_PARAMS);

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
static uint16_t k_cctab[16];
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
#define DEF_MISS(NAME, SLOT, RIP)                                                          \
    PH NAME(FXR_PARAMS) {                                                                  \
        Block *b = fxi_lookup(c, (RIP));                                                   \
        if (b != fxi_stop) __atomic_store_n(&(SLOT), b->u, __ATOMIC_RELEASE);              \
        PGO(b->u);                                                                         \
    }
DEF_MISS(p_miss_t, u->ulink, u->imm)     // jump, call or taken branch: the target in imm
DEF_MISS(p_miss_f, u->ulink2, u->aux)    // a conditional branch's fallthrough (aux)
DEF_MISS(p_miss_g, u->ulink, u->aux)     // goto (a block split) and syscall: the next instruction
// Indirect branches (ret, jmp/call through a register or memory): a per-thread direct-mapped
// cache of target -> first uop; a miss looks the block up (target in T) and fills the entry.
#define IBTC_MASK 1023u
#define IBTC(t) (&c->fxr_ibtc[((t) ^ ((t) >> 10)) & IBTC_MASK])
#define PIND(target) do {                                                                  \
        uint64_t t_ = (target);                                                            \
        __typeof__(c->fxr_ibtc[0]) *e_ = IBTC(t_);                                         \
        if (FXI_LIKELY(e_->rip == t_)) PGO(e_->u);                                         \
        T = t_; PTAIL(p_miss_ind);                                                         \
    } while (0)
PH p_miss_ind(FXR_PARAMS) {
    Block *b = fxi_lookup(c, T);
    if (b != fxi_stop) { __typeof__(c->fxr_ibtc[0]) *e = IBTC(T); e->u = b->u; e->rip = T; }
    PGO(b->u);
}

// Leaving the chain (exit, error): the CPU state goes back to memory.
PH p_stop(FXR_PARAMS) { SPILL_R(); SPILL_F(); SPILL_X(); }
PH p_nop(FXR_PARAMS) { PNEXT(); }
PH p_jmp(FXR_PARAMS) { PCHAIN(u->ulink, p_miss_t); }
PH p_goto(FXR_PARAMS) { PCHAIN(u->ulink, p_miss_g); }
PH p_call(FXR_PARAMS) { g4 -= 8; st64(g4, u->aux); PCHAIN(u->ulink, p_miss_t); }
PH p_ret(FXR_PARAMS) { uint64_t t = ld64(g4); g4 += 8 + u->aux; PIND(t); }
PH p_call_T(FXR_PARAMS) { uint64_t t = ld64(T); g4 -= 8; st64(g4, u->aux); PIND(t); }
PH p_jmp_T(FXR_PARAMS) { PIND(ld64(T)); }
// An fs/gs operand: the segment base lives in memory, so the address is computed FXI's way.
PH p_call_M(FXR_PARAMS) { SPILL_R(); uint64_t t = ld64(fxi_ea(c, u)); g4 -= 8; st64(g4, u->aux); PIND(t); }
PH p_jmp_M(FXR_PARAMS) { SPILL_R(); PIND(ld64(fxi_ea(c, u))); }
// Conditional branch on the lazy flags, one handler per condition code.
PH p_jcc_slow(FXR_PARAMS) {
    SPILL_F();
    if (fxi_cond(c, u->cc)) PCHAIN(u->ulink, p_miss_t);
    PCHAIN(u->ulink2, p_miss_f);
}
#define DEF_JCC(CC, _)                                                                     \
    PH p_jcc_##CC(FXR_PARAMS) {                                                            \
        int s_ = 0, t_ = pcond(CC, F0, F1, F2, F3, &s_);                                   \
        if (FXI_UNLIKELY(s_)) PTAIL(p_jcc_slow);                                           \
        if (t_) PCHAIN(u->ulink, p_miss_t);                                                \
        PCHAIN(u->ulink2, p_miss_f);                                                       \
    }
C16(DEF_JCC, _)
PH p_syscall(FXR_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X();
    c->rip = u->aux;
    long r = fxi_syscall(c);
    if (c->stop) return;
    RELOAD_R();
    g0 = (uint64_t)r;
    g1 = u->aux;                  // syscall clobbers RCX (return rip) and R11 (rflags)
    g11 = fxi_rflags(c);
    PCHAIN(u->ulink, p_miss_g);
}
// Anything without a pinned form: FXI's handler, with the registers in memory around it.
PH p_slow(FXR_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X();
    u->fn(c, u);
    if (FXI_UNLIKELY(c->stop)) return;
    RELOAD_R(); RELOAD_F(); RELOAD_X();
    PNEXT();
}

#define DEF_PUSHPOP(R, _)                                                                  \
    PH p_push_##R(FXR_PARAMS) { uint64_t v = g##R; g4 -= 8; st64(g4, v); PNEXT(); }        \
    PH p_pop_##R(FXR_PARAMS) { uint64_t v = ld64(g4); g4 += 8; g##R = v; PNEXT(); }       \
    PH p_call_R_##R(FXR_PARAMS) { uint64_t t = g##R; g4 -= 8; st64(g4, u->aux); PIND(t); } \
    PH p_jmp_R_##R(FXR_PARAMS) { PIND(g##R); }
R16(DEF_PUSHPOP, _)
#define E1(R, NAME) p_##NAME##_##R,
static const PFn t_push[16] = { R16(E1, push) }, t_pop[16] = { R16(E1, pop) };
static const PFn t_call_R[16] = { R16(E1, call_R) }, t_jmp_R[16] = { R16(E1, jmp_R) };
static const PFn t_jcc[16] = { C16(E1, jcc) };
// setcc, specialised on cc and register
#define DEF_SETCC(R, CC)                                                                   \
    PH p_setcc_##CC##_##R(FXR_PARAMS) {                                                    \
        int s_ = 0, t_ = pcond(CC, F0, F1, F2, F3, &s_);                                   \
        if (FXI_UNLIKELY(s_)) PTAIL(p_slow);                                               \
        g##R = (g##R & ~0xffull) | (uint64_t)t_; PNEXT();                                  \
    }
#define DEF_SETCC_CC(CC, _) R16(DEF_SETCC, CC)
C16(DEF_SETCC_CC, _)
#define E_SETCC(R, CC) p_setcc_##CC##_##R,
#define ROW_SETCC(CC, _) { R16(E_SETCC, CC) },
static const PFn t_setcc[16][16] = { C16(ROW_SETCC, _) };   // [cc][reg]

// ---- ALU: add or adc sbb and sub xor cmp test (32/64 registers; memory forms via T) ----
#define X_add(a, b, ci) ((a) + (b))
#define X_or(a, b, ci) ((a) | (b))
#define X_adc(a, b, ci) ((a) + (b) + (ci))
#define X_sbb(a, b, ci) ((a) - (b) - (ci))
#define X_and(a, b, ci) ((a) & (b))
#define X_sub(a, b, ci) ((a) - (b))
#define X_xor(a, b, ci) ((a) ^ (b))
#define X_cmp(a, b, ci) ((a) - (b))
#define X_test(a, b, ci) ((a) & (b))
#define K_add LF_ADD
#define K_or LF_LOGIC
#define K_adc LF_ADC
#define K_sbb LF_SBB
#define K_and LF_LOGIC
#define K_sub LF_SUB
#define K_xor LF_LOGIC
#define K_cmp LF_SUB
#define K_test LF_LOGIC
#define W_add 1
#define W_or 1
#define W_adc 1
#define W_sbb 1
#define W_and 1
#define W_sub 1
#define W_xor 1
#define W_cmp 0
#define W_test 0
#define C_add 0
#define C_or 0
#define C_adc 1
#define C_sbb 1
#define C_and 0
#define C_sub 0
#define C_xor 0
#define C_cmp 0
#define C_test 0
// adc/sbb read CF first; a flag kind the pinned code cannot read sends the instruction to p_slow
// before anything changed.
#define ALU_BODY(OPN, SZ, A, B, STORE, FL)                                                 \
    uint64_t ci = 0;                                                                       \
    if (C_##OPN) { int s_ = 0; ci = pcf(F0, F1, F2, F3, &s_); if (FXI_UNLIKELY(s_)) PTAIL(p_slow); } \
    uint64_t a = (A) & M##SZ, b = (B) & M##SZ, r = X_##OPN(a, b, ci) & M##SZ;              \
    (void)ci;                                                                              \
    if (W_##OPN) { STORE; }                                                                \
    if (FLV_##FL) SETF(K_##OPN, SI##SZ, a, b, r, ci);                                      \
    PNEXT();

#define DEF_ALU_RR(S, D, OPN, SZ, FL) PH p_alu_rr_##OPN##_##SZ##FL##_##D##_##S(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, g##S, g##D = r, FL) }
#define DEF_ALU_RR_ROW(D, OPN, SZ, FL) R16B(DEF_ALU_RR, D, OPN, SZ, FL)
#define DEF_ALU_R(D, OPN, SZ, FL)                                                          \
    PH p_alu_ri_##OPN##_##SZ##FL##_##D(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, u->imm, g##D = r, FL) } \
    PH p_alu_rt_##OPN##_##SZ##FL##_##D(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, ld##SZ(T), g##D = r, FL) }
#define DEF_ALU_T(S, OPN, SZ, FL) PH p_alu_tr_##OPN##_##SZ##FL##_##S(FXR_PARAMS) { ALU_BODY(OPN, SZ, ld##SZ(T), g##S, st##SZ(T, r), FL) }
#define DEF_ALU_SZ(OPN, SZ, FL) R16(DEF_ALU_RR_ROW, OPN, SZ, FL) R16(DEF_ALU_R, OPN, SZ, FL)
#define DEF_ALU_TSZ(OPN, SZ, FL) R16(DEF_ALU_T, OPN, SZ, FL) \
    PH p_alu_ti_##OPN##_##SZ##FL(FXR_PARAMS) { ALU_BODY(OPN, SZ, ld##SZ(T), u->imm, st##SZ(T, r), FL) }
#define DEF_ALU_FL(OPN, FL) DEF_ALU_SZ(OPN, 32, FL) DEF_ALU_SZ(OPN, 64, FL) \
    DEF_ALU_TSZ(OPN, 8, FL) DEF_ALU_TSZ(OPN, 16, FL) DEF_ALU_TSZ(OPN, 32, FL) DEF_ALU_TSZ(OPN, 64, FL)
#define DEF_ALU(OPN) DEF_ALU_FL(OPN, F) DEF_ALU_FL(OPN, N)
DEF_ALU(add) DEF_ALU(or) DEF_ALU(adc) DEF_ALU(sbb) DEF_ALU(and) DEF_ALU(sub) DEF_ALU(xor) DEF_ALU(cmp) DEF_ALU(test)

#define E_ALU_RR(S, D, OPN, SZF) p_alu_rr_##OPN##_##SZF##_##D##_##S,
#define ROW_ALU_RR(D, OPN, SZF) { R16B(E_ALU_RR, D, OPN, SZF) },
#define T_ALU_RR(OPN, FL) { { R16(ROW_ALU_RR, OPN, 32##FL) }, { R16(ROW_ALU_RR, OPN, 64##FL) } },
#define T_ALU_RR_ALL(FL) { T_ALU_RR(add, FL) T_ALU_RR(or, FL) T_ALU_RR(adc, FL) T_ALU_RR(sbb, FL) T_ALU_RR(and, FL) \
    T_ALU_RR(sub, FL) T_ALU_RR(xor, FL) T_ALU_RR(cmp, FL) T_ALU_RR(test, FL) }
static const PFn t_alu_rr[2][ALU_COUNT][2][16][16] = { T_ALU_RR_ALL(N), T_ALU_RR_ALL(F) };   // [flags live][op][64?][D][S]
#define E_ALU_X(D, KIND, OPN, SZF) p_alu_##KIND##_##OPN##_##SZF##_##D,
#define T_ALU_X(KIND, OPN, FL) { { R16(E_ALU_X, KIND, OPN, 32##FL) }, { R16(E_ALU_X, KIND, OPN, 64##FL) } },
#define T_ALU_X_ALL(KIND, FL) { T_ALU_X(KIND, add, FL) T_ALU_X(KIND, or, FL) T_ALU_X(KIND, adc, FL) T_ALU_X(KIND, sbb, FL) \
    T_ALU_X(KIND, and, FL) T_ALU_X(KIND, sub, FL) T_ALU_X(KIND, xor, FL) T_ALU_X(KIND, cmp, FL) T_ALU_X(KIND, test, FL) }
static const PFn t_alu_ri[2][ALU_COUNT][2][16] = { T_ALU_X_ALL(ri, N), T_ALU_X_ALL(ri, F) };
static const PFn t_alu_rt[2][ALU_COUNT][2][16] = { T_ALU_X_ALL(rt, N), T_ALU_X_ALL(rt, F) };
#define T_ALU_TR(OPN, FL) { { R16(E_ALU_X, tr, OPN, 8##FL) }, { R16(E_ALU_X, tr, OPN, 16##FL) }, \
    { R16(E_ALU_X, tr, OPN, 32##FL) }, { R16(E_ALU_X, tr, OPN, 64##FL) } },
#define T_ALU_TR_ALL(FL) { T_ALU_TR(add, FL) T_ALU_TR(or, FL) T_ALU_TR(adc, FL) T_ALU_TR(sbb, FL) T_ALU_TR(and, FL) \
    T_ALU_TR(sub, FL) T_ALU_TR(xor, FL) T_ALU_TR(cmp, FL) T_ALU_TR(test, FL) }
static const PFn t_alu_tr[2][ALU_COUNT][4][16] = { T_ALU_TR_ALL(N), T_ALU_TR_ALL(F) };
#define T_ALU_TI(OPN, FL) { p_alu_ti_##OPN##_8##FL, p_alu_ti_##OPN##_16##FL, p_alu_ti_##OPN##_32##FL, p_alu_ti_##OPN##_64##FL },
#define T_ALU_TI_ALL(FL) { T_ALU_TI(add, FL) T_ALU_TI(or, FL) T_ALU_TI(adc, FL) T_ALU_TI(sbb, FL) T_ALU_TI(and, FL) \
    T_ALU_TI(sub, FL) T_ALU_TI(xor, FL) T_ALU_TI(cmp, FL) T_ALU_TI(test, FL) }
static const PFn t_alu_ti[2][ALU_COUNT][4] = { T_ALU_TI_ALL(N), T_ALU_TI_ALL(F) };

// ---- moves, loads, stores, lea, effective addresses ----
// Load kinds (also movzx/movsx sources): value from an address A.
enum { KL32, KL64, KZ8, KZ16, KS8_32, KS8_64, KS16_32, KS16_64, KS32_64, K_COUNT };
#define LDK_KL32(A) ld32(A)
#define LDK_KL64(A) ld64(A)
#define LDK_KZ8(A) ld8(A)
#define LDK_KZ16(A) ld16(A)
#define LDK_KS8_32(A) (uint64_t)(uint32_t)(int32_t)(int8_t)ld8(A)
#define LDK_KS8_64(A) (uint64_t)(int64_t)(int8_t)ld8(A)
#define LDK_KS16_32(A) (uint64_t)(uint32_t)(int32_t)(int16_t)ld16(A)
#define LDK_KS16_64(A) (uint64_t)(int64_t)(int16_t)ld16(A)
#define LDK_KS32_64(A) (uint64_t)(int64_t)(int32_t)ld32(A)
// The same extensions from a register value V.
#define EXK_KZ8(V) ((V) & M8)
#define EXK_KZ16(V) ((V) & M16)
#define EXK_KS8_32(V) (uint64_t)(uint32_t)(int32_t)(int8_t)(V)
#define EXK_KS8_64(V) (uint64_t)(int64_t)(int8_t)(V)
#define EXK_KS16_32(V) (uint64_t)(uint32_t)(int32_t)(int16_t)(V)
#define EXK_KS16_64(V) (uint64_t)(int64_t)(int16_t)(V)
#define EXK_KS32_64(V) (uint64_t)(int64_t)(int32_t)(V)

#define DEF_MOV_RR(S, D, SZ) PH p_mov_rr_##SZ##_##D##_##S(FXR_PARAMS) { g##D = g##S & M##SZ; PNEXT(); }
#define DEF_MOV_RR_ROW(D, SZ) R16B(DEF_MOV_RR, D, SZ)
R16(DEF_MOV_RR_ROW, 32) R16(DEF_MOV_RR_ROW, 64)
#define DEF_MOV_D(D, _)                                                                    \
    PH p_mov_ri_32_##D(FXR_PARAMS) { g##D = u->imm & M32; PNEXT(); }                      \
    PH p_mov_ri_64_##D(FXR_PARAMS) { g##D = u->imm; PNEXT(); }                            \
    PH p_movt_32_##D(FXR_PARAMS) { g##D = T & M32; PNEXT(); }                             \
    PH p_movt_64_##D(FXR_PARAMS) { g##D = T; PNEXT(); }                                   \
    PH p_ldt_KL32_##D(FXR_PARAMS) { g##D = LDK_KL32(T); PNEXT(); }                        \
    PH p_ldt_KL64_##D(FXR_PARAMS) { g##D = LDK_KL64(T); PNEXT(); }                        \
    PH p_ldt_KZ8_##D(FXR_PARAMS) { g##D = LDK_KZ8(T); PNEXT(); }                          \
    PH p_ldt_KZ16_##D(FXR_PARAMS) { g##D = LDK_KZ16(T); PNEXT(); }                        \
    PH p_ldt_KS8_32_##D(FXR_PARAMS) { g##D = LDK_KS8_32(T); PNEXT(); }                    \
    PH p_ldt_KS8_64_##D(FXR_PARAMS) { g##D = LDK_KS8_64(T); PNEXT(); }                    \
    PH p_ldt_KS16_32_##D(FXR_PARAMS) { g##D = LDK_KS16_32(T); PNEXT(); }                  \
    PH p_ldt_KS16_64_##D(FXR_PARAMS) { g##D = LDK_KS16_64(T); PNEXT(); }                  \
    PH p_ldt_KS32_64_##D(FXR_PARAMS) { g##D = LDK_KS32_64(T); PNEXT(); }                  \
    PH p_stt_8_##D(FXR_PARAMS) { st8(T, g##D); PNEXT(); }                                 \
    PH p_stt_16_##D(FXR_PARAMS) { st16(T, g##D); PNEXT(); }                               \
    PH p_stt_32_##D(FXR_PARAMS) { st32(T, g##D); PNEXT(); }                               \
    PH p_stt_64_##D(FXR_PARAMS) { st64(T, g##D); PNEXT(); }
R16(DEF_MOV_D, _)
PH p_stti_8(FXR_PARAMS) { st8(T, u->imm); PNEXT(); }
PH p_stti_16(FXR_PARAMS) { st16(T, u->imm); PNEXT(); }
PH p_stti_32(FXR_PARAMS) { st32(T, u->imm); PNEXT(); }
PH p_stti_64(FXR_PARAMS) { st64(T, u->imm); PNEXT(); }
static const PFn t_stti[4] = { p_stti_8, p_stti_16, p_stti_32, p_stti_64 };

// [base + disp] forms, base specialised (Z: absolute / RIP-relative address in disp)
#define DEF_BASE_D(D, B)                                                                   \
    PH p_ld_KL32_##B##_##D(FXR_PARAMS) { g##D = ld32(g##B + (uint64_t)u->disp); PNEXT(); } \
    PH p_ld_KL64_##B##_##D(FXR_PARAMS) { g##D = ld64(g##B + (uint64_t)u->disp); PNEXT(); } \
    PH p_st_8_##B##_##D(FXR_PARAMS) { st8(g##B + (uint64_t)u->disp, g##D); PNEXT(); }      \
    PH p_st_16_##B##_##D(FXR_PARAMS) { st16(g##B + (uint64_t)u->disp, g##D); PNEXT(); }    \
    PH p_st_32_##B##_##D(FXR_PARAMS) { st32(g##B + (uint64_t)u->disp, g##D); PNEXT(); }    \
    PH p_st_64_##B##_##D(FXR_PARAMS) { st64(g##B + (uint64_t)u->disp, g##D); PNEXT(); }    \
    PH p_lea_32_##B##_##D(FXR_PARAMS) { g##D = (g##B + (uint64_t)u->disp) & M32; PNEXT(); } \
    PH p_lea_64_##B##_##D(FXR_PARAMS) { g##D = g##B + (uint64_t)u->disp; PNEXT(); }
#define DEF_BASE(B, _) R16B(DEF_BASE_D, B)                                                 \
    PH p_sti_8_##B(FXR_PARAMS) { st8(g##B + (uint64_t)u->disp, u->imm); PNEXT(); }         \
    PH p_sti_16_##B(FXR_PARAMS) { st16(g##B + (uint64_t)u->disp, u->imm); PNEXT(); }       \
    PH p_sti_32_##B(FXR_PARAMS) { st32(g##B + (uint64_t)u->disp, u->imm); PNEXT(); }       \
    PH p_sti_64_##B(FXR_PARAMS) { st64(g##B + (uint64_t)u->disp, u->imm); PNEXT(); }
R17(DEF_BASE, _)
// T = base + index << scale + disp
#define DEF_EA(I, B) PH p_ea_##B##_##I(FXR_PARAMS) { T = g##B + (g##I << u->scale) + (uint64_t)u->disp; PNEXT(); }
#define DEF_EA_ROW(B, _) R17B(DEF_EA, B)
R17(DEF_EA_ROW, _)

#define E2(D, B, NAME) p_##NAME##_##B##_##D,
#define ROW_B(B, NAME) { R16B(E2, B, NAME) },
static const PFn t_ld[2][17][16] = { { R17(ROW_B, ld_KL32) }, { R17(ROW_B, ld_KL64) } };
static const PFn t_st[4][17][16] = { { R17(ROW_B, st_8) }, { R17(ROW_B, st_16) }, { R17(ROW_B, st_32) }, { R17(ROW_B, st_64) } };
static const PFn t_lea[2][17][16] = { { R17(ROW_B, lea_32) }, { R17(ROW_B, lea_64) } };
#define E_STI(B, SZ) p_sti_##SZ##_##B,
static const PFn t_sti[4][17] = { { R17(E_STI, 8) }, { R17(E_STI, 16) }, { R17(E_STI, 32) }, { R17(E_STI, 64) } };
#define E_EA(I, B) p_ea_##B##_##I,
#define ROW_EA(B, _) { R17B(E_EA, B) },
static const PFn t_ea[17][17] = { R17(ROW_EA, _) };
#define ROW_RR(D, NAME) { R16B(E_RR, D, NAME) },
#define E_RR(S, D, NAME) p_##NAME##_##D##_##S,
static const PFn t_mov_rr[2][16][16] = { { R16(ROW_RR, mov_rr_32) }, { R16(ROW_RR, mov_rr_64) } };
static const PFn t_mov_ri[2][16] = { { R16(E1, mov_ri_32) }, { R16(E1, mov_ri_64) } };
static const PFn t_movt[2][16] = { { R16(E1, movt_32) }, { R16(E1, movt_64) } };
static const PFn t_ldt[K_COUNT][16] = {
    { R16(E1, ldt_KL32) }, { R16(E1, ldt_KL64) }, { R16(E1, ldt_KZ8) }, { R16(E1, ldt_KZ16) }, { R16(E1, ldt_KS8_32) },
    { R16(E1, ldt_KS8_64) }, { R16(E1, ldt_KS16_32) }, { R16(E1, ldt_KS16_64) }, { R16(E1, ldt_KS32_64) },
};
static const PFn t_stt[4][16] = { { R16(E1, stt_8) }, { R16(E1, stt_16) }, { R16(E1, stt_32) }, { R16(E1, stt_64) } };

// movzx/movsx from a register (kinds KZ8.. KS32_64)
#define DEF_EXT(S, D, K) PH p_ext_##K##_##D##_##S(FXR_PARAMS) { g##D = EXK_##K(g##S); PNEXT(); }
#define DEF_EXT_ROW(D, K) R16B(DEF_EXT, D, K)
R16(DEF_EXT_ROW, KZ8) R16(DEF_EXT_ROW, KZ16) R16(DEF_EXT_ROW, KS8_32) R16(DEF_EXT_ROW, KS8_64)
R16(DEF_EXT_ROW, KS16_32) R16(DEF_EXT_ROW, KS16_64) R16(DEF_EXT_ROW, KS32_64)
#define ROW_EXT(D, K) { R16B(E_EXT, D, K) },
#define E_EXT(S, D, K) p_ext_##K##_##D##_##S,
static const PFn t_ext[K_COUNT][16][16] = {
    [KZ8] = { R16(ROW_EXT, KZ8) }, [KZ16] = { R16(ROW_EXT, KZ16) }, [KS8_32] = { R16(ROW_EXT, KS8_32) },
    [KS8_64] = { R16(ROW_EXT, KS8_64) }, [KS16_32] = { R16(ROW_EXT, KS16_32) }, [KS16_64] = { R16(ROW_EXT, KS16_64) },
    [KS32_64] = { R16(ROW_EXT, KS32_64) },
};

// ---- shifts/rotates of a register (32/64). Count: I = immediate (masked and nonzero: the
// lowering turns a zero count into a no-op), C = CL (a zero count changes nothing but a 32-bit
// register's upper half). Rotates get a pinned form only when their flags are dead.
#define SH_shl(a, n, SZ) ((a) << (n))
#define SH_shr(a, n, SZ) ((a) >> (n))
#define SH_sar(a, n, SZ) (uint64_t)((int64_t)((a) << (64 - SZ)) >> (64 - SZ) >> (n))
#define SH_rol(a, n, SZ) ((((a) << ((n) % SZ)) | ((a) >> ((SZ - (n) % SZ) % SZ))))
#define SH_ror(a, n, SZ) ((((a) >> ((n) % SZ)) | ((a) << ((SZ - (n) % SZ) % SZ))))
#define SHK_shl LF_SHL
#define SHK_shr LF_SHR
#define SHK_sar LF_SAR
#define SHK_rol 0
#define SHK_ror 0
#define SHF_shl 1
#define SHF_shr 1
#define SHF_sar 1
#define SHF_rol 0
#define SHF_ror 0
#define SH_N_I(SZ) ((unsigned)u->imm)
#define SH_N_C(SZ) ((unsigned)g1 & (SZ == 64 ? 63u : 31u))
#define SH_Z_I 0
#define SH_Z_C 1
#define DEF_SH(D, OPN, SZ, CNT, FL)                                                        \
    PH p_sh_##OPN##_##SZ##CNT##FL##_##D(FXR_PARAMS) {                                      \
        unsigned n = SH_N_##CNT(SZ);                                                       \
        if (SH_Z_##CNT && FXI_UNLIKELY(n == 0)) { if (SZ == 32) g##D &= M32; PNEXT(); }    \
        uint64_t a = g##D & M##SZ, r = SH_##OPN(a, n, SZ) & M##SZ;                         \
        g##D = r;                                                                          \
        if (SHF_##OPN && FLV_##FL) SETF(SHK_##OPN, SI##SZ, a, n, r, 0);                    \
        PNEXT();                                                                           \
    }
#define DEF_SH_FL(OPN, FL) R16(DEF_SH, OPN, 32, I, FL) R16(DEF_SH, OPN, 64, I, FL) \
    R16(DEF_SH, OPN, 32, C, FL) R16(DEF_SH, OPN, 64, C, FL)
#define DEF_SH_ALL(OPN) DEF_SH_FL(OPN, F) DEF_SH_FL(OPN, N)
DEF_SH_ALL(shl) DEF_SH_ALL(shr) DEF_SH_ALL(sar) DEF_SH_ALL(rol) DEF_SH_ALL(ror)
#define E_SH(D, OPN, SZCF) p_sh_##OPN##_##SZCF##_##D,
#define T_SH(OPN, FL) { { { R16(E_SH, OPN, 32I##FL) }, { R16(E_SH, OPN, 32C##FL) } }, \
    { { R16(E_SH, OPN, 64I##FL) }, { R16(E_SH, OPN, 64C##FL) } } },
enum { PS_SHL, PS_SHR, PS_SAR, PS_ROL, PS_ROR };
static const PFn t_sh[2][5][2][2][16] = {   // [flags live][op][64?][CL?][D]
    { T_SH(shl, N) T_SH(shr, N) T_SH(sar, N) T_SH(rol, N) T_SH(ror, N) },
    { T_SH(shl, F) T_SH(shr, F) T_SH(sar, F) T_SH(rol, F) T_SH(ror, F) },
};

// ---- inc dec not neg (32/64 registers). inc/dec keep CF, so recording their flags reads it. ----
#define DEF_UN(D, SZ, FL)                                                                  \
    PH p_inc_##SZ##FL##_##D(FXR_PARAMS) { uint64_t ci = 0;                                 \
        if (FLV_##FL) { int s_ = 0; ci = pcf(F0, F1, F2, F3, &s_); if (FXI_UNLIKELY(s_)) PTAIL(p_slow); } \
        uint64_t a = g##D & M##SZ, r = (a + 1) & M##SZ; g##D = r;                          \
        if (FLV_##FL) SETF(LF_INC, SI##SZ, a, 1, r, ci); PNEXT(); }                        \
    PH p_dec_##SZ##FL##_##D(FXR_PARAMS) { uint64_t ci = 0;                                 \
        if (FLV_##FL) { int s_ = 0; ci = pcf(F0, F1, F2, F3, &s_); if (FXI_UNLIKELY(s_)) PTAIL(p_slow); } \
        uint64_t a = g##D & M##SZ, r = (a - 1) & M##SZ; g##D = r;                          \
        if (FLV_##FL) SETF(LF_DEC, SI##SZ, a, 1, r, ci); PNEXT(); }                        \
    PH p_not_##SZ##FL##_##D(FXR_PARAMS) { g##D = ~g##D & M##SZ; PNEXT(); }                 \
    PH p_neg_##SZ##FL##_##D(FXR_PARAMS) { uint64_t a = g##D & M##SZ, r = (0 - a) & M##SZ; g##D = r; \
        if (FLV_##FL) SETF(LF_NEG, SI##SZ, a, 0, r, 0); PNEXT(); }
R16(DEF_UN, 32, F) R16(DEF_UN, 64, F) R16(DEF_UN, 32, N) R16(DEF_UN, 64, N)
#define T_UN(SZF) { { R16(E1, inc_##SZF) }, { R16(E1, dec_##SZF) }, { R16(E1, not_##SZF) }, { R16(E1, neg_##SZF) } }
static const PFn t_un[2][2][4][16] = { { T_UN(32N), T_UN(64N) }, { T_UN(32F), T_UN(64F) } };   // [flags live][64?][op][D]

// ---- imul (two/three operand), cmov ----
// The low half is all a flags-dead imul needs; CF/OF (the high half) only when recorded.
#define IMUL_BODY(SZ, A, B, FL)                                                            \
    int64_t a = (ST##SZ)(A), b = (ST##SZ)(B);                                              \
    uint64_t lo = ((uint64_t)a * (uint64_t)b) & M##SZ;                                     \
    if (FLV_##FL) { __int128 r = (__int128)a * b; SETF(LF_MUL, SI##SZ, 0, 0, lo, r != (ST##SZ)r); }
#define DEF_IMUL(S, D, SZ, FL)                                                             \
    PH p_imul2_##SZ##FL##_##D##_##S(FXR_PARAMS) { IMUL_BODY(SZ, g##D, g##S, FL) g##D = lo; PNEXT(); } \
    PH p_imul3_##SZ##FL##_##D##_##S(FXR_PARAMS) { IMUL_BODY(SZ, g##S, u->imm, FL) g##D = lo; PNEXT(); }
#define DEF_IMUL_ROW(D, SZ, FL) R16B(DEF_IMUL, D, SZ, FL)                                  \
    PH p_imul2t_##SZ##FL##_##D(FXR_PARAMS) { IMUL_BODY(SZ, g##D, ld##SZ(T), FL) g##D = lo; PNEXT(); } \
    PH p_imul3t_##SZ##FL##_##D(FXR_PARAMS) { IMUL_BODY(SZ, ld##SZ(T), u->imm, FL) g##D = lo; PNEXT(); }
R16(DEF_IMUL_ROW, 32, F) R16(DEF_IMUL_ROW, 64, F) R16(DEF_IMUL_ROW, 32, N) R16(DEF_IMUL_ROW, 64, N)
#define DEF_CMOV(S, D, SZ)                                                                 \
    PH p_cmov_##SZ##_##D##_##S(FXR_PARAMS) {                                               \
        int s_ = 0, t_ = pcond_tab(u->imm, F0, F1, F2, F3, &s_);                           \
        if (FXI_UNLIKELY(s_)) PTAIL(p_slow);                                               \
        g##D = t_ ? (g##S & M##SZ) : (g##D & M##SZ); PNEXT(); }
#define DEF_CMOV_ROW(D, SZ) R16B(DEF_CMOV, D, SZ)                                          \
    PH p_cmovt_##SZ##_##D(FXR_PARAMS) {   /* the load happens whatever the condition */    \
        int s_ = 0, t_ = pcond_tab(u->imm, F0, F1, F2, F3, &s_);                           \
        if (FXI_UNLIKELY(s_)) PTAIL(p_slow);                                               \
        uint64_t v = ld##SZ(T); g##D = t_ ? v : (g##D & M##SZ); PNEXT(); }
R16(DEF_CMOV_ROW, 32) R16(DEF_CMOV_ROW, 64)
static const PFn t_imul2[2][2][16][16] = { { { R16(ROW_RR, imul2_32N) }, { R16(ROW_RR, imul2_64N) } },
                                           { { R16(ROW_RR, imul2_32F) }, { R16(ROW_RR, imul2_64F) } } };
static const PFn t_imul3[2][2][16][16] = { { { R16(ROW_RR, imul3_32N) }, { R16(ROW_RR, imul3_64N) } },
                                           { { R16(ROW_RR, imul3_32F) }, { R16(ROW_RR, imul3_64F) } } };
static const PFn t_imul2t[2][2][16] = { { { R16(E1, imul2t_32N) }, { R16(E1, imul2t_64N) } },
                                        { { R16(E1, imul2t_32F) }, { R16(E1, imul2t_64F) } } };
static const PFn t_imul3t[2][2][16] = { { { R16(E1, imul3t_32N) }, { R16(E1, imul3t_64N) } },
                                        { { R16(E1, imul3t_32F) }, { R16(E1, imul3t_64F) } } };
static const PFn t_cmov[2][16][16] = { { R16(ROW_RR, cmov_32) }, { R16(ROW_RR, cmov_64) } };
static const PFn t_cmovt[2][16] = { { R16(E1, cmovt_32) }, { R16(E1, cmovt_64) } };

// ---- fused cmp/test + jcc, specialised on the condition code; the flags are recorded only if
// something after the branch reads them. RI: the immediate rides in disp (imm is the target). ----
#define FJ_BR(TAKEN) do { if (TAKEN) PCHAIN(u->ulink, p_miss_t); PCHAIN(u->ulink2, p_miss_f); } while (0)
#define DEF_FJC_RR(S, D, SZ, CC)                                                           \
    PH p_fjc_rr_##SZ##_##CC##_##D##_##S(FXR_PARAMS) {                                      \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ;                                       \
        if (u->flive) SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0);                      \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }
#define DEF_FJC_RR_ROW(D, SZ, CC) R16B(DEF_FJC_RR, D, SZ, CC)
#define DEF_FJ_D(D, SZ, CC)                                                                \
    PH p_fjc_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        if (u->flive) SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0);                      \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fjt_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = a & b;               \
        if (u->flive) SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                  \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }                                                  \
    PH p_fjt_rr_##SZ##_##CC##_##D(FXR_PARAMS) {   /* test r, r: the same register */       \
        uint64_t a = g##D & M##SZ;                                                         \
        if (u->flive) SETF(LF_LOGIC, SI##SZ, a, a, a, 0);                                  \
        FJ_BR(cc_logic(CC, a, SI##SZ)); }
#define DEF_FJ_CC(CC, SZ) R16(DEF_FJC_RR_ROW, SZ, CC) R16(DEF_FJ_D, SZ, CC)
C16(DEF_FJ_CC, 32) C16(DEF_FJ_CC, 64)
// test of two different registers (less common): the condition code at run time
#define DEF_FJT_RRX(S, D, SZ)                                                              \
    PH p_fjt_rrx_##SZ##_##D##_##S(FXR_PARAMS) {                                            \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = a & b;                            \
        if (u->flive) SETF(LF_LOGIC, SI##SZ, a, b, r, 0);                                  \
        FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
#define DEF_FJT_RRX_ROW(D, SZ) R16B(DEF_FJT_RRX, D, SZ)
R16(DEF_FJT_RRX_ROW, 32) R16(DEF_FJT_RRX_ROW, 64)
#define E_FJC_RR(S, D, SZ, CC) p_fjc_rr_##SZ##_##CC##_##D##_##S,
#define ROW_FJC_RR(D, SZ, CC) { R16B(E_FJC_RR, D, SZ, CC) },
#define CC_FJC_RR(CC, SZ) { R16(ROW_FJC_RR, SZ, CC) },
static const PFn t_fjc_rr[2][16][16][16] = { { C16(CC_FJC_RR, 32) }, { C16(CC_FJC_RR, 64) } };   // [64?][cc][D][S]
#define E_FJ_X(D, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##D,
#define CC_FJ_X(CC, SZ, NAME) { R16(E_FJ_X, SZ, CC, NAME) },
static const PFn t_fjc_ri[2][16][16] = { { C16(CC_FJ_X, 32, fjc_ri) }, { C16(CC_FJ_X, 64, fjc_ri) } };   // [64?][cc][D]
static const PFn t_fjt_ri[2][16][16] = { { C16(CC_FJ_X, 32, fjt_ri) }, { C16(CC_FJ_X, 64, fjt_ri) } };
static const PFn t_fjt_rr[2][16][16] = { { C16(CC_FJ_X, 32, fjt_rr) }, { C16(CC_FJ_X, 64, fjt_rr) } };
static const PFn t_fjt_rrx[2][16][16] = { { R16(ROW_RR, fjt_rrx_32) }, { R16(ROW_RR, fjt_rrx_64) } };   // [64?][D][S]

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
#define XD_M xld(&c->xmm[u->dst])
#define XS_0 x0
#define XS_1 x1
#define XS_2 x2
#define XS_3 x3
#define XS_4 x4
#define XS_5 x5
#define XS_6 x6
#define XS_7 x7
#define XS_M xld(&c->xmm[u->src])
#define XW_0(v) (x0 = (v))
#define XW_1(v) (x1 = (v))
#define XW_2(v) (x2 = (v))
#define XW_3(v) (x3 = (v))
#define XW_4(v) (x4 = (v))
#define XW_5(v) (x5 = (v))
#define XW_6(v) (x6 = (v))
#define XW_7(v) (x7 = (v))
#define XW_M(v) xst(&c->xmm[u->dst], (v))

#define LF0(v) (((VF)(v))[0])
#define LD0(v) (((VD)(v))[0])
FXI_INLINE FxrV xlo_f(FxrV d, float v) { VF r = (VF)d; r[0] = v; return (FxrV)r; }
FXI_INLINE FxrV xlo_d(FxrV d, double v) { VD r = (VD)d; r[0] = v; return (FxrV)r; }
FXI_INLINE FxrV xlo_u32(FxrV d, uint32_t v) { VU4 r = (VU4)d; r[0] = v; return (FxrV)r; }
#define XSEL(m, a, b) ((((FxrV)(m)) & (a)) | (~((FxrV)(m)) & (b)))   // per lane: m ? a : b
// A memory operand of W bytes, zero-extended to 16
FXI_INLINE FxrV xldn(uint64_t a, int w) {
    if (w == 16) return xld((const void *)(uintptr_t)a);
    if (w == 8) return (FxrV){ ld64(a), 0 };
    return (FxrV){ ld32(a), 0 };
}
// Byte shuffles: NEON tbl with the indices the lowering stored in u->xidx (>= 16 / 32: zero)
#if defined(__aarch64__)
#include <arm_neon.h>
FXI_INLINE FxrV xtbl1(FxrV t, FxrV i) { return (FxrV)vqtbl1q_u8((uint8x16_t)t, (uint8x16_t)i); }
FXI_INLINE FxrV xtbl2(FxrV a, FxrV b, FxrV i) {
    uint8x16x2_t t = { { (uint8x16_t)a, (uint8x16_t)b } };
    return (FxrV)vqtbl2q_u8(t, (uint8x16_t)i);
}
#else
FXI_INLINE FxrV xtbl1(FxrV t, FxrV i) {
    VU16 a = (VU16)t, x = (VU16)i, r;
    for (int k = 0; k < 16; k++) r[k] = x[k] < 16 ? a[x[k]] : 0;
    return (FxrV)r;
}
FXI_INLINE FxrV xtbl2(FxrV a, FxrV b, FxrV i) {
    VU16 p = (VU16)a, q = (VU16)b, x = (VU16)i, r;
    for (int k = 0; k < 16; k++) r[k] = x[k] < 16 ? p[x[k]] : x[k] < 32 ? q[x[k] - 16] : 0;
    return (FxrV)r;
}
#endif
#define XIDX xld(u->xidx)

FXI_INLINE FxrV xsqrtps(FxrV s) {
    VF v = (VF)s;
    VF r = { __builtin_sqrtf(v[0]), __builtin_sqrtf(v[1]), __builtin_sqrtf(v[2]), __builtin_sqrtf(v[3]) };
    return (FxrV)r;
}
FXI_INLINE FxrV xsqrtpd(FxrV s) { VD v = (VD)s; VD r = { __builtin_sqrt(v[0]), __builtin_sqrt(v[1]) }; return (FxrV)r; }
// cmpps/cmppd predicate (imm & 7): eq lt le unord, then their negations
FXI_INLINE FxrV xcmpps(FxrV d, FxrV s, unsigned p) {
    VF a = (VF)d, b = (VF)s;
    VS4 m;
    switch (p & 3) {
    case 0: m = (VS4)(a == b); break;
    case 1: m = (VS4)(a < b); break;
    case 2: m = (VS4)(a <= b); break;
    default: m = (VS4)(a != a) | (VS4)(b != b); break;
    }
    if (p & 4) m = ~m;
    return (FxrV)m;
}
FXI_INLINE FxrV xcmppd(FxrV d, FxrV s, unsigned p) {
    VD a = (VD)d, b = (VD)s;
    VS2 m;
    switch (p & 3) {
    case 0: m = (VS2)(a == b); break;
    case 1: m = (VS2)(a < b); break;
    case 2: m = (VS2)(a <= b); break;
    default: m = (VS2)(a != a) | (VS2)(b != b); break;
    }
    if (p & 4) m = ~m;
    return (FxrV)m;
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
FXI_INLINE FxrV xcvtps(FxrV s, int t) {
    VF v = (VF)s;
    VS4 r = { xcvt32(v[0], t), xcvt32(v[1], t), xcvt32(v[2], t), xcvt32(v[3], t) };
    return (FxrV)r;
}
FXI_INLINE FxrV xcvtpd(FxrV s, int t) { VD v = (VD)s; VS4 r = { xcvt32(v[0], t), xcvt32(v[1], t), 0, 0 }; return (FxrV)r; }
FXI_INLINE uint64_t xmovmskb(FxrV s) {
    VU16 m = (VU16)((VS16)s < (VS16){ 0 }) & (VU16){ 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
#if defined(__aarch64__)
    return (uint64_t)vaddv_u8(vget_low_u8((uint8x16_t)m)) | (uint64_t)vaddv_u8(vget_high_u8((uint8x16_t)m)) << 8;
#else
    uint64_t r = 0;
    for (int i = 0; i < 16; i++) r |= (uint64_t)(m[i] != 0) << i;
    return r;
#endif
}
FXI_INLINE uint64_t xmovmskps(FxrV s) { VU4 v = (VU4)s >> 31; return v[0] | v[1] << 1 | v[2] << 2 | v[3] << 3; }
FXI_INLINE uint64_t xmovmskpd(FxrV s) { return (s[0] >> 63) | (s[1] >> 63) << 1; }

// ---- two-operand ops: D = op(D, S) ----
#define XO_addps(D, S) ((FxrV)((VF)(D) + (VF)(S)))
#define XO_subps(D, S) ((FxrV)((VF)(D) - (VF)(S)))
#define XO_mulps(D, S) ((FxrV)((VF)(D) * (VF)(S)))
#define XO_divps(D, S) ((FxrV)((VF)(D) / (VF)(S)))
#define XO_minps(D, S) XSEL((VF)(D) < (VF)(S), D, S)   // NaN or equal: the source, as x86
#define XO_maxps(D, S) XSEL((VF)(D) > (VF)(S), D, S)
#define XO_sqrtps(D, S) xsqrtps(S)
#define XO_addpd(D, S) ((FxrV)((VD)(D) + (VD)(S)))
#define XO_subpd(D, S) ((FxrV)((VD)(D) - (VD)(S)))
#define XO_mulpd(D, S) ((FxrV)((VD)(D) * (VD)(S)))
#define XO_divpd(D, S) ((FxrV)((VD)(D) / (VD)(S)))
#define XO_minpd(D, S) XSEL((VD)(D) < (VD)(S), D, S)
#define XO_maxpd(D, S) XSEL((VD)(D) > (VD)(S), D, S)
#define XO_sqrtpd(D, S) xsqrtpd(S)
#define XO_addss(D, S) xlo_f(D, LF0(D) + LF0(S))
#define XO_subss(D, S) xlo_f(D, LF0(D) - LF0(S))
#define XO_mulss(D, S) xlo_f(D, LF0(D) * LF0(S))
#define XO_divss(D, S) xlo_f(D, LF0(D) / LF0(S))
#define XO_minss(D, S) xlo_f(D, LF0(D) < LF0(S) ? LF0(D) : LF0(S))
#define XO_maxss(D, S) xlo_f(D, LF0(D) > LF0(S) ? LF0(D) : LF0(S))
#define XO_sqrtss(D, S) xlo_f(D, __builtin_sqrtf(LF0(S)))
#define XO_addsd(D, S) xlo_d(D, LD0(D) + LD0(S))
#define XO_subsd(D, S) xlo_d(D, LD0(D) - LD0(S))
#define XO_mulsd(D, S) xlo_d(D, LD0(D) * LD0(S))
#define XO_divsd(D, S) xlo_d(D, LD0(D) / LD0(S))
#define XO_minsd(D, S) xlo_d(D, LD0(D) < LD0(S) ? LD0(D) : LD0(S))
#define XO_maxsd(D, S) xlo_d(D, LD0(D) > LD0(S) ? LD0(D) : LD0(S))
#define XO_sqrtsd(D, S) xlo_d(D, __builtin_sqrt(LD0(S)))
#define XO_rcpps(D, S) ((FxrV)((VF){ 1.0f, 1.0f, 1.0f, 1.0f } / (VF)(S)))   // FXI: exact, not an estimate
#define XO_rsqrtps(D, S) ((FxrV)((VF){ 1.0f, 1.0f, 1.0f, 1.0f } / (VF)xsqrtps(S)))
#define XO_rcpss(D, S) xlo_f(D, 1.0f / LF0(S))
#define XO_rsqrtss(D, S) xlo_f(D, 1.0f / __builtin_sqrtf(LF0(S)))
#define XO_pand(D, S) ((D) & (S))
#define XO_pandn(D, S) (~(D) & (S))
#define XO_por(D, S) ((D) | (S))
#define XO_pxor(D, S) ((D) ^ (S))
#define XO_paddb(D, S) ((FxrV)((VU16)(D) + (VU16)(S)))
#define XO_paddw(D, S) ((FxrV)((VU8)(D) + (VU8)(S)))
#define XO_paddd(D, S) ((FxrV)((VU4)(D) + (VU4)(S)))
#define XO_paddq(D, S) ((D) + (S))
#define XO_psubb(D, S) ((FxrV)((VU16)(D) - (VU16)(S)))
#define XO_psubw(D, S) ((FxrV)((VU8)(D) - (VU8)(S)))
#define XO_psubd(D, S) ((FxrV)((VU4)(D) - (VU4)(S)))
#define XO_psubq(D, S) ((D) - (S))
#define XO_pcmpeqb(D, S) ((FxrV)((VU16)(D) == (VU16)(S)))
#define XO_pcmpeqw(D, S) ((FxrV)((VU8)(D) == (VU8)(S)))
#define XO_pcmpeqd(D, S) ((FxrV)((VU4)(D) == (VU4)(S)))
#define XO_pcmpgtb(D, S) ((FxrV)((VS16)(D) > (VS16)(S)))
#define XO_pcmpgtw(D, S) ((FxrV)((VS8)(D) > (VS8)(S)))
#define XO_pcmpgtd(D, S) ((FxrV)((VS4)(D) > (VS4)(S)))
#define XO_pmuludq(D, S) (((D) & 0xffffffffull) * ((S) & 0xffffffffull))
#define XO_pmullw(D, S) ((FxrV)((VU8)(D) * (VU8)(S)))
#define XO_pmaxub(D, S) XSEL((VU16)(D) > (VU16)(S), D, S)
#define XO_pminub(D, S) XSEL((VU16)(D) < (VU16)(S), D, S)
#define XO_pmaxsw(D, S) XSEL((VS8)(D) > (VS8)(S), D, S)
#define XO_pminsw(D, S) XSEL((VS8)(D) < (VS8)(S), D, S)
#define XO_punpcklbw(D, S) ((FxrV)__builtin_shufflevector((VU16)(D), (VU16)(S), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23))
#define XO_punpckhbw(D, S) ((FxrV)__builtin_shufflevector((VU16)(D), (VU16)(S), 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31))
#define XO_punpcklwd(D, S) ((FxrV)__builtin_shufflevector((VU8)(D), (VU8)(S), 0, 8, 1, 9, 2, 10, 3, 11))
#define XO_punpckhwd(D, S) ((FxrV)__builtin_shufflevector((VU8)(D), (VU8)(S), 4, 12, 5, 13, 6, 14, 7, 15))
#define XO_punpckldq(D, S) ((FxrV)__builtin_shufflevector((VU4)(D), (VU4)(S), 0, 4, 1, 5))
#define XO_punpckhdq(D, S) ((FxrV)__builtin_shufflevector((VU4)(D), (VU4)(S), 2, 6, 3, 7))
#define XO_punpcklqdq(D, S) __builtin_shufflevector((D), (S), 0, 2)
#define XO_punpckhqdq(D, S) __builtin_shufflevector((D), (S), 1, 3)
#define XO_pshufd(D, S) xtbl1(S, XIDX)
#define XO_pshuflw(D, S) xtbl1(S, XIDX)
#define XO_pshufhw(D, S) xtbl1(S, XIDX)
#define XO_shufps(D, S) xtbl2(D, S, XIDX)
#define XO_shufpd(D, S) xtbl2(D, S, XIDX)
#define XO_cmpps(D, S) xcmpps(D, S, (unsigned)u->imm)
#define XO_cmppd(D, S) xcmppd(D, S, (unsigned)u->imm)
#define XO_cmpss(D, S) xlo_u32(D, ((VU4)xcmpps(D, S, (unsigned)u->imm))[0])
#define XO_cmpsd(D, S) ((FxrV){ xcmppd(D, S, (unsigned)u->imm)[0], (D)[1] })
#define XO_cvtss2sd(D, S) xlo_d(D, (double)LF0(S))
#define XO_cvtsd2ss(D, S) xlo_f(D, (float)LD0(S))
#define XO_cvtdq2ps(D, S) ((FxrV)__builtin_convertvector((VS4)(S), VF))
#define XO_cvttps2dq(D, S) xcvtps(S, 1)
#define XO_cvtps2dq(D, S) xcvtps(S, 0)
#define XO_cvtdq2pd(D, S) ((FxrV)(VD){ (double)((VS4)(S))[0], (double)((VS4)(S))[1] })
#define XO_cvttpd2dq(D, S) xcvtpd(S, 1)
#define XO_cvtpd2dq(D, S) xcvtpd(S, 0)
#define XO_cvtpd2ps(D, S) ((FxrV)(VF){ (float)((VD)(S))[0], (float)((VD)(S))[1], 0.0f, 0.0f })
#define XO_cvtps2pd(D, S) ((FxrV)(VD){ (double)((VF)(S))[0], (double)((VF)(S))[1] })
#define XO_movx(D, S) (S)
// Register-only moves (their memory forms load or store and are handled apart)
#define XO_movss(D, S) ((FxrV)__builtin_shufflevector((VU4)(D), (VU4)(S), 4, 1, 2, 3))
#define XO_movsd(D, S) __builtin_shufflevector((D), (S), 2, 1)
#define XO_movq(D, S) ((FxrV){ (S)[0], 0 })
#define XO_movhlps(D, S) __builtin_shufflevector((D), (S), 3, 1)
#define XO_movlhps(D, S) __builtin_shufflevector((D), (S), 0, 2)

// FXI's name and memory width, for every op with a register form and a memory form (T)
#define XLIST(X) \
    X(addps, 16) X(subps, 16) X(mulps, 16) X(divps, 16) X(minps, 16) X(maxps, 16) X(sqrtps, 16) \
    X(addpd, 16) X(subpd, 16) X(mulpd, 16) X(divpd, 16) X(minpd, 16) X(maxpd, 16) X(sqrtpd, 16) \
    X(addss, 4) X(subss, 4) X(mulss, 4) X(divss, 4) X(minss, 4) X(maxss, 4) X(sqrtss, 4) \
    X(addsd, 8) X(subsd, 8) X(mulsd, 8) X(divsd, 8) X(minsd, 8) X(maxsd, 8) X(sqrtsd, 8) \
    X(rcpps, 16) X(rsqrtps, 16) X(rcpss, 4) X(rsqrtss, 4) X(pand, 16) X(pandn, 16) X(por, 16) X(pxor, 16) \
    X(paddb, 16) X(paddw, 16) X(paddd, 16) X(paddq, 16) X(psubb, 16) X(psubw, 16) X(psubd, 16) X(psubq, 16) \
    X(pcmpeqb, 16) X(pcmpeqw, 16) X(pcmpeqd, 16) X(pcmpgtb, 16) X(pcmpgtw, 16) X(pcmpgtd, 16) \
    X(pmuludq, 16) X(pmullw, 16) X(pmaxub, 16) X(pminub, 16) X(pmaxsw, 16) X(pminsw, 16) \
    X(punpcklbw, 16) X(punpckhbw, 16) X(punpcklwd, 16) X(punpckhwd, 16) X(punpckldq, 16) X(punpckhdq, 16) \
    X(punpcklqdq, 16) X(punpckhqdq, 16) X(pshufd, 16) X(pshuflw, 16) X(pshufhw, 16) X(shufps, 16) X(shufpd, 16) \
    X(cmpps, 16) X(cmppd, 16) X(cmpss, 4) X(cmpsd, 8) \
    X(cvtss2sd, 4) X(cvtsd2ss, 8) X(cvtdq2ps, 16) X(cvttps2dq, 16) X(cvtps2dq, 16) X(cvtdq2pd, 8) \
    X(cvttpd2dq, 16) X(cvtpd2dq, 16) X(cvtpd2ps, 16) X(cvtps2pd, 8) X(movx, 16)
#define XRLIST(X) X(movss) X(movsd) X(movq) X(movhlps) X(movlhps)

#define DEF_XRR(S, D, OP) PH xr_##OP##_##D##_##S(FXR_PARAMS) { FxrV d_ = XD_##D, s_ = XS_##S; (void)d_; XW_##D(XO_##OP(d_, s_)); PNEXT(); }
#define DEF_XRR_ROW(D, OP) X9B(DEF_XRR, D, OP)
#define DEF_XRT(D, OP, W) PH xt_##OP##_##D(FXR_PARAMS) { FxrV d_ = XD_##D, s_ = xldn(T, W); (void)d_; XW_##D(XO_##OP(d_, s_)); PNEXT(); }
#define DEF_XOP(OP, W) X9(DEF_XRR_ROW, OP) X9(DEF_XRT, OP, W)
XLIST(DEF_XOP)
#define DEF_XRONLY(OP) X9(DEF_XRR_ROW, OP)
XRLIST(DEF_XRONLY)
// (u)comiss / (u)comisd: ZF PF CF, the others clear, as raw lazy flags
#define XCOMIS(F, a, b) do { double a_ = (a), b_ = (b);                                    \
        F0 = (uint64_t)LF(LF_RAW, 3) | (uint64_t)((a_ != a_ || b_ != b_) ? 0x45 : a_ < b_ ? 0x01 : a_ == b_ ? 0x40 : 0) << 32; \
    } while (0)
#define DEF_XCOM(S, D)                                                                     \
    PH xr_comiss_##D##_##S(FXR_PARAMS) { XCOMIS(F0, LF0(XD_##D), LF0(XS_##S)); PNEXT(); }  \
    PH xr_comisd_##D##_##S(FXR_PARAMS) { XCOMIS(F0, LD0(XD_##D), LD0(XS_##S)); PNEXT(); }
#define DEF_XCOM_ROW(D, _) X9B(DEF_XCOM, D)                                                \
    PH xt_comiss_##D(FXR_PARAMS) { XCOMIS(F0, LF0(XD_##D), LF0(xldn(T, 4))); PNEXT(); }    \
    PH xt_comisd_##D(FXR_PARAMS) { XCOMIS(F0, LD0(XD_##D), LD0(xldn(T, 8))); PNEXT(); }
X9(DEF_XCOM_ROW, _)

typedef struct { const char *name; PFn rr[9][9]; PFn rt[9]; } XOp;
#define E_XRR(S, D, OP) xr_##OP##_##D##_##S,
#define ROW_XRR(D, OP) { X9B(E_XRR, D, OP) },
#define E_XRT(D, OP) xt_##OP##_##D,
#define XOP_ENTRY(OP, W) { #OP, { X9(ROW_XRR, OP) }, { X9(E_XRT, OP) } },
#define XROP_ENTRY(OP) { #OP, { X9(ROW_XRR, OP) }, { 0 } },
static const XOp k_xops[] = { XLIST(XOP_ENTRY) XRLIST(XROP_ENTRY) XOP_ENTRY(comiss, 4) XOP_ENTRY(comisd, 8) };
#define N_XOPS (sizeof k_xops / sizeof k_xops[0])

// ---- shifts by an immediate (register only); the byte shifts use a tbl index ----
#define XI_psllw(D, n) ((n) > 15 ? (FxrV){ 0, 0 } : (FxrV)((VU8)(D) << (int)(n)))
#define XI_pslld(D, n) ((n) > 31 ? (FxrV){ 0, 0 } : (FxrV)((VU4)(D) << (int)(n)))
#define XI_psllq(D, n) ((n) > 63 ? (FxrV){ 0, 0 } : (FxrV)((D) << (int)(n)))
#define XI_psrlw(D, n) ((n) > 15 ? (FxrV){ 0, 0 } : (FxrV)((VU8)(D) >> (int)(n)))
#define XI_psrld(D, n) ((n) > 31 ? (FxrV){ 0, 0 } : (FxrV)((VU4)(D) >> (int)(n)))
#define XI_psrlq(D, n) ((n) > 63 ? (FxrV){ 0, 0 } : (FxrV)((D) >> (int)(n)))
#define XI_psraw(D, n) ((FxrV)((VS8)(D) >> (int)((n) > 15 ? 15 : (n))))
#define XI_psrad(D, n) ((FxrV)((VS4)(D) >> (int)((n) > 31 ? 31 : (n))))
#define XI_pslldq(D, n) xtbl1(D, XIDX)
#define XI_psrldq(D, n) xtbl1(D, XIDX)
#define XILIST(X) X(psllw) X(pslld) X(psllq) X(psrlw) X(psrld) X(psrlq) X(psraw) X(psrad) X(pslldq) X(psrldq)
#define DEF_XI(D, OP) PH xi_##OP##_##D(FXR_PARAMS) { unsigned n_ = (unsigned)u->imm; (void)n_; XW_##D(XI_##OP(XD_##D, n_)); PNEXT(); }
#define DEF_XI_ALL(OP) X9(DEF_XI, OP)
XILIST(DEF_XI_ALL)
#define E_XI(D, OP) xi_##OP##_##D,
#define XI_ENTRY(OP) { #OP, { X9(E_XI, OP) } },
static const struct { const char *name; PFn h[9]; } k_xshift[] = { XILIST(XI_ENTRY) };

// ---- loads and stores. 16 bytes with [base + index*scale + disp] in one uop; movss/movsd/movq
// with [base + disp] (else the address comes through T) ----
#define XEA(B, I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_XLS(I, B, X)                                                                   \
    PH xl_movx_##B##_##I##_##X(FXR_PARAMS) { XW_##X(xld((const void *)(uintptr_t)XEA(B, I))); PNEXT(); } \
    PH xs_movx_##B##_##I##_##X(FXR_PARAMS) { xst((void *)(uintptr_t)XEA(B, I), XS_##X); PNEXT(); }
#define DEF_XLS_B(B, X) R17B(DEF_XLS, B, X)
#define DEF_XLS_X(X, _) R17(DEF_XLS_B, X)
X9(DEF_XLS_X, _)
#define DEF_XLS1(B, X)                                                                     \
    PH xl_movss_##B##_##X(FXR_PARAMS) { XW_##X(((FxrV){ ld32(g##B + (uint64_t)u->disp), 0 })); PNEXT(); } \
    PH xl_movsd_##B##_##X(FXR_PARAMS) { XW_##X(((FxrV){ ld64(g##B + (uint64_t)u->disp), 0 })); PNEXT(); } \
    PH xs_movss_##B##_##X(FXR_PARAMS) { st32(g##B + (uint64_t)u->disp, ((VU4)XS_##X)[0]); PNEXT(); } \
    PH xs_movsd_##B##_##X(FXR_PARAMS) { st64(g##B + (uint64_t)u->disp, (XS_##X)[0]); PNEXT(); }
#define DEF_XLS1_X(X, _) R17(DEF_XLS1, X)
X9(DEF_XLS1_X, _)
#define DEF_XLST(X, _)                                                                     \
    PH xlt_movss_##X(FXR_PARAMS) { XW_##X(((FxrV){ ld32(T), 0 })); PNEXT(); }              \
    PH xlt_movsd_##X(FXR_PARAMS) { XW_##X(((FxrV){ ld64(T), 0 })); PNEXT(); }              \
    PH xlt_movlps_##X(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(((FxrV){ ld64(T), d_[1] })); PNEXT(); } \
    PH xlt_movhps_##X(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(((FxrV){ d_[0], ld64(T) })); PNEXT(); } \
    PH xst_movss_##X(FXR_PARAMS) { st32(T, ((VU4)XS_##X)[0]); PNEXT(); }                  \
    PH xst_movsd_##X(FXR_PARAMS) { st64(T, (XS_##X)[0]); PNEXT(); }                        \
    PH xst_movhps_##X(FXR_PARAMS) { st64(T, (XS_##X)[1]); PNEXT(); }                       \
    PH xst_movx_##X(FXR_PARAMS) { xst((void *)(uintptr_t)T, XS_##X); PNEXT(); }
X9(DEF_XLST, _)
#define E_XLS(I, B, X, NAME) NAME##_##B##_##I##_##X,
#define ROW_XLS(B, X, NAME) { R17B(E_XLS, B, X, NAME) },
#define X_XLS(X, NAME) { R17(ROW_XLS, X, NAME) },
static const PFn t_xl_movx[9][17][17] = { X9(X_XLS, xl_movx) }, t_xs_movx[9][17][17] = { X9(X_XLS, xs_movx) };   // [xmm][base][index]
#define E_XLS1(B, X, NAME) NAME##_##B##_##X,
#define X_XLS1(X, NAME) { R17(E_XLS1, X, NAME) },
static const PFn t_xl_movss[9][17] = { X9(X_XLS1, xl_movss) }, t_xl_movsd[9][17] = { X9(X_XLS1, xl_movsd) },
    t_xs_movss[9][17] = { X9(X_XLS1, xs_movss) }, t_xs_movsd[9][17] = { X9(X_XLS1, xs_movsd) };
#define E_X1(X, NAME) NAME##_##X,
static const PFn t_xlt_movss[9] = { X9(E_X1, xlt_movss) }, t_xlt_movsd[9] = { X9(E_X1, xlt_movsd) },
    t_xlt_movlps[9] = { X9(E_X1, xlt_movlps) }, t_xlt_movhps[9] = { X9(E_X1, xlt_movhps) },
    t_xst_movss[9] = { X9(E_X1, xst_movss) }, t_xst_movsd[9] = { X9(E_X1, xst_movsd) },
    t_xst_movhps[9] = { X9(E_X1, xst_movhps) }, t_xst_movx[9] = { X9(E_X1, xst_movx) };

// ---- between general registers and XMM ----
#define DEF_XG(G, X)                                                                       \
    PH xg_cvtsi2ss32_##X##_##G(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_f(d_, (float)(int32_t)g##G)); PNEXT(); } \
    PH xg_cvtsi2ss64_##X##_##G(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_f(d_, (float)(int64_t)g##G)); PNEXT(); } \
    PH xg_cvtsi2sd32_##X##_##G(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_d(d_, (double)(int32_t)g##G)); PNEXT(); } \
    PH xg_cvtsi2sd64_##X##_##G(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_d(d_, (double)(int64_t)g##G)); PNEXT(); } \
    PH xg_movd32_##X##_##G(FXR_PARAMS) { XW_##X(((FxrV){ g##G & M32, 0 })); PNEXT(); }    \
    PH xg_movd64_##X##_##G(FXR_PARAMS) { XW_##X(((FxrV){ g##G, 0 })); PNEXT(); }          \
    PH gx_cvttss2si32_##G##_##X(FXR_PARAMS) { g##G = (uint32_t)xcvt32(LF0(XS_##X), 1); PNEXT(); } \
    PH gx_cvtss2si32_##G##_##X(FXR_PARAMS) { g##G = (uint32_t)xcvt32(LF0(XS_##X), 0); PNEXT(); } \
    PH gx_cvttsd2si32_##G##_##X(FXR_PARAMS) { g##G = (uint32_t)xcvt32(LD0(XS_##X), 1); PNEXT(); } \
    PH gx_cvtsd2si32_##G##_##X(FXR_PARAMS) { g##G = (uint32_t)xcvt32(LD0(XS_##X), 0); PNEXT(); } \
    PH gx_cvttss2si64_##G##_##X(FXR_PARAMS) { g##G = (uint64_t)xcvt64(LF0(XS_##X), 1); PNEXT(); } \
    PH gx_cvtss2si64_##G##_##X(FXR_PARAMS) { g##G = (uint64_t)xcvt64(LF0(XS_##X), 0); PNEXT(); } \
    PH gx_cvttsd2si64_##G##_##X(FXR_PARAMS) { g##G = (uint64_t)xcvt64(LD0(XS_##X), 1); PNEXT(); } \
    PH gx_cvtsd2si64_##G##_##X(FXR_PARAMS) { g##G = (uint64_t)xcvt64(LD0(XS_##X), 0); PNEXT(); } \
    PH gx_movd32_##G##_##X(FXR_PARAMS) { g##G = ((VU4)XS_##X)[0]; PNEXT(); }              \
    PH gx_movd64_##G##_##X(FXR_PARAMS) { g##G = (XS_##X)[0]; PNEXT(); }                    \
    PH gx_pmovmskb_##G##_##X(FXR_PARAMS) { g##G = xmovmskb(XS_##X); PNEXT(); }            \
    PH gx_movmskps_##G##_##X(FXR_PARAMS) { g##G = xmovmskps(XS_##X); PNEXT(); }           \
    PH gx_movmskpd_##G##_##X(FXR_PARAMS) { g##G = xmovmskpd(XS_##X); PNEXT(); }
#define DEF_XG_X(X, _) R16(DEF_XG, X)
X9(DEF_XG_X, _)
// with the integer operand in memory (T)
#define DEF_XGT(X, _)                                                                      \
    PH xgt_cvtsi2ss32_##X(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_f(d_, (float)(int32_t)ld32(T))); PNEXT(); } \
    PH xgt_cvtsi2ss64_##X(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_f(d_, (float)(int64_t)ld64(T))); PNEXT(); } \
    PH xgt_cvtsi2sd32_##X(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_d(d_, (double)(int32_t)ld32(T))); PNEXT(); } \
    PH xgt_cvtsi2sd64_##X(FXR_PARAMS) { FxrV d_ = XD_##X; XW_##X(xlo_d(d_, (double)(int64_t)ld64(T))); PNEXT(); }
X9(DEF_XGT, _)
enum { XG_SI2SS32, XG_SI2SS64, XG_SI2SD32, XG_SI2SD64, XG_MOVD32, XG_MOVD64, XG_COUNT };
enum { GX_TSS32, GX_SS32, GX_TSD32, GX_SD32, GX_TSS64, GX_SS64, GX_TSD64, GX_SD64, GX_MOVD32, GX_MOVD64,
       GX_PMOVMSKB, GX_MOVMSKPS, GX_MOVMSKPD, GX_COUNT };
#define E_XG(G, X, NAME) xg_##NAME##_##X##_##G,
#define ROW_XG(X, NAME) { R16(E_XG, X, NAME) },
#define T_XG(NAME) { X9(ROW_XG, NAME) },
static const PFn t_xg[XG_COUNT][9][16] = { T_XG(cvtsi2ss32) T_XG(cvtsi2ss64) T_XG(cvtsi2sd32) T_XG(cvtsi2sd64) T_XG(movd32) T_XG(movd64) };
#define E_GX(G, X, NAME) gx_##NAME##_##G##_##X,
#define ROW_GX(X, NAME) { R16(E_GX, X, NAME) },
#define T_GX(NAME) { X9(ROW_GX, NAME) },
static const PFn t_gx[GX_COUNT][9][16] = {   // [kind][xmm][gpr]
    T_GX(cvttss2si32) T_GX(cvtss2si32) T_GX(cvttsd2si32) T_GX(cvtsd2si32) T_GX(cvttss2si64) T_GX(cvtss2si64)
    T_GX(cvttsd2si64) T_GX(cvtsd2si64) T_GX(movd32) T_GX(movd64) T_GX(pmovmskb) T_GX(movmskps) T_GX(movmskpd) };
static const PFn t_xgt[4][9] = { { X9(E_X1, xgt_cvtsi2ss32) }, { X9(E_X1, xgt_cvtsi2ss64) },
                                 { X9(E_X1, xgt_cvtsi2sd32) }, { X9(E_X1, xgt_cvtsi2sd64) } };
static int xcls(unsigned r) { return r < 8 ? (int)r : 8; }

// Byte indices for the tbl-based shuffles, from the instruction's immediate
static void xshuffle_index(const char *op, unsigned imm, uint8_t *x) {
    if (!strcmp(op, "pshufd"))
        for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) x[4 * i + k] = (uint8_t)(4 * ((imm >> (2 * i)) & 3) + k);
    else if (!strcmp(op, "pshuflw")) {
        for (int i = 0; i < 4; i++) for (int k = 0; k < 2; k++) x[2 * i + k] = (uint8_t)(2 * ((imm >> (2 * i)) & 3) + k);
        for (int i = 8; i < 16; i++) x[i] = (uint8_t)i;
    } else if (!strcmp(op, "pshufhw")) {
        for (int i = 0; i < 8; i++) x[i] = (uint8_t)i;
        for (int i = 0; i < 4; i++) for (int k = 0; k < 2; k++) x[8 + 2 * i + k] = (uint8_t)(8 + 2 * ((imm >> (2 * i)) & 3) + k);
    } else if (!strcmp(op, "shufps"))
        for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) x[4 * i + k] = (uint8_t)((i >= 2 ? 16 : 0) + 4 * ((imm >> (2 * i)) & 3) + k);
    else if (!strcmp(op, "shufpd"))
        for (int k = 0; k < 8; k++) { x[k] = (uint8_t)(8 * (imm & 1) + k); x[8 + k] = (uint8_t)(16 + 8 * ((imm >> 1) & 1) + k); }
    else if (!strcmp(op, "psrldq"))
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)(i + imm < 16 ? i + imm : 0xff);
    else if (!strcmp(op, "pslldq"))
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)((unsigned)i >= imm ? i - imm : 0xff);
}

// ===========================================================================
// Lowering: FXI's decoded uops -> pinned handlers
// ===========================================================================
enum { FAM_NONE, FAM_ALU, FAM_MOV, FAM_LEA, FAM_SHIFT, FAM_UNARY, FAM_EXT, FAM_CMOV, FAM_SETCC, FAM_IMUL2,
       FAM_IMUL3, FAM_FJCC, FAM_NAMED, FAM_X, FAM_XI, FAM_XS };
enum { N_JMP, N_JCC, N_CALL, N_CALL_R, N_CALL_M, N_JMP_R, N_JMP_M, N_RET, N_GOTO, N_SYSCALL, N_STOP, N_PUSH_R,
       N_POP_R, N_NOP };
// FAM_XS: SSE loads/stores and the ops between general registers and XMM
enum { XS_MOVX_MR, XS_MOVSS_RM, XS_MOVSD_RM, XS_MOVSS_MR, XS_MOVSD_MR, XS_MOVLPS_RM, XS_MOVHPS_RM, XS_MOVHPS_MR,
       XS_XG = 16, XS_XGM = 32, XS_GX = 48 };
typedef struct { OpFn key; uint8_t fam, a, b, c2, d; } Desc;
#define DESC_SLOTS 16384
static Desc g_desc[DESC_SLOTS];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint64_t hptr(OpFn f) { return ((uint64_t)(uintptr_t)f * 0x9E3779B97F4A7C15ull) >> 50; }
static void put(OpFn f, uint8_t fam, uint8_t a, uint8_t b, uint8_t c2, uint8_t d) {
    if (!f) return;
    uint64_t h = hptr(f) & (DESC_SLOTS - 1);
    while (g_desc[h].key && g_desc[h].key != f) h = (h + 1) & (DESC_SLOTS - 1);
    g_desc[h] = (Desc){ f, fam, a, b, c2, d };
}
static const Desc *find(OpFn f) {
    uint64_t h = hptr(f) & (DESC_SLOTS - 1);
    while (g_desc[h].key) { if (g_desc[h].key == f) return &g_desc[h]; h = (h + 1) & (DESC_SLOTS - 1); }
    return 0;
}
static int is_shuffle(const char *n) {
    return !strcmp(n, "pshufd") || !strcmp(n, "pshuflw") || !strcmp(n, "pshufhw") || !strcmp(n, "shufps") ||
           !strcmp(n, "shufpd") || !strcmp(n, "psrldq") || !strcmp(n, "pslldq");
}
static void init_desc(void) {
    for (int cc = 0; cc < 16; cc++)
        for (int i = 0; i < 16; i++)
            k_cctab[cc] |= (uint16_t)(cc_flags((unsigned)cc, (i >> 3) & 1, (i >> 1) & 1, i & 1, (i >> 2) & 1, 0) << i);
    for (int op = 0; op < ALU_COUNT; op++)
        for (int fm = 0; fm < F_COUNT; fm++)
            for (int si = 0; si < 4; si++)
                for (int fl = 0; fl < 2; fl++) put(fxi_alu_tab[op][fm][si][fl], FAM_ALU, op, fm, si, 0);
    for (int fm = 0; fm < F_COUNT; fm++) for (int si = 0; si < 4; si++) put(fxi_mov_tab[fm][si], FAM_MOV, fm, si, 0, 0);
    for (int si = 1; si < 4; si++) put(fxi_lea_tab[si], FAM_LEA, si, 0, 0, 0);
    for (int op = 0; op < 8; op++) for (int rm = 0; rm < 2; rm++) for (int si = 0; si < 4; si++)
        put(fxi_shift_tab[op][rm][si], FAM_SHIFT, op, rm, si, 0);
    for (int op = 0; op < 4; op++) for (int rm = 0; rm < 2; rm++) for (int si = 0; si < 4; si++)
        put(fxi_unary_tab[op][rm][si], FAM_UNARY, op, rm, si, 0);
    for (int sg = 0; sg < 2; sg++) for (int rm = 0; rm < 2; rm++) for (int ss = 0; ss < 3; ss++) for (int ds = 1; ds < 4; ds++)
        put(fxi_ext_tab[sg][rm][ss][ds], FAM_EXT, sg, rm, ss, ds);
    for (int rm = 0; rm < 2; rm++) for (int si = 1; si < 4; si++) {
        put(fxi_cmov_tab[rm][si], FAM_CMOV, rm, si, 0, 0);
        put(fxi_imul2_tab[rm][si], FAM_IMUL2, rm, si, 0, 0);
        put(fxi_imul3_tab[rm][si], FAM_IMUL3, rm, si, 0, 0);
    }
    put(fxi_setcc_tab[0], FAM_SETCC, 0, 0, 0, 0);
    for (int t = 0; t < 2; t++) for (int ri = 0; ri < 2; ri++) for (int s = 0; s < 2; s++) for (int cc = 0; cc < 16; cc++)
        put(fxi_fjcc_tab[t][ri][s][cc], FAM_FJCC, t, ri, s, cc);
    static const struct { const char *n; int id; } named[] = {
        { "jmp", N_JMP }, { "jcc", N_JCC }, { "call", N_CALL }, { "call_R", N_CALL_R }, { "call_M", N_CALL_M },
        { "jmp_R", N_JMP_R }, { "jmp_M", N_JMP_M }, { "ret", N_RET }, { "goto", N_GOTO }, { "syscall", N_SYSCALL },
        { "stop", N_STOP }, { "push_R", N_PUSH_R }, { "pop_R", N_POP_R }, { "nop", N_NOP },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) put(fxi_named(named[i].n), FAM_NAMED, (uint8_t)named[i].id, 0, 0, 0);
    // SSE: two-operand ops (register form; memory form when the op has one), c2 = shuffle, d = comis
    char nm[48];
    for (size_t i = 0; i < N_XOPS; i++) {
        const char *n = k_xops[i].name;
        int shuf = is_shuffle(n), com = !strcmp(n, "comiss") || !strcmp(n, "comisd");
        snprintf(nm, sizeof nm, "%s_RR", n); put(fxi_named(nm), FAM_X, (uint8_t)i, 0, (uint8_t)shuf, (uint8_t)com);
        if (k_xops[i].rt[0]) { snprintf(nm, sizeof nm, "%s_RM", n); put(fxi_named(nm), FAM_X, (uint8_t)i, 1, (uint8_t)shuf, (uint8_t)com); }
    }
    for (size_t i = 0; i < sizeof k_xshift / sizeof k_xshift[0]; i++) {
        snprintf(nm, sizeof nm, "%s_RI", k_xshift[i].name);
        put(fxi_named(nm), FAM_XI, (uint8_t)i, 0, (uint8_t)is_shuffle(k_xshift[i].name), 0);
    }
    static const struct { const char *n; int id; } xnamed[] = {
        { "movx_MR", XS_MOVX_MR }, { "movss_RM", XS_MOVSS_RM }, { "movsd_RM", XS_MOVSD_RM }, { "movss_MR", XS_MOVSS_MR },
        { "movsd_MR", XS_MOVSD_MR }, { "movlps_RM", XS_MOVLPS_RM }, { "movhps_RM", XS_MOVHPS_RM }, { "movhps_MR", XS_MOVHPS_MR },
        { "cvtsi2ss_R32", XS_XG + XG_SI2SS32 }, { "cvtsi2ss_R64", XS_XG + XG_SI2SS64 },
        { "cvtsi2sd_R32", XS_XG + XG_SI2SD32 }, { "cvtsi2sd_R64", XS_XG + XG_SI2SD64 },
        { "movd_XR32", XS_XG + XG_MOVD32 }, { "movd_XR64", XS_XG + XG_MOVD64 },
        { "cvtsi2ss_M32", XS_XGM + 0 }, { "cvtsi2ss_M64", XS_XGM + 1 }, { "cvtsi2sd_M32", XS_XGM + 2 }, { "cvtsi2sd_M64", XS_XGM + 3 },
        { "cvttss2si_R32", XS_GX + GX_TSS32 }, { "cvtss2si_R32", XS_GX + GX_SS32 },
        { "cvttsd2si_R32", XS_GX + GX_TSD32 }, { "cvtsd2si_R32", XS_GX + GX_SD32 },
        { "cvttss2si_R64", XS_GX + GX_TSS64 }, { "cvtss2si_R64", XS_GX + GX_SS64 },
        { "cvttsd2si_R64", XS_GX + GX_TSD64 }, { "cvtsd2si_R64", XS_GX + GX_SD64 },
        { "movd_RX32", XS_GX + GX_MOVD32 }, { "movd_RX64", XS_GX + GX_MOVD64 },
        { "pmovmskb_RR", XS_GX + GX_PMOVMSKB }, { "movmskps_RR", XS_GX + GX_MOVMSKPS }, { "movmskpd_RR", XS_GX + GX_MOVMSKPD },
    };
    for (size_t i = 0; i < sizeof xnamed / sizeof xnamed[0]; i++) put(fxi_named(xnamed[i].n), FAM_XS, (uint8_t)xnamed[i].id, 0, 0, 0);
}

static int greg(unsigned off) { return (off & 7) == 0 && off < 128 ? (int)(off >> 3) : -1; }   // pinnable GPR
static int simple_mem(const Uop *u) { return u->index == R_ZERO && u->base <= R_ZERO; }        // [base + disp]
static int ea_ok(const Uop *u) { return u->base <= R_ZERO && u->index <= R_ZERO; }

typedef struct { Uop *out; int n; } Out;
static Uop *put_uop(Out *o, const Uop *src, PFn p) { Uop *x = &o->out[o->n++]; *x = *src; x->p = p; return x; }
// T = EA of src, then the op itself
static Uop *with_ea(Out *o, const Uop *src, PFn p) {
    put_uop(o, src, t_ea[src->base][src->index]);
    return put_uop(o, src, p);
}

static void lower_one(Out *o, const Uop *u) {
    const Desc *d = find(u->fn);
    int D = greg(u->dst), S = greg(u->src), fl = u->flive != 0;
    if (d) switch (d->fam) {
    case FAM_ALU: {
        int op = d->a, fm = d->b, si = d->c2;
        // cmp/test of registers whose flags nothing reads: no effect at all
        if (!fl && (op == ALU_CMP || op == ALU_TEST) && (fm == F_RR || fm == F_RI)) return;
        if (fm == F_RR && si >= 2 && D >= 0 && S >= 0) { put_uop(o, u, t_alu_rr[fl][op][si - 2][D][S]); return; }
        if (fm == F_RI && si >= 2 && D >= 0) { put_uop(o, u, t_alu_ri[fl][op][si - 2][D]); return; }
        if (fm == F_RM && si >= 2 && D >= 0 && ea_ok(u)) { with_ea(o, u, t_alu_rt[fl][op][si - 2][D]); return; }
        if (fm == F_MR && S >= 0 && ea_ok(u)) { with_ea(o, u, t_alu_tr[fl][op][si][S]); return; }
        if (fm == F_MI && ea_ok(u)) { with_ea(o, u, t_alu_ti[fl][op][si]); return; }
        break;
    }
    case FAM_MOV: {
        int fm = d->a, si = d->b;
        if (fm == F_RR && si >= 2 && D >= 0 && S >= 0) { put_uop(o, u, t_mov_rr[si - 2][D][S]); return; }
        if (fm == F_RI && si >= 2 && D >= 0) { put_uop(o, u, t_mov_ri[si - 2][D]); return; }
        if (fm == F_RM && si >= 2 && D >= 0) {
            if (simple_mem(u)) { put_uop(o, u, t_ld[si - 2][u->base][D]); return; }
            if (ea_ok(u)) { with_ea(o, u, t_ldt[si == 3 ? KL64 : KL32][D]); return; }
        }
        if (fm == F_MR && S >= 0) {
            if (simple_mem(u)) { put_uop(o, u, t_st[si][u->base][S]); return; }
            if (ea_ok(u)) { with_ea(o, u, t_stt[si][S]); return; }
        }
        if (fm == F_MI) {
            if (simple_mem(u)) { put_uop(o, u, t_sti[si][u->base]); return; }
            if (ea_ok(u)) { put_uop(o, u, t_sti_bi[si][u->base][u->index]); return; }
        }
        break;
    }
    case FAM_LEA: {
        int si = d->a;
        if (si >= 2 && D >= 0) {
            if (simple_mem(u)) { put_uop(o, u, t_lea[si - 2][u->base][D]); return; }
            if (ea_ok(u)) { with_ea(o, u, t_movt[si - 2][D]); return; }
        }
        break;
    }
    case FAM_EXT: {
        int sg = d->a, rm = d->b, ss = d->c2, ds = d->d, k = -1;
        if (ds < 2) break;
        if (!sg) k = ss == 0 ? KZ8 : ss == 1 ? KZ16 : -1;
        else if (ss == 0) k = ds == 2 ? KS8_32 : KS8_64;
        else if (ss == 1) k = ds == 2 ? KS16_32 : KS16_64;
        else if (ds == 3) k = KS32_64;
        if (k < 0 || D < 0) break;
        if (!rm && S >= 0) { put_uop(o, u, t_ext[k][D][S]); return; }
        if (rm && ea_ok(u)) { with_ea(o, u, t_ldt[k][D]); return; }
        break;
    }
    case FAM_SHIFT: {
        int op = d->a, rm = d->b, si = d->c2, ps = -1;
        if (rm || si < 2 || D < 0) break;
        if (op == SH_SHL || op == SH_SAL) ps = PS_SHL;
        else if (op == SH_SHR) ps = PS_SHR;
        else if (op == SH_SAR) ps = PS_SAR;
        else if (op == SH_ROL && !u->cc) ps = PS_ROL;
        else if (op == SH_ROR && !u->cc) ps = PS_ROR;
        if (ps < 0) break;
        if (u->src == 0xffff) { put_uop(o, u, t_sh[fl][ps][si - 2][1][D]); return; }
        unsigned n = (unsigned)u->imm & (si == 3 ? 63u : 31u);
        if (!n) { put_uop(o, u, si == 2 ? t_mov_rr[0][D][D] : p_nop); return; }   // flags unchanged; a 32-bit register still zero-extends
        put_uop(o, u, t_sh[fl][ps][si - 2][0][D])->imm = n;
        return;
    }
    case FAM_UNARY:
        if (!d->b && d->c2 >= 2 && D >= 0) { put_uop(o, u, t_un[fl][d->c2 - 2][d->a][D]); return; }
        break;
    case FAM_IMUL2: case FAM_IMUL3: {
        int rm = d->a, si = d->b;
        if (si < 2 || D < 0) break;
        const PFn (*rr)[16][16] = d->fam == FAM_IMUL2 ? t_imul2[fl] : t_imul3[fl];
        const PFn (*rt)[16] = d->fam == FAM_IMUL2 ? t_imul2t[fl] : t_imul3t[fl];
        if (!rm && S >= 0) { put_uop(o, u, rr[si - 2][D][S]); return; }
        if (rm && ea_ok(u)) { with_ea(o, u, rt[si - 2][D]); return; }
        break;
    }
    case FAM_CMOV: {
        int rm = d->a, si = d->b;
        if (si < 2 || D < 0 || u->cc == 10 || u->cc == 11) break;   // parity: FXI's handler
        Uop *x = 0;
        if (!rm && S >= 0) x = put_uop(o, u, t_cmov[si - 2][D][S]);
        else if (rm && ea_ok(u)) x = with_ea(o, u, t_cmovt[si - 2][D]);
        if (!x) break;
        x->imm = k_cctab[u->cc & 15];
        return;
    }
    case FAM_SETCC:
        if (D >= 0) { put_uop(o, u, t_setcc[u->cc & 15][D]); return; }
        break;
    case FAM_FJCC: {
        int t = d->a, ri = d->b, s64 = d->c2, cc = d->d;
        if (D < 0 || (!ri && S < 0)) { fprintf(stderr, "fxr: fused branch without register operands\n"); abort(); }
        Uop *x;
        if (ri) x = put_uop(o, u, t ? t_fjt_ri[s64][cc][D] : t_fjc_ri[s64][cc][D]);
        else if (!t) x = put_uop(o, u, t_fjc_rr[s64][cc][D][S]);
        else x = put_uop(o, u, D == S ? t_fjt_rr[s64][cc][D] : t_fjt_rrx[s64][D][S]);
        x->cc = (uint8_t)cc;
        return;
    }
    case FAM_NAMED:
        switch (d->a) {
        case N_JMP: put_uop(o, u, p_jmp); return;
        case N_JCC: put_uop(o, u, t_jcc[u->cc & 15]); return;
        case N_CALL: put_uop(o, u, p_call); return;
        case N_CALL_R: if (S >= 0) { put_uop(o, u, t_call_R[S]); return; } break;
        case N_JMP_R: if (S >= 0) { put_uop(o, u, t_jmp_R[S]); return; } break;
        case N_CALL_M: if (ea_ok(u)) with_ea(o, u, p_call_T); else put_uop(o, u, p_call_M); return;
        case N_JMP_M: if (ea_ok(u)) with_ea(o, u, p_jmp_T); else put_uop(o, u, p_jmp_M); return;
        case N_RET: put_uop(o, u, p_ret); return;
        case N_GOTO: put_uop(o, u, p_goto); return;
        case N_SYSCALL: put_uop(o, u, p_syscall); return;
        case N_STOP: put_uop(o, u, p_stop); return;
        case N_NOP: put_uop(o, u, p_nop); return;
        case N_PUSH_R: if (S >= 0) { put_uop(o, u, t_push[S]); return; } break;
        case N_POP_R: if (D >= 0) { put_uop(o, u, t_pop[D]); return; } break;
        }
        // Control flow has no FXI fallback here (FXI's handlers would continue FXI's own chain).
        if (d->a <= N_STOP) { fprintf(stderr, "fxr: no pinned form for control-flow uop %d\n", d->a); abort(); }
        break;
    case FAM_X: {   // two-operand SSE: dst XMM, src XMM or memory
        const XOp *x = &k_xops[d->a];
        int dc = xcls(u->dst);
        Uop *y;
        if (!d->b) {
            if (d->d && !fl) return;   // (u)comis of registers whose flags nothing reads
            y = put_uop(o, u, x->rr[dc][xcls(u->src)]);
        } else {
            if (!ea_ok(u)) break;      // fs/gs operand: FXI's handler
            if (!strcmp(x->name, "movx")) { put_uop(o, u, t_xl_movx[dc][u->base][u->index]); return; }
            y = with_ea(o, u, x->rt[dc]);
        }
        if (d->c2) { uint8_t ix[16]; xshuffle_index(x->name, (unsigned)u->imm, ix); memcpy(y->xidx, ix, 16); }
        return;
    }
    case FAM_XI: {   // shift by an immediate
        Uop *y = put_uop(o, u, k_xshift[d->a].h[xcls(u->dst)]);
        if (d->c2) { uint8_t ix[16]; xshuffle_index(k_xshift[d->a].name, (unsigned)u->imm, ix); memcpy(y->xidx, ix, 16); }
        return;
    }
    case FAM_XS: {   // loads, stores, general register <-> XMM
        int id = d->a, dc = xcls(u->dst), sc = xcls(u->src);
        if (id >= XS_GX) {   // GPR <- XMM: dst is a GPR byte offset, src an XMM
            if (D < 0) break;
            put_uop(o, u, t_gx[id - XS_GX][sc][D]);
            return;
        }
        if (id >= XS_XGM) { if (!ea_ok(u)) break; with_ea(o, u, t_xgt[id - XS_XGM][dc]); return; }
        if (id >= XS_XG) { if (S < 0) break; put_uop(o, u, t_xg[id - XS_XG][dc][S]); return; }
        if (!ea_ok(u)) break;
        switch (id) {
        case XS_MOVX_MR: put_uop(o, u, t_xs_movx[sc][u->base][u->index]); return;
        case XS_MOVSS_RM: if (simple_mem(u)) put_uop(o, u, t_xl_movss[dc][u->base]); else with_ea(o, u, t_xlt_movss[dc]); return;
        case XS_MOVSD_RM: if (simple_mem(u)) put_uop(o, u, t_xl_movsd[dc][u->base]); else with_ea(o, u, t_xlt_movsd[dc]); return;
        case XS_MOVSS_MR: if (simple_mem(u)) put_uop(o, u, t_xs_movss[sc][u->base]); else with_ea(o, u, t_xst_movss[sc]); return;
        case XS_MOVSD_MR: if (simple_mem(u)) put_uop(o, u, t_xs_movsd[sc][u->base]); else with_ea(o, u, t_xst_movsd[sc]); return;
        case XS_MOVLPS_RM: with_ea(o, u, t_xlt_movlps[dc]); return;
        case XS_MOVHPS_RM: with_ea(o, u, t_xlt_movhps[dc]); return;
        case XS_MOVHPS_MR: with_ea(o, u, t_xst_movhps[sc]); return;
        }
        break;
    }
    }
    put_uop(o, u, p_slow);
}

Block *fxr_lower(struct Fxi *vm, Block *b) {
    (void)vm;
    pthread_once(&g_once, init_desc);
    Out o = { malloc(sizeof(Uop) * (2 * (size_t)b->n + 1)), 0 };
    for (uint32_t i = 0; i < b->n; i++) lower_one(&o, &b->u[i]);
    Block *nb = malloc(sizeof(Block) + sizeof(Uop) * (size_t)o.n);
    nb->rip = b->rip;
    nb->n = (uint32_t)o.n;
    memcpy(nb->u, o.out, sizeof(Uop) * (size_t)o.n);
    free(o.out);
    free(b);
    return nb;
}

void fxr_init_stop(Block *b) { b->u[0].p = p_stop; }

void fxr_enter(FxiCpu *c, Block *b) {
    if (!c->fxr_ready) {
        for (unsigned i = 0; i <= IBTC_MASK; i++) { c->fxr_ibtc[i].rip = ~0ull; c->fxr_ibtc[i].u = 0; }
        c->fxr_ready = 1;
    }
    uint64_t g0 = c->r[0], g1 = c->r[1], g2 = c->r[2], g3 = c->r[3], g4 = c->r[4], g5 = c->r[5], g6 = c->r[6],
             g7 = c->r[7], g8 = c->r[8], g9 = c->r[9], g10 = c->r[10], g11 = c->r[11], g12 = c->r[12], g13 = c->r[13],
             g14 = c->r[14], g15 = c->r[15], T = 0, F0, F1, F2, F3;
    FxrV x0, x1, x2, x3, x4, x5, x6, x7;
    RELOAD_F();
    RELOAD_X();
    b->u[0].p(c, b->u, FXR_ARGS);
}
