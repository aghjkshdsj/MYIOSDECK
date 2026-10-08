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
// No loop or idiom recognition: every specialisation is per instruction.

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "fxi_internal.h"

#define PH static FXR_CC void
#define PNEXT() do { Uop *n_ = u + 1; __attribute__((musttail)) return n_->p(c, n_, FXR_ARGS); } while (0)
#define PGOTO(blk) do { Block *b_ = (blk); __attribute__((musttail)) return b_->u[0].p(c, b_->u, FXR_ARGS); } while (0)
#define gZ ((uint64_t)0)

#define R16(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R16B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
    M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) \
    M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define R17(M, ...) R16(M, __VA_ARGS__) M(Z, __VA_ARGS__)
#define R17B(M, ...) R16B(M, __VA_ARGS__) M(Z, __VA_ARGS__)

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

static const uint64_t kMask[4] = { M8, M16, M32, M64 };
static const uint64_t kSign[4] = { 0x80ull, 0x8000ull, 0x80000000ull, 0x8000000000000000ull };

// ---- conditions, branch-free from the operands of the last flag writer ----
FXI_INLINE int cc_bits(unsigned cc, int o, int cf, int z, int s, uint64_t r) {
    int p = ((cc >> 1) == 5) ? !__builtin_parity((unsigned)(r & 0xff)) : 0;
    unsigned bits = (unsigned)o | (unsigned)cf << 1 | (unsigned)z << 2 | (unsigned)(cf | z) << 3 |
                    (unsigned)s << 4 | (unsigned)p << 5 | (unsigned)(s ^ o) << 6 | (unsigned)(z | (s ^ o)) << 7;
    return (int)(((bits >> (cc >> 1)) & 1) ^ (cc & 1));
}
FXI_INLINE int cc_sub(unsigned cc, uint64_t a, uint64_t b, unsigned si) {
    uint64_t m = kMask[si], sb = kSign[si];
    a &= m; b &= m;
    uint64_t r = (a - b) & m;
    return cc_bits(cc, (((a ^ b) & (a ^ r)) & sb) != 0, a < b, r == 0, (r & sb) != 0, r);
}
FXI_INLINE int cc_logic(unsigned cc, uint64_t r, unsigned si) {
    r &= kMask[si];
    return cc_bits(cc, 0, 0, r == 0, (r & kSign[si]) != 0, r);
}
FXI_INLINE int cc_add(unsigned cc, uint64_t a, uint64_t b, uint64_t r, unsigned si) {
    uint64_t m = kMask[si], sb = kSign[si];
    a &= m; b &= m; r &= m;
    return cc_bits(cc, (((a ^ r) & (b ^ r)) & sb) != 0, r < a, r == 0, (r & sb) != 0, r);
}
static int slow_cond(FxiCpu *c, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, unsigned cc) {
    SPILL_F();
    return fxi_cond(c, cc);
}
FXI_INLINE int pcond(FxiCpu *c, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, unsigned cc) {
    unsigned op = (uint32_t)F0, k = op >> 2, si = op & 3;
    if (FXI_LIKELY(k == LF_SUB)) return cc_sub(cc, F1, F2, si);
    if (k == LF_LOGIC) return cc_logic(cc, F3, si);
    if (k == LF_ADD) return cc_add(cc, F1, F2, F3, si);
    return slow_cond(c, F0, F1, F2, F3, cc);
}
static uint64_t slow_cf(FxiCpu *c, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3) {
    SPILL_F();
    return fxi_flag_cf(c);
}
FXI_INLINE uint64_t pcf(FxiCpu *c, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3) {
    unsigned op = (uint32_t)F0, k = op >> 2, si = op & 3;
    uint64_t m = kMask[si];
    if (k == LF_SUB) return (F1 & m) < (F2 & m);
    if (k == LF_ADD) return (F3 & m) < (F1 & m);
    if (k == LF_LOGIC) return 0;
    return slow_cf(c, F0, F1, F2, F3);
}

// ---- chaining ----
#define PCHAIN(slot, rip_) do {                                                            \
        Block *bc_ = (slot);                                                               \
        if (FXI_UNLIKELY(!bc_)) { bc_ = fxi_lookup(c, (rip_)); if (bc_ != fxi_stop) (slot) = bc_; } \
        PGOTO(bc_);                                                                        \
    } while (0)
