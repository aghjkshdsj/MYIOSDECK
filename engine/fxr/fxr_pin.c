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

PH p_stop(FXR_PARAMS) { (void)c; (void)u; }
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
    SPILL_R(); SPILL_F();
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
    SPILL_R(); SPILL_F();
    u->fn(c, u);
    if (FXI_UNLIKELY(c->stop)) return;
    RELOAD_R(); RELOAD_F();
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

// ---- SSE: XMM stays in memory; pinned so a vector op never spills the GPRs. The
// definitions are FXI's own (fxr_sse_ops.inc), memory operands come through T. ----
#define LANES4(...) for (int i = 0; i < 4; i++) { __VA_ARGS__; }
#define LANES2(...) for (int i = 0; i < 2; i++) { __VA_ARGS__; }
typedef struct SseOp { const char *name; PFn rr, rt; struct SseOp *next; int hot; } SseOp;
static SseOp *g_sse_ops;
// Load W bytes from an address into a zeroed 16-byte temporary.
#define XLOAD(t, addr, W) do { if ((W) < 16) memset(&(t), 0, sizeof(t)); memcpy(&(t), (const void *)(uintptr_t)(addr), (W)); } while (0)
// Each op's body is one inline function shared by its register, T and [base+index] forms.
#define DEF_X(NAME, W, ...)                                                                \
    FXI_INLINE void sx_##NAME(FxiCpu *c, Uop *u, X128 *D, const X128 *S) {                 \
        (void)c; (void)u; (void)D; (void)S; __VA_ARGS__; }                                 \
    PH px_##NAME##_RR(FXR_PARAMS) { X128 t = c->xmm[u->src]; sx_##NAME(c, u, &c->xmm[u->dst], &t); PNEXT(); } \
    PH px_##NAME##_RT(FXR_PARAMS) { X128 t; XLOAD(t, T, (W)); sx_##NAME(c, u, &c->xmm[u->dst], &t); PNEXT(); } \
    static SseOp sse_##NAME = { #NAME, px_##NAME##_RR, px_##NAME##_RT, 0, -1 };            \
    __attribute__((constructor)) static void reg_##NAME(void) { sse_##NAME.next = g_sse_ops; g_sse_ops = &sse_##NAME; }
#include "fxr_sse_ops.inc"
#undef DEF_X

// The hottest vector ops with a [base + index*scale + disp] memory operand get one handler per
// (base, index) pair, so the address costs no extra dispatch (XMM operands stay runtime indices).
#define HOT_SSE(X) X(movx, 16) X(addps, 16) X(subps, 16) X(mulps, 16) X(divps, 16) X(addpd, 16) X(subpd, 16) \
    X(mulpd, 16) X(divpd, 16) X(addss, 4) X(subss, 4) X(mulss, 4) X(divss, 4) X(addsd, 8) X(subsd, 8) X(mulsd, 8) \
    X(divsd, 8) X(pand, 16) X(pandn, 16) X(por, 16) X(pxor, 16) X(paddd, 16) X(paddq, 16) X(psubd, 16) X(psubq, 16) \
    X(pcmpeqb, 16) X(pcmpeqd, 16) X(pmuludq, 16)
#define BI_EA(B, I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_XBI(I, B, NAME, W) PH pxb_##NAME##_##B##_##I(FXR_PARAMS) {                     \
        X128 t; XLOAD(t, BI_EA(B, I), (W)); sx_##NAME(c, u, &c->xmm[u->dst], &t); PNEXT(); }
#define DEF_XBI_ROW(B, NAME, W) R17B(DEF_XBI, B, NAME, W)
#define DEF_HOT(NAME, W) R17(DEF_XBI_ROW, NAME, W)
HOT_SSE(DEF_HOT)
#define E_XBI(I, B, NAME) pxb_##NAME##_##B##_##I,
#define ROW_XBI(B, NAME) { R17B(E_XBI, B, NAME) },
#define TAB_HOT(NAME, W) { R17(ROW_XBI, NAME) },
static const PFn t_hot[][17][17] = { HOT_SSE(TAB_HOT) };
#define NAME_HOT(NAME, W) #NAME,
static const char *const k_hot_names[] = { HOT_SSE(NAME_HOT) };

