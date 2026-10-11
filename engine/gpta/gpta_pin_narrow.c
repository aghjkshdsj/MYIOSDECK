// SPDX-License-Identifier: GPL-3.0-or-later
// Byte/word register operations preserve the other bits of the guest GPR.
// H0..H3 denote AH/CH/DH/BH; decoding selects them only for byte operands.
#include "gpta_pin.h"

#define NG_0 g0
#define NG_1 g1
#define NG_2 g2
#define NG_3 g3
#define NG_4 g4
#define NG_5 g5
#define NG_6 g6
#define NG_7 g7
#define NG_8 g8
#define NG_9 g9
#define NG_10 g10
#define NG_11 g11
#define NG_12 g12
#define NG_13 g13
#define NG_14 g14
#define NG_15 g15
#define NG_H0 g0
#define NG_H1 g1
#define NG_H2 g2
#define NG_H3 g3
#define NS_0 0
#define NS_1 0
#define NS_2 0
#define NS_3 0
#define NS_4 0
#define NS_5 0
#define NS_6 0
#define NS_7 0
#define NS_8 0
#define NS_9 0
#define NS_10 0
#define NS_11 0
#define NS_12 0
#define NS_13 0
#define NS_14 0
#define NS_15 0
#define NS_H0 8
#define NS_H1 8
#define NS_H2 8
#define NS_H3 8
#define NR(D, SZ) ((NG_##D >> NS_##D) & M##SZ)
#define NW(D, SZ, V) (NG_##D = (NG_##D & ~(M##SZ << NS_##D)) | (((V) & M##SZ) << NS_##D))
#define N20(M, ...) R16(M, __VA_ARGS__) M(H0, __VA_ARGS__) M(H1, __VA_ARGS__) M(H2, __VA_ARGS__) M(H3, __VA_ARGS__)
#define N20B(M, ...) R16B(M, __VA_ARGS__) M(H0, __VA_ARGS__) M(H1, __VA_ARGS__) M(H2, __VA_ARGS__) M(H3, __VA_ARGS__)

#define NALU(OP, SZ, D, B, FL) \
    uint64_t ci = 0; \
    if ((OP) == ALU_ADC || (OP) == ALU_SBB) { \
        int slow = 0; ci = pcf(F0, F1, F2, F3, &slow); if (FXI_UNLIKELY(slow)) PTAIL(p_slow); \
    } \
    uint64_t a = NR(D, SZ), b = (B) & M##SZ; \
    uint64_t r = ((OP) == ALU_ADD || (OP) == ALU_ADC ? a + b + ci : \
        (OP) == ALU_SUB || (OP) == ALU_SBB || (OP) == ALU_CMP ? a - b - ci : \
        (OP) == ALU_OR ? a | b : (OP) == ALU_XOR ? a ^ b : a & b) & M##SZ; \
    if ((OP) != ALU_CMP && (OP) != ALU_TEST) NW(D, SZ, r); \
    if (FL) { \
        SETF((OP) == ALU_ADD ? LF_ADD : (OP) == ALU_ADC ? LF_ADC : \
            (OP) == ALU_SBB ? LF_SBB : (OP) == ALU_SUB || (OP) == ALU_CMP ? LF_SUB : LF_LOGIC, \
            SI##SZ, a, b, r, ci); \
    } \
    PNEXT();
#define DEF_NRR(S, D, SZ, OP, FL) PH p_nrr_##OP##_##SZ##_##FL##_##D##_##S(GPTA_PARAMS) { NALU(OP, SZ, D, NR(S, SZ), FL) }
#define DEF_ND(D, SZ, OP, FL) \
    N20B(DEF_NRR, D, SZ, OP, FL) \
    PH p_nri_##OP##_##SZ##_##FL##_##D(GPTA_PARAMS) { NALU(OP, SZ, D, u->imm, FL) }
#define DEF_NOP(OP, FL) N20(DEF_ND, 8, OP, FL) N20(DEF_ND, 16, OP, FL)
#define NOPS(M, FL) M(ALU_ADD, FL) M(ALU_OR, FL) M(ALU_ADC, FL) M(ALU_SBB, FL) M(ALU_AND, FL) M(ALU_SUB, FL) M(ALU_XOR, FL) M(ALU_CMP, FL) M(ALU_TEST, FL)
NOPS(DEF_NOP, 0) NOPS(DEF_NOP, 1)
#define E_NRR(S, D, SZ, OP, FL) p_nrr_##OP##_##SZ##_##FL##_##D##_##S,
#define ROW_NRR(D, SZ, OP, FL) { N20B(E_NRR, D, SZ, OP, FL) },
#define T_NRR(OP, FL) { { N20(ROW_NRR, 8, OP, FL) }, { N20(ROW_NRR, 16, OP, FL) } },
#define E_NRI(D, SZ, OP, FL) p_nri_##OP##_##SZ##_##FL##_##D,
#define T_NRI(OP, FL) { { N20(E_NRI, 8, OP, FL) }, { N20(E_NRI, 16, OP, FL) } },
const PFn t_nrr[2][ALU_COUNT][2][20][20] = { { NOPS(T_NRR, 0) }, { NOPS(T_NRR, 1) } };
const PFn t_nri[2][ALU_COUNT][2][20] = { { NOPS(T_NRI, 0) }, { NOPS(T_NRI, 1) } };

#define DEF_NMOV_S(S, D, SZ) PH p_nmov_##SZ##_##D##_##S(GPTA_PARAMS) { uint64_t v = NR(S, SZ); NW(D, SZ, v); PNEXT(); }
#define DEF_NMOV_D(D, SZ) N20B(DEF_NMOV_S, D, SZ) \
    PH p_nmov_i_##SZ##_##D(GPTA_PARAMS) { NW(D, SZ, u->imm); PNEXT(); }
N20(DEF_NMOV_D, 8) N20(DEF_NMOV_D, 16)
#define E_NMOV_S(S, D, SZ) p_nmov_##SZ##_##D##_##S,
#define ROW_NMOV(D, SZ) { N20B(E_NMOV_S, D, SZ) },
#define E_NMOV_I(D, SZ) p_nmov_i_##SZ##_##D,
const PFn t_nmov[2][20][20] = { { N20(ROW_NMOV, 8) }, { N20(ROW_NMOV, 16) } };
const PFn t_nmovi[2][20] = { { N20(E_NMOV_I, 8) }, { N20(E_NMOV_I, 16) } };