#define PIND(target) do {                                                                  \
        uint64_t t_ = (target); Block *l_ = u->link;                                       \
        if (FXI_LIKELY(l_ && l_->rip == t_)) PGOTO(l_);                                    \
        Block *bi_ = fxi_lookup(c, t_);                                                    \
        if (bi_ != fxi_stop) u->link = bi_;                                                \
        PGOTO(bi_);                                                                        \
    } while (0)

PH p_stop(FXR_PARAMS) { (void)c; (void)u; }
PH p_nop(FXR_PARAMS) { PNEXT(); }
PH p_jmp(FXR_PARAMS) { PCHAIN(u->link, u->imm); }
PH p_goto(FXR_PARAMS) { PCHAIN(u->link, u->aux); }
PH p_jcc(FXR_PARAMS) {
    if (pcond(c, F0, F1, F2, F3, u->cc)) PCHAIN(u->link, u->imm);
    PCHAIN(u->link2, u->aux);
}
PH p_call(FXR_PARAMS) { g4 -= 8; st64(g4, u->aux); PCHAIN(u->link, u->imm); }
PH p_ret(FXR_PARAMS) { uint64_t t = ld64(g4); g4 += 8 + u->aux; PIND(t); }
PH p_call_M(FXR_PARAMS) { SPILL_R(); uint64_t t = ld64(fxi_ea(c, u)); g4 -= 8; st64(g4, u->aux); PIND(t); }
PH p_jmp_M(FXR_PARAMS) { SPILL_R(); PIND(ld64(fxi_ea(c, u))); }
PH p_syscall(FXR_PARAMS) {
    SPILL_R(); SPILL_F();
    c->rip = u->aux;
    long r = fxi_syscall(c);
    if (c->stop) return;
    RELOAD_R();
    g0 = (uint64_t)r;
    g1 = u->aux;                  // syscall clobbers RCX (return rip) and R11 (rflags)
    g11 = fxi_rflags(c);
    PCHAIN(u->link, u->aux);
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
    PH p_jmp_R_##R(FXR_PARAMS) { PIND(g##R); }                                             \
    PH p_setcc_##R(FXR_PARAMS) { g##R = (g##R & ~0xffull) | (uint64_t)pcond(c, F0, F1, F2, F3, u->cc); PNEXT(); }
R16(DEF_PUSHPOP, _)
#define E1(R, NAME) p_##NAME##_##R,
static const PFn t_push[16] = { R16(E1, push) }, t_pop[16] = { R16(E1, pop) };
static const PFn t_call_R[16] = { R16(E1, call_R) }, t_jmp_R[16] = { R16(E1, jmp_R) }, t_setcc[16] = { R16(E1, setcc) };

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
#define ALU_BODY(OPN, SZ, A, B, STORE)                                                     \
    uint64_t ci = C_##OPN ? pcf(c, F0, F1, F2, F3) : 0; (void)ci;                          \
    uint64_t a = (A) & M##SZ, b = (B) & M##SZ, r = X_##OPN(a, b, ci) & M##SZ;              \
    if (W_##OPN) { STORE; }                                                                \
    SETF(K_##OPN, SI##SZ, a, b, r, ci);                                                    \
    PNEXT();

#define DEF_ALU_RR(S, D, OPN, SZ) PH p_alu_rr_##OPN##_##SZ##_##D##_##S(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, g##S, g##D = r) }
#define DEF_ALU_RR_ROW(D, OPN, SZ) R16B(DEF_ALU_RR, D, OPN, SZ)
#define DEF_ALU_R(D, OPN, SZ)                                                              \
    PH p_alu_ri_##OPN##_##SZ##_##D(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, u->imm, g##D = r) } \
    PH p_alu_rt_##OPN##_##SZ##_##D(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, ld##SZ(T), g##D = r) }
#define DEF_ALU_T(S, OPN, SZ) PH p_alu_tr_##OPN##_##SZ##_##S(FXR_PARAMS) { ALU_BODY(OPN, SZ, ld##SZ(T), g##S, st##SZ(T, r)) }
#define DEF_ALU_SZ(OPN, SZ) R16(DEF_ALU_RR_ROW, OPN, SZ) R16(DEF_ALU_R, OPN, SZ)
#define DEF_ALU_TSZ(OPN, SZ) R16(DEF_ALU_T, OPN, SZ) \
    PH p_alu_ti_##OPN##_##SZ(FXR_PARAMS) { ALU_BODY(OPN, SZ, ld##SZ(T), u->imm, st##SZ(T, r)) }
#define DEF_ALU(OPN) DEF_ALU_SZ(OPN, 32) DEF_ALU_SZ(OPN, 64) \
    DEF_ALU_TSZ(OPN, 8) DEF_ALU_TSZ(OPN, 16) DEF_ALU_TSZ(OPN, 32) DEF_ALU_TSZ(OPN, 64)
DEF_ALU(add) DEF_ALU(or) DEF_ALU(adc) DEF_ALU(sbb) DEF_ALU(and) DEF_ALU(sub) DEF_ALU(xor) DEF_ALU(cmp) DEF_ALU(test)

#define E_ALU_RR(S, D, OPN, SZ) p_alu_rr_##OPN##_##SZ##_##D##_##S,
#define ROW_ALU_RR(D, OPN, SZ) { R16B(E_ALU_RR, D, OPN, SZ) },
#define T_ALU_RR(OPN) { { R16(ROW_ALU_RR, OPN, 32) }, { R16(ROW_ALU_RR, OPN, 64) } },
static const PFn t_alu_rr[ALU_COUNT][2][16][16] = {
    T_ALU_RR(add) T_ALU_RR(or) T_ALU_RR(adc) T_ALU_RR(sbb) T_ALU_RR(and) T_ALU_RR(sub) T_ALU_RR(xor) T_ALU_RR(cmp) T_ALU_RR(test)
};
#define E_ALU_X(D, KIND, OPN, SZ) p_alu_##KIND##_##OPN##_##SZ##_##D,
#define T_ALU_X(KIND, OPN) { { R16(E_ALU_X, KIND, OPN, 32) }, { R16(E_ALU_X, KIND, OPN, 64) } },
static const PFn t_alu_ri[ALU_COUNT][2][16] = {
    T_ALU_X(ri, add) T_ALU_X(ri, or) T_ALU_X(ri, adc) T_ALU_X(ri, sbb) T_ALU_X(ri, and) T_ALU_X(ri, sub) T_ALU_X(ri, xor) T_ALU_X(ri, cmp) T_ALU_X(ri, test)
};
static const PFn t_alu_rt[ALU_COUNT][2][16] = {
    T_ALU_X(rt, add) T_ALU_X(rt, or) T_ALU_X(rt, adc) T_ALU_X(rt, sbb) T_ALU_X(rt, and) T_ALU_X(rt, sub) T_ALU_X(rt, xor) T_ALU_X(rt, cmp) T_ALU_X(rt, test)
};
#define T_ALU_TR(OPN) { { R16(E_ALU_X, tr, OPN, 8) }, { R16(E_ALU_X, tr, OPN, 16) }, { R16(E_ALU_X, tr, OPN, 32) }, { R16(E_ALU_X, tr, OPN, 64) } },
static const PFn t_alu_tr[ALU_COUNT][4][16] = {
    T_ALU_TR(add) T_ALU_TR(or) T_ALU_TR(adc) T_ALU_TR(sbb) T_ALU_TR(and) T_ALU_TR(sub) T_ALU_TR(xor) T_ALU_TR(cmp) T_ALU_TR(test)
};
#define T_ALU_TI(OPN) { p_alu_ti_##OPN##_8, p_alu_ti_##OPN##_16, p_alu_ti_##OPN##_32, p_alu_ti_##OPN##_64 },
static const PFn t_alu_ti[ALU_COUNT][4] = {
    T_ALU_TI(add) T_ALU_TI(or) T_ALU_TI(adc) T_ALU_TI(sbb) T_ALU_TI(and) T_ALU_TI(sub) T_ALU_TI(xor) T_ALU_TI(cmp) T_ALU_TI(test)
};

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

// ---- shifts/rotates of a register (32/64). Count: imm (I) or CL (C). Zero count: no change,
// except that a 32-bit destination is still zero-extended.
// Rotates get a pinned form only when their flags are dead (lowering checks u->cc).
#define SH_shl(a, n, SZ) ((a) << (n))
#define SH_shr(a, n, SZ) ((a) >> (n))
#define SH_sar(a, n, SZ) (uint64_t)((int64_t)((a) << (64 - SZ)) >> (64 - SZ) >> (n))
#define SH_rol(a, n, SZ) ((((a) << ((n) % SZ)) | ((a) >> ((SZ - (n) % SZ) % SZ))))
#define SH_ror(a, n, SZ) ((((a) >> ((n) % SZ)) | ((a) << ((SZ - (n) % SZ) % SZ))))
#define SHK_shl LF_SHL
#define SHK_shr LF_SHR
#define SHK_sar LF_SAR
#define SHF_shl 1
#define SHF_shr 1
#define SHF_sar 1
#define SHF_rol 0
#define SHF_ror 0
#define SHK_rol 0
#define SHK_ror 0
#define DEF_SH(D, OPN, SZ, CNT, CV)                                                        \
    PH p_sh_##OPN##_##SZ##_##CNT##_##D(FXR_PARAMS) {                                       \
        unsigned n = (unsigned)(CV) & (SZ == 64 ? 63u : 31u);                              \
        if (FXI_UNLIKELY(n == 0)) { if (SZ == 32) g##D &= M32; PNEXT(); }                  \
        uint64_t a = g##D & M##SZ, r = SH_##OPN(a, n, SZ) & M##SZ;                         \
        g##D = r;                                                                          \
        if (SHF_##OPN) SETF(SHK_##OPN, SI##SZ, a, n, r, 0);                                \
        PNEXT();                                                                           \
    }
#define DEF_SH_ALL(OPN) R16(DEF_SH, OPN, 32, I, u->imm) R16(DEF_SH, OPN, 64, I, u->imm) \
    R16(DEF_SH, OPN, 32, C, g1) R16(DEF_SH, OPN, 64, C, g1)
DEF_SH_ALL(shl) DEF_SH_ALL(shr) DEF_SH_ALL(sar) DEF_SH_ALL(rol) DEF_SH_ALL(ror)
#define E_SH(D, OPN, SZ, CNT) p_sh_##OPN##_##SZ##_##CNT##_##D,
#define T_SH(OPN) { { { R16(E_SH, OPN, 32, I) }, { R16(E_SH, OPN, 32, C) } }, { { R16(E_SH, OPN, 64, I) }, { R16(E_SH, OPN, 64, C) } } },
enum { PS_SHL, PS_SHR, PS_SAR, PS_ROL, PS_ROR };
static const PFn t_sh[5][2][2][16] = { T_SH(shl) T_SH(shr) T_SH(sar) T_SH(rol) T_SH(ror) };

// ---- inc dec not neg (32/64 registers); u->cc != 0: flags live ----
#define DEF_UN(D, SZ)                                                                      \
    PH p_inc_##SZ##_##D(FXR_PARAMS) { uint64_t a = g##D & M##SZ, r = (a + 1) & M##SZ; g##D = r; \
        if (u->cc) { uint64_t ci = pcf(c, F0, F1, F2, F3); SETF(LF_INC, SI##SZ, a, 1, r, ci); } PNEXT(); } \
    PH p_dec_##SZ##_##D(FXR_PARAMS) { uint64_t a = g##D & M##SZ, r = (a - 1) & M##SZ; g##D = r; \
        if (u->cc) { uint64_t ci = pcf(c, F0, F1, F2, F3); SETF(LF_DEC, SI##SZ, a, 1, r, ci); } PNEXT(); } \
    PH p_not_##SZ##_##D(FXR_PARAMS) { g##D = ~g##D & M##SZ; PNEXT(); }                     \
    PH p_neg_##SZ##_##D(FXR_PARAMS) { uint64_t a = g##D & M##SZ, r = (0 - a) & M##SZ; g##D = r; \
        if (u->cc) SETF(LF_NEG, SI##SZ, a, 0, r, 0); PNEXT(); }
R16(DEF_UN, 32) R16(DEF_UN, 64)
#define T_UN(SZ) { { R16(E1, inc_##SZ) }, { R16(E1, dec_##SZ) }, { R16(E1, not_##SZ) }, { R16(E1, neg_##SZ) } }
static const PFn t_un[2][4][16] = { T_UN(32), T_UN(64) };

// ---- imul (two/three operand), cmov ----
#define IMUL_BODY(SZ, A, B)                                                                \
    int64_t a = (ST##SZ)(A), b = (ST##SZ)(B);                                              \
    __int128 r = (__int128)a * b;                                                          \
    uint64_t lo = (uint64_t)r & M##SZ;                                                     \
    SETF(LF_MUL, SI##SZ, 0, 0, lo, r != (ST##SZ)r);
#define DEF_IMUL(S, D, SZ)                                                                 \
    PH p_imul2_##SZ##_##D##_##S(FXR_PARAMS) { IMUL_BODY(SZ, g##D, g##S) g##D = lo; PNEXT(); } \
    PH p_imul3_##SZ##_##D##_##S(FXR_PARAMS) { IMUL_BODY(SZ, g##S, u->imm) g##D = lo; PNEXT(); } \
    PH p_cmov_##SZ##_##D##_##S(FXR_PARAMS) {                                               \
        g##D = pcond(c, F0, F1, F2, F3, u->cc) ? (g##S & M##SZ) : (g##D & M##SZ); PNEXT(); }
#define DEF_IMUL_ROW(D, SZ) R16B(DEF_IMUL, D, SZ)                                          \
    PH p_imul2t_##SZ##_##D(FXR_PARAMS) { IMUL_BODY(SZ, g##D, ld##SZ(T)) g##D = lo; PNEXT(); } \
    PH p_imul3t_##SZ##_##D(FXR_PARAMS) { IMUL_BODY(SZ, ld##SZ(T), u->imm) g##D = lo; PNEXT(); } \
    PH p_cmovt_##SZ##_##D(FXR_PARAMS) {                                                    \
        uint64_t v = ld##SZ(T); g##D = pcond(c, F0, F1, F2, F3, u->cc) ? v : (g##D & M##SZ); PNEXT(); }
R16(DEF_IMUL_ROW, 32) R16(DEF_IMUL_ROW, 64)
static const PFn t_imul2[2][16][16] = { { R16(ROW_RR, imul2_32) }, { R16(ROW_RR, imul2_64) } };
static const PFn t_imul3[2][16][16] = { { R16(ROW_RR, imul3_32) }, { R16(ROW_RR, imul3_64) } };
static const PFn t_cmov[2][16][16] = { { R16(ROW_RR, cmov_32) }, { R16(ROW_RR, cmov_64) } };
static const PFn t_imul2t[2][16] = { { R16(E1, imul2t_32) }, { R16(E1, imul2t_64) } };
static const PFn t_imul3t[2][16] = { { R16(E1, imul3t_32) }, { R16(E1, imul3t_64) } };
static const PFn t_cmovt[2][16] = { { R16(E1, cmovt_32) }, { R16(E1, cmovt_64) } };

// ---- fused cmp/test + jcc (cc in u->cc; the RI immediate rides in disp) ----
#define FJ_BR(TAKEN) do { if (TAKEN) PCHAIN(u->link, u->imm); PCHAIN(u->link2, u->aux); } while (0)
#define DEF_FJ_RR(S, D, SZ)                                                                \
    PH p_fjc_rr_##SZ##_##D##_##S(FXR_PARAMS) {                                             \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = (a - b) & M##SZ;                  \
        SETF(LF_SUB, SI##SZ, a, b, r, 0); FJ_BR(cc_sub(u->cc, a, b, SI##SZ)); }            \
    PH p_fjt_rr_##SZ##_##D##_##S(FXR_PARAMS) {                                             \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = a & b;                            \
        SETF(LF_LOGIC, SI##SZ, a, b, r, 0); FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
#define DEF_FJ_ROW(D, SZ) R16B(DEF_FJ_RR, D, SZ)                                           \
    PH p_fjc_ri_##SZ##_##D(FXR_PARAMS) {                                                   \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = (a - b) & M##SZ;     \
        SETF(LF_SUB, SI##SZ, a, b, r, 0); FJ_BR(cc_sub(u->cc, a, b, SI##SZ)); }            \
    PH p_fjt_ri_##SZ##_##D(FXR_PARAMS) {                                                   \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = a & b;               \
        SETF(LF_LOGIC, SI##SZ, a, b, r, 0); FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
R16(DEF_FJ_ROW, 32) R16(DEF_FJ_ROW, 64)
static const PFn t_fj_rr[2][2][16][16] = {
    { { R16(ROW_RR, fjc_rr_32) }, { R16(ROW_RR, fjc_rr_64) } }, { { R16(ROW_RR, fjt_rr_32) }, { R16(ROW_RR, fjt_rr_64) } } };
static const PFn t_fj_ri[2][2][16] = {
    { { R16(E1, fjc_ri_32) }, { R16(E1, fjc_ri_64) } }, { { R16(E1, fjt_ri_32) }, { R16(E1, fjt_ri_64) } } };

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
    int D = greg(u->dst), S = greg(u->src);
    if (d) switch (d->fam) {
    case FAM_ALU: {
        int op = d->a, fm = d->b, si = d->c2;
        if (fm == F_RR && si >= 2 && D >= 0 && S >= 0) { put_uop(o, u, t_alu_rr[op][si - 2][D][S]); return; }
        if (fm == F_RI && si >= 2 && D >= 0) { put_uop(o, u, t_alu_ri[op][si - 2][D]); return; }
        if (fm == F_RM && si >= 2 && D >= 0 && ea_ok(u)) { with_ea(o, u, t_alu_rt[op][si - 2][D]); return; }
        if (fm == F_MR && S >= 0 && ea_ok(u)) { with_ea(o, u, t_alu_tr[op][si][S]); return; }
        if (fm == F_MI && ea_ok(u)) { with_ea(o, u, t_alu_ti[op][si]); return; }
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
        put_uop(o, u, t_sh[ps][si - 2][u->src == 0xffff][D]);
        return;
    }
    case FAM_UNARY:
        if (!d->b && d->c2 >= 2 && D >= 0) { put_uop(o, u, t_un[d->c2 - 2][d->a][D]); return; }
        break;
    case FAM_IMUL2: case FAM_IMUL3: case FAM_CMOV: {
        int rm = d->a, si = d->b;
        if (si < 2 || D < 0) break;
        const PFn (*rr)[16][16] = d->fam == FAM_IMUL2 ? t_imul2 : d->fam == FAM_IMUL3 ? t_imul3 : t_cmov;
        const PFn (*rt)[16] = d->fam == FAM_IMUL2 ? t_imul2t : d->fam == FAM_IMUL3 ? t_imul3t : t_cmovt;
        if (!rm && S >= 0) { put_uop(o, u, rr[si - 2][D][S]); return; }
        if (rm && ea_ok(u)) { with_ea(o, u, rt[si - 2][D]); return; }
        break;
    }
    case FAM_SETCC:
        if (D >= 0) { put_uop(o, u, t_setcc[D]); return; }
        break;
    case FAM_FJCC: {
        int t = d->a, ri = d->b, s64 = d->c2;
        if (D < 0) break;
        Uop *x;
        if (!ri) { if (S < 0) break; x = put_uop(o, u, t_fj_rr[t][s64][D][S]); }
        else x = put_uop(o, u, t_fj_ri[t][s64][D]);
        x->cc = d->d;
        return;
    }
    case FAM_NAMED:
        switch (d->a) {
        case N_JMP: put_uop(o, u, p_jmp); return;
        case N_JCC: put_uop(o, u, p_jcc); return;
        case N_CALL: put_uop(o, u, p_call); return;
        case N_CALL_R: if (S >= 0) { put_uop(o, u, t_call_R[S]); return; } break;
        case N_JMP_R: if (S >= 0) { put_uop(o, u, t_jmp_R[S]); return; } break;
        case N_CALL_M: put_uop(o, u, p_call_M); return;
        case N_JMP_M: put_uop(o, u, p_jmp_M); return;
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
    uint64_t g0 = c->r[0], g1 = c->r[1], g2 = c->r[2], g3 = c->r[3], g4 = c->r[4], g5 = c->r[5], g6 = c->r[6],
             g7 = c->r[7], g8 = c->r[8], g9 = c->r[9], g10 = c->r[10], g11 = c->r[11], g12 = c->r[12], g13 = c->r[13],
             g14 = c->r[14], g15 = c->r[15], T = 0, F0, F1, F2, F3;
    RELOAD_F();
    b->u[0].p(c, b->u, FXR_ARGS);
}