// Vector/scalar loads and stores, and integer stores of an immediate, with [base + index]:
#define DEF_MEMBI(I, B)                                                                    \
    PH pxs_movx_##B##_##I(FXR_PARAMS) { memcpy((void *)(uintptr_t)BI_EA(B, I), &c->xmm[u->src], 16); PNEXT(); } \
    PH pxs_movss_##B##_##I(FXR_PARAMS) { st32(BI_EA(B, I), c->xmm[u->src].d[0]); PNEXT(); } \
    PH pxs_movsd_##B##_##I(FXR_PARAMS) { st64(BI_EA(B, I), c->xmm[u->src].q[0]); PNEXT(); } \
    PH pxl_movss_##B##_##I(FXR_PARAMS) { X128 *D = &c->xmm[u->dst]; uint32_t v = (uint32_t)ld32(BI_EA(B, I)); \
        D->q[0] = D->q[1] = 0; D->d[0] = v; PNEXT(); }                                      \
    PH pxl_movsd_##B##_##I(FXR_PARAMS) { X128 *D = &c->xmm[u->dst]; uint64_t v = ld64(BI_EA(B, I)); \
        D->q[0] = v; D->q[1] = 0; PNEXT(); }                                                \
    PH psi_8_##B##_##I(FXR_PARAMS) { st8(BI_EA(B, I), u->imm); PNEXT(); }                   \
    PH psi_16_##B##_##I(FXR_PARAMS) { st16(BI_EA(B, I), u->imm); PNEXT(); }                 \
    PH psi_32_##B##_##I(FXR_PARAMS) { st32(BI_EA(B, I), u->imm); PNEXT(); }                 \
    PH psi_64_##B##_##I(FXR_PARAMS) { st64(BI_EA(B, I), u->imm); PNEXT(); }
#define DEF_MEMBI_ROW(B, _) R17B(DEF_MEMBI, B)
R17(DEF_MEMBI_ROW, _)
#define E_MBI(I, B, NAME) NAME##_##B##_##I,
#define ROW_MBI(B, NAME) { R17B(E_MBI, B, NAME) },
static const PFn t_xs_movx[17][17] = { R17(ROW_MBI, pxs_movx) }, t_xs_movss[17][17] = { R17(ROW_MBI, pxs_movss) },
    t_xs_movsd[17][17] = { R17(ROW_MBI, pxs_movsd) }, t_xl_movss[17][17] = { R17(ROW_MBI, pxl_movss) },
    t_xl_movsd[17][17] = { R17(ROW_MBI, pxl_movsd) };
static const PFn t_sti_bi[4][17][17] = { { R17(ROW_MBI, psi_8) }, { R17(ROW_MBI, psi_16) }, { R17(ROW_MBI, psi_32) },
                                          { R17(ROW_MBI, psi_64) } };
// (u)comiss/(u)comisd: ZF PF CF, the others clear, as raw lazy flags.
#define COMIS(NAME, W, FIELD)                                                              \
    PH px_##NAME##_RR(FXR_PARAMS) { double a = c->xmm[u->dst].FIELD[0], b = c->xmm[u->src].FIELD[0]; \
        uint64_t f = (a != a || b != b) ? 0x45 : a < b ? 0x01 : a == b ? 0x40 : 0;         \
        SETF(LF_RAW, 3, 0, 0, 0, f); PNEXT(); }                                            \
    PH px_##NAME##_RT(FXR_PARAMS) { X128 t; memset(&t, 0, sizeof t); memcpy(&t, (const void *)(uintptr_t)T, (W)); \
        double a = c->xmm[u->dst].FIELD[0], b = t.FIELD[0];                                \
        uint64_t f = (a != a || b != b) ? 0x45 : a < b ? 0x01 : a == b ? 0x40 : 0;         \
        SETF(LF_RAW, 3, 0, 0, 0, f); PNEXT(); }                                            \
    static SseOp sse_##NAME = { #NAME, px_##NAME##_RR, px_##NAME##_RT, 0, -1 };            \
    __attribute__((constructor)) static void reg_##NAME(void) { sse_##NAME.next = g_sse_ops; g_sse_ops = &sse_##NAME; }
