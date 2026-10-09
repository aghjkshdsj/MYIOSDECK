// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: pinned integer handlers (ALU, moves, loads/stores, shifts, unary, imul, cmov). See fxr_pin.c.

#include "fxr_pin.h"

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
// before anything changed. KEEP1 follows a memory source's load, KEEP2 a memory destination's
// store (exact faults: what the instruction overwrites stays in its register until the access
// is done, fxr_pin.h KEEP_F / KEEP_R).
#define ALU_BODY(OPN, SZ, A, B, STORE, FL, KEEP1, KEEP2)                                   \
    uint64_t ci = 0;                                                                       \
    if (C_##OPN) { int s_ = 0; ci = pcf(F0, F1, F2, F3, &s_); if (FXI_UNLIKELY(s_)) PTAIL(p_slow); } \
    uint64_t a = (A) & M##SZ, b = (B) & M##SZ;                                             \
    KEEP1;                                                                                 \
    uint64_t r = X_##OPN(a, b, ci) & M##SZ;                                                \
    (void)ci;                                                                              \
    if (W_##OPN) { STORE; }                                                                \
    KEEP2;                                                                                 \
    if (FLV_##FL) SETF(K_##OPN, SI##SZ, a, b, r, ci);                                      \
    PNEXT();
#define MKEEP_F KEEP_F()   // flags live: written after the access
#define MKEEP_N ((void)0)  // flags dead: not written at all

#define DEF_ALU_RR(S, D, OPN, SZ, FL) PH p_alu_rr_##OPN##_##SZ##FL##_##D##_##S(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, g##S, g##D = r, FL, (void)0, (void)0) }
#define DEF_ALU_RR_ROW(D, OPN, SZ, FL) R16B(DEF_ALU_RR, D, OPN, SZ, FL)
#define DEF_ALU_R(D, OPN, SZ, FL)                                                          \
    PH p_alu_ri_##OPN##_##SZ##FL##_##D(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, u->imm, g##D = r, FL, (void)0, (void)0) } \
    PH p_alu_rt_##OPN##_##SZ##FL##_##D(FXR_PARAMS) { ALU_BODY(OPN, SZ, g##D, ld##SZ(T), g##D = r, FL, MKEEP_##FL; KEEP_R(D), (void)0) }
#define DEF_ALU_T(S, OPN, SZ, FL) PH p_alu_tr_##OPN##_##SZ##FL##_##S(FXR_PARAMS) { ALU_BODY(OPN, SZ, ld##SZ(T), g##S, st##SZ(T, r), FL, (void)0, MKEEP_##FL) }
#define DEF_ALU_SZ(OPN, SZ, FL) R16(DEF_ALU_RR_ROW, OPN, SZ, FL) R16(DEF_ALU_R, OPN, SZ, FL)
#define DEF_ALU_TSZ(OPN, SZ, FL) R16(DEF_ALU_T, OPN, SZ, FL) \
    PH p_alu_ti_##OPN##_##SZ##FL(FXR_PARAMS) { ALU_BODY(OPN, SZ, ld##SZ(T), u->imm, st##SZ(T, r), FL, (void)0, MKEEP_##FL) }
#define DEF_ALU_FL(OPN, FL) DEF_ALU_SZ(OPN, 32, FL) DEF_ALU_SZ(OPN, 64, FL) \
    DEF_ALU_TSZ(OPN, 8, FL) DEF_ALU_TSZ(OPN, 16, FL) DEF_ALU_TSZ(OPN, 32, FL) DEF_ALU_TSZ(OPN, 64, FL)
#define DEF_ALU(OPN) DEF_ALU_FL(OPN, F) DEF_ALU_FL(OPN, N)
DEF_ALU(add) DEF_ALU(or) DEF_ALU(adc) DEF_ALU(sbb) DEF_ALU(and) DEF_ALU(sub) DEF_ALU(xor) DEF_ALU(cmp) DEF_ALU(test)

#define E_ALU_RR(S, D, OPN, SZF) p_alu_rr_##OPN##_##SZF##_##D##_##S,
#define ROW_ALU_RR(D, OPN, SZF) { R16B(E_ALU_RR, D, OPN, SZF) },
#define T_ALU_RR(OPN, FL) { { R16(ROW_ALU_RR, OPN, 32##FL) }, { R16(ROW_ALU_RR, OPN, 64##FL) } },
#define T_ALU_RR_ALL(FL) { T_ALU_RR(add, FL) T_ALU_RR(or, FL) T_ALU_RR(adc, FL) T_ALU_RR(sbb, FL) T_ALU_RR(and, FL) \
    T_ALU_RR(sub, FL) T_ALU_RR(xor, FL) T_ALU_RR(cmp, FL) T_ALU_RR(test, FL) }
const PFn t_alu_rr[2][ALU_COUNT][2][16][16] = { T_ALU_RR_ALL(N), T_ALU_RR_ALL(F) };   // [flags live][op][64?][D][S]
#define E_ALU_X(D, KIND, OPN, SZF) p_alu_##KIND##_##OPN##_##SZF##_##D,
#define T_ALU_X(KIND, OPN, FL) { { R16(E_ALU_X, KIND, OPN, 32##FL) }, { R16(E_ALU_X, KIND, OPN, 64##FL) } },
#define T_ALU_X_ALL(KIND, FL) { T_ALU_X(KIND, add, FL) T_ALU_X(KIND, or, FL) T_ALU_X(KIND, adc, FL) T_ALU_X(KIND, sbb, FL) \
    T_ALU_X(KIND, and, FL) T_ALU_X(KIND, sub, FL) T_ALU_X(KIND, xor, FL) T_ALU_X(KIND, cmp, FL) T_ALU_X(KIND, test, FL) }
const PFn t_alu_ri[2][ALU_COUNT][2][16] = { T_ALU_X_ALL(ri, N), T_ALU_X_ALL(ri, F) };
const PFn t_alu_rt[2][ALU_COUNT][2][16] = { T_ALU_X_ALL(rt, N), T_ALU_X_ALL(rt, F) };
#define T_ALU_TR(OPN, FL) { { R16(E_ALU_X, tr, OPN, 8##FL) }, { R16(E_ALU_X, tr, OPN, 16##FL) }, \
    { R16(E_ALU_X, tr, OPN, 32##FL) }, { R16(E_ALU_X, tr, OPN, 64##FL) } },
#define T_ALU_TR_ALL(FL) { T_ALU_TR(add, FL) T_ALU_TR(or, FL) T_ALU_TR(adc, FL) T_ALU_TR(sbb, FL) T_ALU_TR(and, FL) \
    T_ALU_TR(sub, FL) T_ALU_TR(xor, FL) T_ALU_TR(cmp, FL) T_ALU_TR(test, FL) }
const PFn t_alu_tr[2][ALU_COUNT][4][16] = { T_ALU_TR_ALL(N), T_ALU_TR_ALL(F) };
#define T_ALU_TI(OPN, FL) { p_alu_ti_##OPN##_8##FL, p_alu_ti_##OPN##_16##FL, p_alu_ti_##OPN##_32##FL, p_alu_ti_##OPN##_64##FL },
#define T_ALU_TI_ALL(FL) { T_ALU_TI(add, FL) T_ALU_TI(or, FL) T_ALU_TI(adc, FL) T_ALU_TI(sbb, FL) T_ALU_TI(and, FL) \
    T_ALU_TI(sub, FL) T_ALU_TI(xor, FL) T_ALU_TI(cmp, FL) T_ALU_TI(test, FL) }
const PFn t_alu_ti[2][ALU_COUNT][4] = { T_ALU_TI_ALL(N), T_ALU_TI_ALL(F) };

// ---- moves, loads, stores, lea, effective addresses ----
// Load kinds (also movzx/movsx sources): value from an address A.
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
const PFn t_stti[4] = { p_stti_8, p_stti_16, p_stti_32, p_stti_64 };

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
#define DEF_EA(I, B) PH p_ea_##B##_##I(FXR_PARAMS) { T = g##B + (g##I << u->scale) + (uint64_t)u->disp; PNEXT(); } \
    PH p_eaz_##B##_##I(FXR_PARAMS) { T = g##B + g##I; PNEXT(); }   /* no scale, no displacement */
#define DEF_EA_ROW(B, _) R17B(DEF_EA, B)
R17(DEF_EA_ROW, _)

#define E2(D, B, NAME) p_##NAME##_##B##_##D,
#define ROW_B(B, NAME) { R16B(E2, B, NAME) },
const PFn t_ld[2][17][16] = { { R17(ROW_B, ld_KL32) }, { R17(ROW_B, ld_KL64) } };
const PFn t_st[4][17][16] = { { R17(ROW_B, st_8) }, { R17(ROW_B, st_16) }, { R17(ROW_B, st_32) }, { R17(ROW_B, st_64) } };
const PFn t_lea[2][17][16] = { { R17(ROW_B, lea_32) }, { R17(ROW_B, lea_64) } };
#define E_STI(B, SZ) p_sti_##SZ##_##B,
const PFn t_sti[4][17] = { { R17(E_STI, 8) }, { R17(E_STI, 16) }, { R17(E_STI, 32) }, { R17(E_STI, 64) } };
// stores of an immediate with [base + index*scale + disp]
#define BI_EA(B, I) (g##B + (g##I << u->scale) + (uint64_t)u->disp)
#define DEF_STI_BI(I, B)                                                                   \
    PH psi_8_##B##_##I(FXR_PARAMS) { st8(BI_EA(B, I), u->imm); PNEXT(); }                   \
    PH psi_16_##B##_##I(FXR_PARAMS) { st16(BI_EA(B, I), u->imm); PNEXT(); }                 \
    PH psi_32_##B##_##I(FXR_PARAMS) { st32(BI_EA(B, I), u->imm); PNEXT(); }                 \
    PH psi_64_##B##_##I(FXR_PARAMS) { st64(BI_EA(B, I), u->imm); PNEXT(); }
#define DEF_STI_BI_ROW(B, _) R17B(DEF_STI_BI, B)
R17(DEF_STI_BI_ROW, _)
#define E_MBI(I, B, NAME) NAME##_##B##_##I,
#define ROW_MBI(B, NAME) { R17B(E_MBI, B, NAME) },
const PFn t_sti_bi[4][17][17] = { { R17(ROW_MBI, psi_8) }, { R17(ROW_MBI, psi_16) }, { R17(ROW_MBI, psi_32) },
                                          { R17(ROW_MBI, psi_64) } };
#define E_EA(I, B) p_ea_##B##_##I,
#define ROW_EA(B, _) { R17B(E_EA, B) },
const PFn t_ea[17][17] = { R17(ROW_EA, _) };
#define E_EAZ(I, B) p_eaz_##B##_##I,
#define ROW_EAZ(B, _) { R17B(E_EAZ, B) },
const PFn t_eaz[17][17] = { R17(ROW_EAZ, _) };
#define ROW_RR(D, NAME) { R16B(E_RR, D, NAME) },
#define E_RR(S, D, NAME) p_##NAME##_##D##_##S,
const PFn t_mov_rr[2][16][16] = { { R16(ROW_RR, mov_rr_32) }, { R16(ROW_RR, mov_rr_64) } };
const PFn t_mov_ri[2][16] = { { R16(E1, mov_ri_32) }, { R16(E1, mov_ri_64) } };
const PFn t_movt[2][16] = { { R16(E1, movt_32) }, { R16(E1, movt_64) } };
const PFn t_ldt[K_COUNT][16] = {
    { R16(E1, ldt_KL32) }, { R16(E1, ldt_KL64) }, { R16(E1, ldt_KZ8) }, { R16(E1, ldt_KZ16) }, { R16(E1, ldt_KS8_32) },
    { R16(E1, ldt_KS8_64) }, { R16(E1, ldt_KS16_32) }, { R16(E1, ldt_KS16_64) }, { R16(E1, ldt_KS32_64) },
};
const PFn t_stt[4][16] = { { R16(E1, stt_8) }, { R16(E1, stt_16) }, { R16(E1, stt_32) }, { R16(E1, stt_64) } };

// movzx/movsx from a register (kinds KZ8.. KS32_64)
#define DEF_EXT(S, D, K) PH p_ext_##K##_##D##_##S(FXR_PARAMS) { g##D = EXK_##K(g##S); PNEXT(); }
#define DEF_EXT_ROW(D, K) R16B(DEF_EXT, D, K)
R16(DEF_EXT_ROW, KZ8) R16(DEF_EXT_ROW, KZ16) R16(DEF_EXT_ROW, KS8_32) R16(DEF_EXT_ROW, KS8_64)
R16(DEF_EXT_ROW, KS16_32) R16(DEF_EXT_ROW, KS16_64) R16(DEF_EXT_ROW, KS32_64)
#define ROW_EXT(D, K) { R16B(E_EXT, D, K) },
#define E_EXT(S, D, K) p_ext_##K##_##D##_##S,
const PFn t_ext[K_COUNT][16][16] = {
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
const PFn t_sh[2][5][2][2][16] = {   // [flags live][op][64?][CL?][D]
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
const PFn t_un[2][2][4][16] = { { T_UN(32N), T_UN(64N) }, { T_UN(32F), T_UN(64F) } };   // [flags live][64?][op][D]

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
const PFn t_imul2[2][2][16][16] = { { { R16(ROW_RR, imul2_32N) }, { R16(ROW_RR, imul2_64N) } },
                                           { { R16(ROW_RR, imul2_32F) }, { R16(ROW_RR, imul2_64F) } } };
const PFn t_imul3[2][2][16][16] = { { { R16(ROW_RR, imul3_32N) }, { R16(ROW_RR, imul3_64N) } },
                                           { { R16(ROW_RR, imul3_32F) }, { R16(ROW_RR, imul3_64F) } } };
const PFn t_imul2t[2][2][16] = { { { R16(E1, imul2t_32N) }, { R16(E1, imul2t_64N) } },
                                        { { R16(E1, imul2t_32F) }, { R16(E1, imul2t_64F) } } };
const PFn t_imul3t[2][2][16] = { { { R16(E1, imul3t_32N) }, { R16(E1, imul3t_64N) } },
                                        { { R16(E1, imul3t_32F) }, { R16(E1, imul3t_64F) } } };
const PFn t_cmov[2][16][16] = { { R16(ROW_RR, cmov_32) }, { R16(ROW_RR, cmov_64) } };
const PFn t_cmovt[2][16] = { { R16(E1, cmovt_32) }, { R16(E1, cmovt_64) } };