COMIS(comiss, 4, f)
COMIS(comisd, 8, g)
// Scalar moves and stores (FXI's named handlers)
PH px_movx_TS(FXR_PARAMS) { memcpy((void *)(uintptr_t)T, &c->xmm[u->src], 16); PNEXT(); }
PH px_movss_RR(FXR_PARAMS) { c->xmm[u->dst].d[0] = c->xmm[u->src].d[0]; PNEXT(); }
PH px_movss_RT(FXR_PARAMS) { X128 *D = &c->xmm[u->dst]; D->q[0] = D->q[1] = 0; D->d[0] = (uint32_t)ld32(T); PNEXT(); }
PH px_movss_TS(FXR_PARAMS) { st32(T, c->xmm[u->src].d[0]); PNEXT(); }
PH px_movsd_RR(FXR_PARAMS) { c->xmm[u->dst].q[0] = c->xmm[u->src].q[0]; PNEXT(); }
PH px_movsd_RT(FXR_PARAMS) { X128 *D = &c->xmm[u->dst]; D->q[0] = ld64(T); D->q[1] = 0; PNEXT(); }
PH px_movsd_TS(FXR_PARAMS) { st64(T, c->xmm[u->src].q[0]); PNEXT(); }
PH px_movq_RR(FXR_PARAMS) { X128 *D = &c->xmm[u->dst]; D->q[0] = c->xmm[u->src].q[0]; D->q[1] = 0; PNEXT(); }

// ===========================================================================
// Lowering: FXI's decoded uops -> pinned handlers
// ===========================================================================
enum { FAM_NONE, FAM_ALU, FAM_MOV, FAM_LEA, FAM_SHIFT, FAM_UNARY, FAM_EXT, FAM_CMOV, FAM_SETCC, FAM_IMUL2,
       FAM_IMUL3, FAM_FJCC, FAM_NAMED, FAM_SSE };
enum { N_JMP, N_JCC, N_CALL, N_CALL_R, N_CALL_M, N_JMP_R, N_JMP_M, N_RET, N_GOTO, N_SYSCALL, N_STOP, N_PUSH_R,
       N_POP_R, N_NOP, N_MOVX_MR, N_MOVSS_RR, N_MOVSS_RM, N_MOVSS_MR, N_MOVSD_RR, N_MOVSD_RM, N_MOVSD_MR, N_MOVQ_RR };
typedef struct { OpFn key; uint8_t fam, a, b, c2, d; const SseOp *sse; } Desc;
#define DESC_SLOTS 16384
static Desc g_desc[DESC_SLOTS];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint64_t hptr(OpFn f) { return ((uint64_t)(uintptr_t)f * 0x9E3779B97F4A7C15ull) >> 50; }
static void put(OpFn f, uint8_t fam, uint8_t a, uint8_t b, uint8_t c2, uint8_t d, const SseOp *s) {
    if (!f) return;
    uint64_t h = hptr(f) & (DESC_SLOTS - 1);
    while (g_desc[h].key && g_desc[h].key != f) h = (h + 1) & (DESC_SLOTS - 1);
    g_desc[h] = (Desc){ f, fam, a, b, c2, d, s };
}
static const Desc *find(OpFn f) {
    uint64_t h = hptr(f) & (DESC_SLOTS - 1);
    while (g_desc[h].key) { if (g_desc[h].key == f) return &g_desc[h]; h = (h + 1) & (DESC_SLOTS - 1); }
    return 0;
}
static void init_desc(void) {
    for (int cc = 0; cc < 16; cc++)
        for (int i = 0; i < 16; i++)
            k_cctab[cc] |= (uint16_t)(cc_flags((unsigned)cc, (i >> 3) & 1, (i >> 1) & 1, i & 1, (i >> 2) & 1, 0) << i);
    for (int op = 0; op < ALU_COUNT; op++)
        for (int fm = 0; fm < F_COUNT; fm++)
            for (int si = 0; si < 4; si++)
                for (int fl = 0; fl < 2; fl++) put(fxi_alu_tab[op][fm][si][fl], FAM_ALU, op, fm, si, 0, 0);
    for (int fm = 0; fm < F_COUNT; fm++) for (int si = 0; si < 4; si++) put(fxi_mov_tab[fm][si], FAM_MOV, fm, si, 0, 0, 0);
    for (int si = 1; si < 4; si++) put(fxi_lea_tab[si], FAM_LEA, si, 0, 0, 0, 0);
    for (int op = 0; op < 8; op++) for (int rm = 0; rm < 2; rm++) for (int si = 0; si < 4; si++)
        put(fxi_shift_tab[op][rm][si], FAM_SHIFT, op, rm, si, 0, 0);
    for (int op = 0; op < 4; op++) for (int rm = 0; rm < 2; rm++) for (int si = 0; si < 4; si++)
        put(fxi_unary_tab[op][rm][si], FAM_UNARY, op, rm, si, 0, 0);
    for (int sg = 0; sg < 2; sg++) for (int rm = 0; rm < 2; rm++) for (int ss = 0; ss < 3; ss++) for (int ds = 1; ds < 4; ds++)
        put(fxi_ext_tab[sg][rm][ss][ds], FAM_EXT, sg, rm, ss, ds, 0);
    for (int rm = 0; rm < 2; rm++) for (int si = 1; si < 4; si++) {
        put(fxi_cmov_tab[rm][si], FAM_CMOV, rm, si, 0, 0, 0);
        put(fxi_imul2_tab[rm][si], FAM_IMUL2, rm, si, 0, 0, 0);
        put(fxi_imul3_tab[rm][si], FAM_IMUL3, rm, si, 0, 0, 0);
    }
    put(fxi_setcc_tab[0], FAM_SETCC, 0, 0, 0, 0, 0);
    for (int t = 0; t < 2; t++) for (int ri = 0; ri < 2; ri++) for (int s = 0; s < 2; s++) for (int cc = 0; cc < 16; cc++)
        put(fxi_fjcc_tab[t][ri][s][cc], FAM_FJCC, t, ri, s, cc, 0);
    static const struct { const char *n; int id; } named[] = {
        { "jmp", N_JMP }, { "jcc", N_JCC }, { "call", N_CALL }, { "call_R", N_CALL_R }, { "call_M", N_CALL_M },
        { "jmp_R", N_JMP_R }, { "jmp_M", N_JMP_M }, { "ret", N_RET }, { "goto", N_GOTO }, { "syscall", N_SYSCALL },
        { "stop", N_STOP }, { "push_R", N_PUSH_R }, { "pop_R", N_POP_R }, { "nop", N_NOP }, { "movx_MR", N_MOVX_MR },
        { "movss_RR", N_MOVSS_RR }, { "movss_RM", N_MOVSS_RM }, { "movss_MR", N_MOVSS_MR }, { "movsd_RR", N_MOVSD_RR },
        { "movsd_RM", N_MOVSD_RM }, { "movsd_MR", N_MOVSD_MR }, { "movq_RR", N_MOVQ_RR },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) put(fxi_named(named[i].n), FAM_NAMED, (uint8_t)named[i].id, 0, 0, 0, 0);
    char nm[48];
    for (SseOp *s = g_sse_ops; s; s = s->next)
        for (size_t h = 0; h < sizeof k_hot_names / sizeof k_hot_names[0]; h++)
            if (!strcmp(s->name, k_hot_names[h])) s->hot = (int)h;
    for (const SseOp *s = g_sse_ops; s; s = s->next) {
        snprintf(nm, sizeof nm, "%s_RR", s->name); put(fxi_named(nm), FAM_SSE, 0, 0, 0, 0, s);
        snprintf(nm, sizeof nm, "%s_RM", s->name); put(fxi_named(nm), FAM_SSE, 1, 0, 0, 0, s);
    }
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
        case N_MOVSS_RR: put_uop(o, u, px_movss_RR); return;
        case N_MOVSD_RR: put_uop(o, u, px_movsd_RR); return;
        case N_MOVQ_RR: put_uop(o, u, px_movq_RR); return;
        case N_MOVSS_RM: if (ea_ok(u)) { put_uop(o, u, t_xl_movss[u->base][u->index]); return; } break;
        case N_MOVSD_RM: if (ea_ok(u)) { put_uop(o, u, t_xl_movsd[u->base][u->index]); return; } break;
        case N_MOVSS_MR: if (ea_ok(u)) { put_uop(o, u, t_xs_movss[u->base][u->index]); return; } break;
        case N_MOVSD_MR: if (ea_ok(u)) { put_uop(o, u, t_xs_movsd[u->base][u->index]); return; } break;
        case N_MOVX_MR: if (ea_ok(u)) { put_uop(o, u, t_xs_movx[u->base][u->index]); return; } break;
        }
        // Control flow has no FXI fallback here (FXI's handlers would continue FXI's own chain).
        if (d->a <= N_STOP) { fprintf(stderr, "fxr: no pinned form for control-flow uop %d\n", d->a); abort(); }
        break;
    case FAM_SSE:
        if (!d->a) { put_uop(o, u, d->sse->rr); return; }
        if (ea_ok(u)) {
            if (d->sse->hot >= 0) { put_uop(o, u, t_hot[d->sse->hot][u->base][u->index]); return; }
            with_ea(o, u, d->sse->rt);
            return;
        }
        break;
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
    RELOAD_F();
    b->u[0].p(c, b->u, FXR_ARGS);
}
