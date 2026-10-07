// SPDX-License-Identifier: GPL-3.0-or-later
// FXI integer handlers. Generated per operation x operand form x size, each a
// straight-line function that ends by tail-calling the next uop: no dispatch
// loop, no runtime operand decoding. "F" variants record lazy flags; "N"
// variants (chosen by the decoder when the flags are dead) do not.

#include "fxi_internal.h"

#define M8 0xffull
#define M16 0xffffull
#define M32 0xffffffffull
#define M64 0xffffffffffffffffull
#define SB8 0x80ull
#define SB16 0x8000ull
#define SB32 0x80000000ull
#define SB64 0x8000000000000000ull
#define SI8 0
#define SI16 1
#define SI32 2
#define SI64 3
#define ST8 int8_t
#define ST16 int16_t
#define ST32 int32_t
#define ST64 int64_t
#define FLV_F 1
#define FLV_N 0

// ---------------------------------------------------------------------------
// ALU: add or adc sbb and sub xor cmp test
// ---------------------------------------------------------------------------
#define LOAD_RR(S) uint64_t a = rd##S(c, u->dst), b = rd##S(c, u->src); uint64_t addr = 0; (void)addr;
#define LOAD_RI(S) uint64_t a = rd##S(c, u->dst), b = u->imm & M##S; uint64_t addr = 0; (void)addr;
#define LOAD_RM(S) uint64_t a = rd##S(c, u->dst), b = ld##S(fxi_ea(c, u)); uint64_t addr = 0; (void)addr;
#define LOAD_MR(S) uint64_t addr = fxi_ea(c, u); uint64_t a = ld##S(addr), b = rd##S(c, u->src);
#define LOAD_MI(S) uint64_t addr = fxi_ea(c, u); uint64_t a = ld##S(addr), b = u->imm & M##S;
#define STORE_RR(S) wr##S(c, u->dst, res);
#define STORE_RI(S) wr##S(c, u->dst, res);
#define STORE_RM(S) wr##S(c, u->dst, res);
#define STORE_MR(S) st##S(addr, res);
#define STORE_MI(S) st##S(addr, res);

#define DEF_ALU1(OPN, KIND, EXPR, WR, CIN, FORM, S, FL)                                   \
    static void alu_##OPN##_##FORM##_##S##_##FL(FxiCpu *c, Uop *u) {                      \
        uint64_t cin = (CIN) ? fxi_flag_cf(c) : 0;                                        \
        LOAD_##FORM(S)                                                                    \
        uint64_t res = (EXPR) & M##S;                                                     \
        if (WR) { STORE_##FORM(S) }                                                       \
        if (FLV_##FL) {                                                                   \
            c->lf_op = LF(KIND, SI##S); c->lf_a = a; c->lf_b = b; c->lf_res = res;        \
            c->lf_cin = (uint32_t)cin;                                                    \
        }                                                                                 \
        (void)cin;                                                                        \
        FXI_NEXT(c, u);                                                                   \
    }
#define DEF_ALU_FS(OPN, KIND, EXPR, WR, CIN, FORM, S) \
    DEF_ALU1(OPN, KIND, EXPR, WR, CIN, FORM, S, F) DEF_ALU1(OPN, KIND, EXPR, WR, CIN, FORM, S, N)
#define DEF_ALU_F(OPN, KIND, EXPR, WR, CIN, FORM)                                          \
    DEF_ALU_FS(OPN, KIND, EXPR, WR, CIN, FORM, 8) DEF_ALU_FS(OPN, KIND, EXPR, WR, CIN, FORM, 16) \
    DEF_ALU_FS(OPN, KIND, EXPR, WR, CIN, FORM, 32) DEF_ALU_FS(OPN, KIND, EXPR, WR, CIN, FORM, 64)
#define DEF_ALU(OPN, KIND, EXPR, WR, CIN)                                                \
    DEF_ALU_F(OPN, KIND, EXPR, WR, CIN, RR) DEF_ALU_F(OPN, KIND, EXPR, WR, CIN, RI)       \
    DEF_ALU_F(OPN, KIND, EXPR, WR, CIN, RM) DEF_ALU_F(OPN, KIND, EXPR, WR, CIN, MR)       \
    DEF_ALU_F(OPN, KIND, EXPR, WR, CIN, MI)

DEF_ALU(add, LF_ADD, a + b, 1, 0)
DEF_ALU(or, LF_LOGIC, a | b, 1, 0)
DEF_ALU(adc, LF_ADC, a + b + cin, 1, 1)
DEF_ALU(sbb, LF_SBB, a - b - cin, 1, 1)
DEF_ALU(and, LF_LOGIC, a & b, 1, 0)
DEF_ALU(sub, LF_SUB, a - b, 1, 0)
DEF_ALU(xor, LF_LOGIC, a ^ b, 1, 0)
DEF_ALU(cmp, LF_SUB, a - b, 0, 0)
DEF_ALU(test, LF_LOGIC, a & b, 0, 0)

#define TAB_ALU_S(OPN, FORM, S) { alu_##OPN##_##FORM##_##S##_N, alu_##OPN##_##FORM##_##S##_F }
#define TAB_ALU_F(OPN, FORM) { TAB_ALU_S(OPN, FORM, 8), TAB_ALU_S(OPN, FORM, 16), TAB_ALU_S(OPN, FORM, 32), TAB_ALU_S(OPN, FORM, 64) }
#define TAB_ALU(OPN) { TAB_ALU_F(OPN, RR), TAB_ALU_F(OPN, RI), TAB_ALU_F(OPN, RM), TAB_ALU_F(OPN, MR), TAB_ALU_F(OPN, MI) }

const OpFn fxi_alu_tab[ALU_COUNT][F_COUNT][4][2] = {
    [ALU_ADD] = TAB_ALU(add), [ALU_OR] = TAB_ALU(or), [ALU_ADC] = TAB_ALU(adc), [ALU_SBB] = TAB_ALU(sbb),
    [ALU_AND] = TAB_ALU(and), [ALU_SUB] = TAB_ALU(sub), [ALU_XOR] = TAB_ALU(xor), [ALU_CMP] = TAB_ALU(cmp),
    [ALU_TEST] = TAB_ALU(test),
};

// ---------------------------------------------------------------------------
// MOV, LEA
// ---------------------------------------------------------------------------
#define DEF_MOV(S)                                                                                            \
    static void mov_RR_##S(FxiCpu *c, Uop *u) { wr##S(c, u->dst, rd##S(c, u->src)); FXI_NEXT(c, u); }         \
    static void mov_RI_##S(FxiCpu *c, Uop *u) { wr##S(c, u->dst, u->imm); FXI_NEXT(c, u); }                   \
    static void mov_RM_##S(FxiCpu *c, Uop *u) { wr##S(c, u->dst, ld##S(fxi_ea(c, u))); FXI_NEXT(c, u); }      \
    static void mov_MR_##S(FxiCpu *c, Uop *u) { st##S(fxi_ea(c, u), rd##S(c, u->src)); FXI_NEXT(c, u); }      \
    static void mov_MI_##S(FxiCpu *c, Uop *u) { st##S(fxi_ea(c, u), u->imm); FXI_NEXT(c, u); }
DEF_MOV(8) DEF_MOV(16) DEF_MOV(32) DEF_MOV(64)
#define TAB_MOV(F) { mov_##F##_8, mov_##F##_16, mov_##F##_32, mov_##F##_64 }
const OpFn fxi_mov_tab[F_COUNT][4] = { TAB_MOV(RR), TAB_MOV(RI), TAB_MOV(RM), TAB_MOV(MR), TAB_MOV(MI) };

static void lea_16(FxiCpu *c, Uop *u) { wr16(c, u->dst, fxi_ea(c, u)); FXI_NEXT(c, u); }
static void lea_32(FxiCpu *c, Uop *u) { wr32(c, u->dst, fxi_ea(c, u)); FXI_NEXT(c, u); }
static void lea_64(FxiCpu *c, Uop *u) { wr64(c, u->dst, fxi_ea(c, u)); FXI_NEXT(c, u); }
const OpFn fxi_lea_tab[4] = { 0, lea_16, lea_32, lea_64 };

// ---------------------------------------------------------------------------
// Shifts and rotates. Count: u->src == 0xffff means CL, else u->imm.
// A zero count leaves flags (and, for 32-bit regs, x86 still zero-extends) alone.
// ---------------------------------------------------------------------------
#define SH_COUNT(S) (((u->src == 0xffff) ? (unsigned)c->r[R_CX] : (unsigned)u->imm) & ((S) == 64 ? 63u : 31u))
#define SH_LOAD_R(S) uint64_t addr = 0; (void)addr; uint64_t a = rd##S(c, u->dst);
#define SH_LOAD_M(S) uint64_t addr = fxi_ea(c, u); uint64_t a = ld##S(addr);
#define SH_STORE_R(S) wr##S(c, u->dst, res);
#define SH_STORE_M(S) st##S(addr, res);

#define DEF_SHIFT(OPN, KIND, EXPR, FORM, S)                                                \
    static void sh_##OPN##_##FORM##_##S(FxiCpu *c, Uop *u) {                               \
        unsigned n = SH_COUNT(S);                                                          \
        const unsigned sbits = (S); (void)sbits;                                           \
        SH_LOAD_##FORM(S)                                                                  \
        if (FXI_UNLIKELY(n == 0)) FXI_NEXT(c, u);                                          \
        uint64_t res = (EXPR) & M##S;                                                      \
        SH_STORE_##FORM(S)                                                                 \
        c->lf_op = LF(KIND, SI##S); c->lf_a = a; c->lf_b = n; c->lf_res = res;             \
        FXI_NEXT(c, u);                                                                    \
    }
#define DEF_SHIFT_S(OPN, KIND, EXPR, S) DEF_SHIFT(OPN, KIND, EXPR, R, S) DEF_SHIFT(OPN, KIND, EXPR, M, S)
#define DEF_SHIFT_ALL(OPN, KIND, EXPR) \
    DEF_SHIFT_S(OPN, KIND, EXPR, 8) DEF_SHIFT_S(OPN, KIND, EXPR, 16) DEF_SHIFT_S(OPN, KIND, EXPR, 32) DEF_SHIFT_S(OPN, KIND, EXPR, 64)

DEF_SHIFT_ALL(shl, LF_SHL, n >= 64 ? 0 : a << n)
DEF_SHIFT_ALL(shr, LF_SHR, n >= 64 ? 0 : a >> n)
DEF_SHIFT_ALL(sar, LF_SAR, (uint64_t)((((int64_t)(a << (64 - sbits))) >> (64 - sbits)) >> (n > 63 ? 63 : n)))

// Rotates touch only CF/OF: materialise the other flags, then set those two.
#define DEF_ROT(OPN, IS_LEFT, FORM, S)                                                     \
    static void sh_##OPN##_##FORM##_##S(FxiCpu *c, Uop *u) {                               \
        unsigned n = SH_COUNT(S);                                                          \
        SH_LOAD_##FORM(S)                                                                  \
        if (FXI_UNLIKELY(n == 0)) FXI_NEXT(c, u);                                          \
        unsigned k = n % (S);                                                              \
        uint64_t res = k == 0 ? a : (IS_LEFT ? ((a << k) | (a >> ((S) - k))) : ((a >> k) | (a << ((S) - k)))) & M##S; \
        SH_STORE_##FORM(S)                                                                 \
        if (u->cc) { /* flags live */                                                      \
            uint64_t f = fxi_rflags(c) & ~0x801ull;                                        \
            uint64_t cf = IS_LEFT ? (res & 1) : ((res >> ((S) - 1)) & 1);                  \
            uint64_t of = IS_LEFT ? (((res >> ((S) - 1)) & 1) ^ cf)                        \
                                  : (((res >> ((S) - 1)) ^ (res >> ((S) - 2))) & 1);       \
            fxi_set_rflags(c, f | cf | of << 11);                                          \
        }                                                                                  \
        FXI_NEXT(c, u);                                                                    \
    }
#define DEF_ROT_S(OPN, L, S) DEF_ROT(OPN, L, R, S) DEF_ROT(OPN, L, M, S)
DEF_ROT_S(rol, 1, 8) DEF_ROT_S(rol, 1, 16) DEF_ROT_S(rol, 1, 32) DEF_ROT_S(rol, 1, 64)
DEF_ROT_S(ror, 0, 8) DEF_ROT_S(ror, 0, 16) DEF_ROT_S(ror, 0, 32) DEF_ROT_S(ror, 0, 64)

// RCL/RCR: rare in compiled code; a straightforward bit loop.
#define DEF_RC(OPN, IS_LEFT, FORM, S)                                                      \
    static void sh_##OPN##_##FORM##_##S(FxiCpu *c, Uop *u) {                               \
        unsigned n = SH_COUNT(S) % ((S) + 1);                                              \
        SH_LOAD_##FORM(S)                                                                  \
        if (n == 0) FXI_NEXT(c, u);                                                        \
        uint64_t f = fxi_rflags(c), cf = f & 1, res = a;                                   \
        for (unsigned i = 0; i < n; i++) {                                                 \
            if (IS_LEFT) { uint64_t out = (res >> ((S) - 1)) & 1; res = ((res << 1) | cf) & M##S; cf = out; } \
            else { uint64_t out = res & 1; res = (res >> 1) | (cf << ((S) - 1)); cf = out; } \
        }                                                                                  \
        SH_STORE_##FORM(S)                                                                 \
        uint64_t of = IS_LEFT ? (((res >> ((S) - 1)) & 1) ^ cf) : (((res >> ((S) - 1)) ^ (res >> ((S) - 2))) & 1); \
        fxi_set_rflags(c, (f & ~0x801ull) | cf | of << 11);                                \
        FXI_NEXT(c, u);                                                                    \
    }
#define DEF_RC_S(OPN, L, S) DEF_RC(OPN, L, R, S) DEF_RC(OPN, L, M, S)
DEF_RC_S(rcl, 1, 8) DEF_RC_S(rcl, 1, 16) DEF_RC_S(rcl, 1, 32) DEF_RC_S(rcl, 1, 64)
DEF_RC_S(rcr, 0, 8) DEF_RC_S(rcr, 0, 16) DEF_RC_S(rcr, 0, 32) DEF_RC_S(rcr, 0, 64)

#define TAB_SH(OPN) { { sh_##OPN##_R_8, sh_##OPN##_R_16, sh_##OPN##_R_32, sh_##OPN##_R_64 }, \
                      { sh_##OPN##_M_8, sh_##OPN##_M_16, sh_##OPN##_M_32, sh_##OPN##_M_64 } }
const OpFn fxi_shift_tab[8][2][4] = {
    [SH_ROL] = TAB_SH(rol), [SH_ROR] = TAB_SH(ror), [SH_RCL] = TAB_SH(rcl), [SH_RCR] = TAB_SH(rcr),
    [SH_SHL] = TAB_SH(shl), [SH_SHR] = TAB_SH(shr), [SH_SAL] = TAB_SH(shl), [SH_SAR] = TAB_SH(sar),
};

// ---------------------------------------------------------------------------
// INC DEC NOT NEG. u->cc != 0: flags live (INC/DEC then also keep CF).
// ---------------------------------------------------------------------------
#define DEF_UNARY(OPN, FORM, S, BODY)                                                      \
    static void un_##OPN##_##FORM##_##S(FxiCpu *c, Uop *u) {                               \
        SH_LOAD_##FORM(S)                                                                  \
        BODY                                                                               \
        FXI_NEXT(c, u);                                                                    \
    }
#define UN_INC(S, FORM) uint64_t res = (a + 1) & M##S; SH_STORE_##FORM(S) \
    if (u->cc) { uint32_t cf = (uint32_t)fxi_flag_cf(c); c->lf_op = LF(LF_INC, SI##S); c->lf_a = a; c->lf_b = 1; c->lf_res = res; c->lf_cin = cf; }
#define UN_DEC(S, FORM) uint64_t res = (a - 1) & M##S; SH_STORE_##FORM(S) \
    if (u->cc) { uint32_t cf = (uint32_t)fxi_flag_cf(c); c->lf_op = LF(LF_DEC, SI##S); c->lf_a = a; c->lf_b = 1; c->lf_res = res; c->lf_cin = cf; }
#define UN_NOT(S, FORM) uint64_t res = ~a & M##S; SH_STORE_##FORM(S)
#define UN_NEG(S, FORM) uint64_t res = (0 - a) & M##S; SH_STORE_##FORM(S) \
    if (u->cc) { c->lf_op = LF(LF_NEG, SI##S); c->lf_a = a; c->lf_b = 0; c->lf_res = res; }
#define DEF_UNARY_S(OPN, MAC, S) DEF_UNARY(OPN, R, S, MAC(S, R)) DEF_UNARY(OPN, M, S, MAC(S, M))
#define DEF_UNARY_ALL(OPN, MAC) DEF_UNARY_S(OPN, MAC, 8) DEF_UNARY_S(OPN, MAC, 16) DEF_UNARY_S(OPN, MAC, 32) DEF_UNARY_S(OPN, MAC, 64)
DEF_UNARY_ALL(inc, UN_INC)
DEF_UNARY_ALL(dec, UN_DEC)
DEF_UNARY_ALL(not, UN_NOT)
DEF_UNARY_ALL(neg, UN_NEG)
#define TAB_UN(OPN) { { un_##OPN##_R_8, un_##OPN##_R_16, un_##OPN##_R_32, un_##OPN##_R_64 }, \
                      { un_##OPN##_M_8, un_##OPN##_M_16, un_##OPN##_M_32, un_##OPN##_M_64 } }
const OpFn fxi_unary_tab[4][2][4] = { TAB_UN(inc), TAB_UN(dec), TAB_UN(not), TAB_UN(neg) };

// ---------------------------------------------------------------------------
// MOVZX / MOVSX / MOVSXD: [signed][R/M][src 8/16/32][dst size]
// ---------------------------------------------------------------------------
#define EXT_SRC_R(SS) rd##SS(c, u->src)
#define EXT_SRC_M(SS) ld##SS(fxi_ea(c, u))
#define DEF_EXT(SG, FORM, SS, DS)                                                          \
    static void ext_##SG##_##FORM##_##SS##_##DS(FxiCpu *c, Uop *u) {                       \
        uint64_t v = EXT_SRC_##FORM(SS);                                                   \
        if (SG) v = (uint64_t)(int64_t)(ST##SS)v;                                          \
        wr##DS(c, u->dst, v);                                                              \
        FXI_NEXT(c, u);                                                                    \
    }
#define DEF_EXT_D(SG, FORM, SS) DEF_EXT(SG, FORM, SS, 16) DEF_EXT(SG, FORM, SS, 32) DEF_EXT(SG, FORM, SS, 64)
#define DEF_EXT_F(SG, SS) DEF_EXT_D(SG, R, SS) DEF_EXT_D(SG, M, SS)
DEF_EXT_F(0, 8) DEF_EXT_F(0, 16) DEF_EXT_F(0, 32) DEF_EXT_F(1, 8) DEF_EXT_F(1, 16) DEF_EXT_F(1, 32)
#define TAB_EXT_D(SG, FORM, SS) { 0, ext_##SG##_##FORM##_##SS##_16, ext_##SG##_##FORM##_##SS##_32, ext_##SG##_##FORM##_##SS##_64 }
#define TAB_EXT_F(SG, FORM) { TAB_EXT_D(SG, FORM, 8), TAB_EXT_D(SG, FORM, 16), TAB_EXT_D(SG, FORM, 32) }
const OpFn fxi_ext_tab[2][2][3][4] = { { TAB_EXT_F(0, R), TAB_EXT_F(0, M) }, { TAB_EXT_F(1, R), TAB_EXT_F(1, M) } };

// ---------------------------------------------------------------------------
// CMOVcc (a 32-bit cmov always zero-extends its destination), SETcc
// ---------------------------------------------------------------------------
#define DEF_CMOV(FORM, S)                                                                  \
    static void cmov_##FORM##_##S(FxiCpu *c, Uop *u) {                                     \
        uint64_t v = EXT_SRC_##FORM(S);                                                    \
        wr##S(c, u->dst, fxi_cond(c, u->cc) ? v : rd##S(c, u->dst));                       \
        FXI_NEXT(c, u);                                                                    \
    }
DEF_CMOV(R, 16) DEF_CMOV(R, 32) DEF_CMOV(R, 64) DEF_CMOV(M, 16) DEF_CMOV(M, 32) DEF_CMOV(M, 64)
const OpFn fxi_cmov_tab[2][4] = { { 0, cmov_R_16, cmov_R_32, cmov_R_64 }, { 0, cmov_M_16, cmov_M_32, cmov_M_64 } };

static void setcc_R(FxiCpu *c, Uop *u) { wr8(c, u->dst, (uint64_t)fxi_cond(c, u->cc)); FXI_NEXT(c, u); }
static void setcc_M(FxiCpu *c, Uop *u) { st8(fxi_ea(c, u), (uint64_t)fxi_cond(c, u->cc)); FXI_NEXT(c, u); }
const OpFn fxi_setcc_tab[2] = { setcc_R, setcc_M };

// ---------------------------------------------------------------------------
// MUL IMUL DIV IDIV (one operand, rDX:rAX), IMUL two/three operand
// ---------------------------------------------------------------------------
static void fxi_div0(FxiCpu *c, Uop *u) { c->rip = u->aux; fxi_fail(c, "divide error (#DE) at %#llx", (unsigned long long)u->aux); }

#define MD_SRC_R(S) rd##S(c, u->src)
#define MD_SRC_M(S) ld##S(fxi_ea(c, u))
#define DEF_MULDIV(FORM)                                                                   \
    static void mul_##FORM##_8(FxiCpu *c, Uop *u) {                                        \
        uint64_t r = (uint64_t)(uint8_t)c->r[R_AX] * MD_SRC_##FORM(8); wr16(c, R_AX * 8, r); \
        c->lf_op = LF(LF_MUL, 0); c->lf_res = r; c->lf_cin = (r >> 8) != 0; FXI_NEXT(c, u); } \
    static void mul_##FORM##_16(FxiCpu *c, Uop *u) {                                       \
        uint64_t r = (uint64_t)(uint16_t)c->r[R_AX] * MD_SRC_##FORM(16);                   \
        wr16(c, R_AX * 8, r); wr16(c, R_DX * 8, r >> 16);                                  \
        c->lf_op = LF(LF_MUL, 1); c->lf_res = r; c->lf_cin = (r >> 16) != 0; FXI_NEXT(c, u); } \
    static void mul_##FORM##_32(FxiCpu *c, Uop *u) {                                       \
        uint64_t r = (uint64_t)(uint32_t)c->r[R_AX] * MD_SRC_##FORM(32);                   \
        wr32(c, R_AX * 8, r); wr32(c, R_DX * 8, r >> 32);                                  \
        c->lf_op = LF(LF_MUL, 2); c->lf_res = r; c->lf_cin = (r >> 32) != 0; FXI_NEXT(c, u); } \
    static void mul_##FORM##_64(FxiCpu *c, Uop *u) {                                       \
        unsigned __int128 r = (unsigned __int128)c->r[R_AX] * MD_SRC_##FORM(64);           \
        c->r[R_AX] = (uint64_t)r; c->r[R_DX] = (uint64_t)(r >> 64);                        \
        c->lf_op = LF(LF_MUL, 3); c->lf_res = (uint64_t)r; c->lf_cin = c->r[R_DX] != 0; FXI_NEXT(c, u); } \
    static void imul1_##FORM##_8(FxiCpu *c, Uop *u) {                                      \
        int64_t r = (int64_t)(int8_t)c->r[R_AX] * (int8_t)MD_SRC_##FORM(8); wr16(c, R_AX * 8, (uint64_t)r); \
        c->lf_op = LF(LF_MUL, 0); c->lf_res = (uint64_t)r; c->lf_cin = r != (int8_t)r; FXI_NEXT(c, u); } \
    static void imul1_##FORM##_16(FxiCpu *c, Uop *u) {                                     \
        int64_t r = (int64_t)(int16_t)c->r[R_AX] * (int16_t)MD_SRC_##FORM(16);             \
        wr16(c, R_AX * 8, (uint64_t)r); wr16(c, R_DX * 8, (uint64_t)r >> 16);              \
        c->lf_op = LF(LF_MUL, 1); c->lf_res = (uint64_t)r; c->lf_cin = r != (int16_t)r; FXI_NEXT(c, u); } \
    static void imul1_##FORM##_32(FxiCpu *c, Uop *u) {                                     \
        int64_t r = (int64_t)(int32_t)c->r[R_AX] * (int32_t)MD_SRC_##FORM(32);             \
        wr32(c, R_AX * 8, (uint64_t)r); wr32(c, R_DX * 8, (uint64_t)r >> 32);              \
        c->lf_op = LF(LF_MUL, 2); c->lf_res = (uint64_t)r; c->lf_cin = r != (int32_t)r; FXI_NEXT(c, u); } \
    static void imul1_##FORM##_64(FxiCpu *c, Uop *u) {                                     \
        __int128 r = (__int128)(int64_t)c->r[R_AX] * (int64_t)MD_SRC_##FORM(64);           \
        c->r[R_AX] = (uint64_t)r; c->r[R_DX] = (uint64_t)((unsigned __int128)r >> 64);     \
        c->lf_op = LF(LF_MUL, 3); c->lf_res = (uint64_t)r; c->lf_cin = r != (int64_t)r; FXI_NEXT(c, u); } \
    static void div_##FORM##_8(FxiCpu *c, Uop *u) {                                        \
        uint64_t d = MD_SRC_##FORM(8), n = (uint16_t)c->r[R_AX];                           \
        if (FXI_UNLIKELY(!d || n / d > 0xff)) { fxi_div0(c, u); return; }                  \
        wr8(c, R_AX * 8, n / d); wr8(c, R_AX * 8 + 1, n % d); FXI_NEXT(c, u); }            \
    static void div_##FORM##_16(FxiCpu *c, Uop *u) {                                       \
        uint64_t d = MD_SRC_##FORM(16), n = (uint64_t)(uint16_t)c->r[R_DX] << 16 | (uint16_t)c->r[R_AX]; \
        if (FXI_UNLIKELY(!d || n / d > 0xffff)) { fxi_div0(c, u); return; }                \
        wr16(c, R_AX * 8, n / d); wr16(c, R_DX * 8, n % d); FXI_NEXT(c, u); }              \
    static void div_##FORM##_32(FxiCpu *c, Uop *u) {                                       \
        uint64_t d = MD_SRC_##FORM(32), n = (uint64_t)(uint32_t)c->r[R_DX] << 32 | (uint32_t)c->r[R_AX]; \
        if (FXI_UNLIKELY(!d || n / d > 0xffffffffull)) { fxi_div0(c, u); return; }         \
        wr32(c, R_AX * 8, n / d); wr32(c, R_DX * 8, n % d); FXI_NEXT(c, u); }              \
    static void div_##FORM##_64(FxiCpu *c, Uop *u) {                                       \
        uint64_t d = MD_SRC_##FORM(64);                                                    \
        unsigned __int128 n = (unsigned __int128)c->r[R_DX] << 64 | c->r[R_AX];            \
        if (FXI_UNLIKELY(!d || (n / d) >> 64)) { fxi_div0(c, u); return; }                 \
        c->r[R_AX] = (uint64_t)(n / d); c->r[R_DX] = (uint64_t)(n % d); FXI_NEXT(c, u); }  \
    static void idiv_##FORM##_8(FxiCpu *c, Uop *u) {                                       \
        int64_t d = (int8_t)MD_SRC_##FORM(8), n = (int16_t)c->r[R_AX];                     \
        if (FXI_UNLIKELY(!d || n / d > 127 || n / d < -128)) { fxi_div0(c, u); return; }   \
        wr8(c, R_AX * 8, (uint64_t)(n / d)); wr8(c, R_AX * 8 + 1, (uint64_t)(n % d)); FXI_NEXT(c, u); } \
    static void idiv_##FORM##_16(FxiCpu *c, Uop *u) {                                      \
        int64_t d = (int16_t)MD_SRC_##FORM(16), n = (int32_t)((uint32_t)(uint16_t)c->r[R_DX] << 16 | (uint16_t)c->r[R_AX]); \
        if (FXI_UNLIKELY(!d || n / d > 32767 || n / d < -32768)) { fxi_div0(c, u); return; } \
        wr16(c, R_AX * 8, (uint64_t)(n / d)); wr16(c, R_DX * 8, (uint64_t)(n % d)); FXI_NEXT(c, u); } \
    static void idiv_##FORM##_32(FxiCpu *c, Uop *u) {                                      \
        int64_t d = (int32_t)MD_SRC_##FORM(32), n = (int64_t)((uint64_t)(uint32_t)c->r[R_DX] << 32 | (uint32_t)c->r[R_AX]); \
        if (FXI_UNLIKELY(!d || n / d > 2147483647ll || n / d < -2147483648ll)) { fxi_div0(c, u); return; } \
        wr32(c, R_AX * 8, (uint64_t)(n / d)); wr32(c, R_DX * 8, (uint64_t)(n % d)); FXI_NEXT(c, u); } \
    static void idiv_##FORM##_64(FxiCpu *c, Uop *u) {                                      \
        int64_t d = (int64_t)MD_SRC_##FORM(64);                                            \
        __int128 n = (__int128)((unsigned __int128)c->r[R_DX] << 64 | c->r[R_AX]);         \
        if (FXI_UNLIKELY(!d || n / d > INT64_MAX || n / d < INT64_MIN)) { fxi_div0(c, u); return; } \
        c->r[R_AX] = (uint64_t)(n / d); c->r[R_DX] = (uint64_t)(n % d); FXI_NEXT(c, u); }
DEF_MULDIV(R) DEF_MULDIV(M)
#define TAB_MD(OPN) { { OPN##_R_8, OPN##_R_16, OPN##_R_32, OPN##_R_64 }, { OPN##_M_8, OPN##_M_16, OPN##_M_32, OPN##_M_64 } }
const OpFn fxi_muldiv_tab[4][2][4] = { TAB_MD(mul), TAB_MD(imul1), TAB_MD(div), TAB_MD(idiv) };

#define DEF_IMUL2(FORM, S)                                                                 \
    static void imul2_##FORM##_##S(FxiCpu *c, Uop *u) {                                    \
        int64_t a = (ST##S)rd##S(c, u->dst), b = (ST##S)EXT_SRC_##FORM(S);                 \
        __int128 r = (__int128)a * b;                                                      \
        wr##S(c, u->dst, (uint64_t)r);                                                     \
        c->lf_op = LF(LF_MUL, SI##S); c->lf_res = (uint64_t)r; c->lf_cin = r != (ST##S)r;  \
        FXI_NEXT(c, u);                                                                    \
    }                                                                                      \
    static void imul3_##FORM##_##S(FxiCpu *c, Uop *u) {                                    \
        int64_t a = (ST##S)EXT_SRC_##FORM(S), b = (ST##S)u->imm;                           \
        __int128 r = (__int128)a * b;                                                      \
        wr##S(c, u->dst, (uint64_t)r);                                                     \
        c->lf_op = LF(LF_MUL, SI##S); c->lf_res = (uint64_t)r; c->lf_cin = r != (ST##S)r;  \
        FXI_NEXT(c, u);                                                                    \
    }
DEF_IMUL2(R, 16) DEF_IMUL2(R, 32) DEF_IMUL2(R, 64) DEF_IMUL2(M, 16) DEF_IMUL2(M, 32) DEF_IMUL2(M, 64)
const OpFn fxi_imul2_tab[2][4] = { { 0, imul2_R_16, imul2_R_32, imul2_R_64 }, { 0, imul2_M_16, imul2_M_32, imul2_M_64 } };
const OpFn fxi_imul3_tab[2][4] = { { 0, imul3_R_16, imul3_R_32, imul3_R_64 }, { 0, imul3_M_16, imul3_M_32, imul3_M_64 } };

// ---------------------------------------------------------------------------
// Control flow. Successor blocks are chained into the uop on first use.
// ---------------------------------------------------------------------------
#define CHAIN(c, slot, rip_)                                                               \
    do {                                                                                   \
        Block *b_ = (slot);                                                                \
        if (FXI_UNLIKELY(!b_)) { b_ = fxi_lookup((c), (rip_)); if (b_ != fxi_stop) (slot) = b_; } \
        FXI_GOTO_BLOCK((c), b_);                                                           \
    } while (0)
// Indirect target with a one-entry inline cache (imm = cached rip, link = block).
#define INDIRECT(c, u, target)                                                             \
    do {                                                                                   \
        uint64_t t_ = (target);                                                            \
        if (FXI_LIKELY(t_ == (u)->imm && (u)->link)) FXI_GOTO_BLOCK((c), (u)->link);       \
        Block *b_ = fxi_lookup((c), t_);                                                   \
        if (b_ != fxi_stop) { (u)->imm = t_; (u)->link = b_; }                             \
        FXI_GOTO_BLOCK((c), b_);                                                           \
    } while (0)

static void op_jmp(FxiCpu *c, Uop *u) { CHAIN(c, u->link, u->imm); }
static void op_jcc(FxiCpu *c, Uop *u) {
    if (fxi_cond(c, u->cc)) CHAIN(c, u->link, u->imm);
    CHAIN(c, u->link2, u->aux);
}
static void op_call(FxiCpu *c, Uop *u) {
    c->r[R_SP] -= 8; st64(c->r[R_SP], u->aux);
    CHAIN(c, u->link, u->imm);
}
static void op_call_R(FxiCpu *c, Uop *u) {
    uint64_t t = c->r[u->src >> 3];
    c->r[R_SP] -= 8; st64(c->r[R_SP], u->aux);
    INDIRECT(c, u, t);
}
static void op_call_M(FxiCpu *c, Uop *u) {
    uint64_t t = ld64(fxi_ea(c, u));
    c->r[R_SP] -= 8; st64(c->r[R_SP], u->aux);
    INDIRECT(c, u, t);
}
static void op_jmp_R(FxiCpu *c, Uop *u) { INDIRECT(c, u, c->r[u->src >> 3]); }
static void op_jmp_M(FxiCpu *c, Uop *u) { INDIRECT(c, u, ld64(fxi_ea(c, u))); }
static void op_ret(FxiCpu *c, Uop *u) {
    uint64_t t = ld64(c->r[R_SP]);
    c->r[R_SP] += 8 + u->aux;   // aux: ret imm16
    INDIRECT(c, u, t);
}
// Block ended without a branch (length cap or before a syscall): continue at aux.
static void op_goto(FxiCpu *c, Uop *u) { CHAIN(c, u->link, u->aux); }
static void op_stop(FxiCpu *c, Uop *u) { (void)c; (void)u; }
// Windows mode: the next block is native ARM64EC code; leave for the transition glue.
static void op_ec_exit(FxiCpu *c, Uop *u) { c->rip = u->imm; c->stop = FXI_STOP_EC; }
static void op_fail_ud(FxiCpu *c, Uop *u) {
    c->rip = u->aux;
    fxi_fail(c, "unimplemented instruction at %#llx: %s", (unsigned long long)u->aux, (const char *)(uintptr_t)u->imm);
}
static void op_hlt(FxiCpu *c, Uop *u) { c->rip = u->aux; fxi_fail(c, "hlt/ud2/int3 at %#llx", (unsigned long long)u->aux); }

static void op_syscall(FxiCpu *c, Uop *u) {
    c->rip = u->aux;
    long r = fxi_syscall(c);
    if (c->stop) return;
    c->r[R_AX] = (uint64_t)r;
    c->r[R_CX] = u->aux;          // syscall clobbers RCX (return rip) and R11 (rflags)
    c->r[11] = fxi_rflags(c);
    CHAIN(c, u->link, u->aux);
}

// Fused cmp/test + jcc: [cmp/test][RR/RI][32/64][cc]. Flags are still recorded
// lazily because a successor block may read them.
#define CC_SUB(cc, a, b, S) (                                                              \
    (cc) == 0 ? (((a ^ b) & (a ^ ((a - b) & M##S))) & SB##S) != 0 :                        \
    (cc) == 1 ? (((a ^ b) & (a ^ ((a - b) & M##S))) & SB##S) == 0 :                        \
    (cc) == 2 ? a < b : (cc) == 3 ? a >= b : (cc) == 4 ? a == b : (cc) == 5 ? a != b :     \
    (cc) == 6 ? a <= b : (cc) == 7 ? a > b :                                               \
    (cc) == 8 ? (((a - b) & SB##S) != 0) : (cc) == 9 ? (((a - b) & SB##S) == 0) :          \
    (cc) == 10 ? !__builtin_parity((unsigned)((a - b) & 0xff)) : (cc) == 11 ? __builtin_parity((unsigned)((a - b) & 0xff)) : \
    (cc) == 12 ? (ST##S)a < (ST##S)b : (cc) == 13 ? (ST##S)a >= (ST##S)b :                 \
    (cc) == 14 ? (ST##S)a <= (ST##S)b : (ST##S)a > (ST##S)b)
#define CC_LOGIC(cc, r, S) (                                                               \
    (cc) == 0 ? 0 : (cc) == 1 ? 1 : (cc) == 2 ? 0 : (cc) == 3 ? 1 :                        \
    (cc) == 4 ? r == 0 : (cc) == 5 ? r != 0 : (cc) == 6 ? r == 0 : (cc) == 7 ? r != 0 :    \
    (cc) == 8 ? (r & SB##S) != 0 : (cc) == 9 ? (r & SB##S) == 0 :                          \
    (cc) == 10 ? !__builtin_parity((unsigned)(r & 0xff)) : (cc) == 11 ? __builtin_parity((unsigned)(r & 0xff)) : \
    (cc) == 12 ? (r & SB##S) != 0 : (cc) == 13 ? (r & SB##S) == 0 :                        \
    (cc) == 14 ? (r == 0 || (r & SB##S)) : (r != 0 && !(r & SB##S)))
#define FJ_B_RR(S) rd##S(c, u->src)
#define FJ_B_RI(S) (u->disp & M##S)          // the immediate rides in disp (imm is the target)
#define DEF_FJCC(FORM, S, CC)                                                              \
    static void fj_cmp_##FORM##_##S##_##CC(FxiCpu *c, Uop *u) {                            \
        uint64_t a = rd##S(c, u->dst), b = FJ_B_##FORM(S);                                 \
        c->lf_op = LF(LF_SUB, SI##S); c->lf_a = a; c->lf_b = b; c->lf_res = (a - b) & M##S; \
        if (CC_SUB(CC, a, b, S)) CHAIN(c, u->link, u->imm);                                \
        CHAIN(c, u->link2, u->aux);                                                        \
    }                                                                                      \
    static void fj_test_##FORM##_##S##_##CC(FxiCpu *c, Uop *u) {                           \
        uint64_t a = rd##S(c, u->dst), b = FJ_B_##FORM(S), r = a & b;                      \
        c->lf_op = LF(LF_LOGIC, SI##S); c->lf_a = a; c->lf_b = b; c->lf_res = r;           \
        if (CC_LOGIC(CC, r, S)) CHAIN(c, u->link, u->imm);                                 \
        CHAIN(c, u->link2, u->aux);                                                        \
    }
#define DEF_FJCC_CC(FORM, S)                                                               \
    DEF_FJCC(FORM, S, 0) DEF_FJCC(FORM, S, 1) DEF_FJCC(FORM, S, 2) DEF_FJCC(FORM, S, 3)    \
    DEF_FJCC(FORM, S, 4) DEF_FJCC(FORM, S, 5) DEF_FJCC(FORM, S, 6) DEF_FJCC(FORM, S, 7)    \
    DEF_FJCC(FORM, S, 8) DEF_FJCC(FORM, S, 9) DEF_FJCC(FORM, S, 10) DEF_FJCC(FORM, S, 11)  \
    DEF_FJCC(FORM, S, 12) DEF_FJCC(FORM, S, 13) DEF_FJCC(FORM, S, 14) DEF_FJCC(FORM, S, 15)
DEF_FJCC_CC(RR, 32) DEF_FJCC_CC(RR, 64) DEF_FJCC_CC(RI, 32) DEF_FJCC_CC(RI, 64)
#define TAB_FJ_CC(OPN, FORM, S) {                                                          \
    fj_##OPN##_##FORM##_##S##_0, fj_##OPN##_##FORM##_##S##_1, fj_##OPN##_##FORM##_##S##_2, fj_##OPN##_##FORM##_##S##_3, \
    fj_##OPN##_##FORM##_##S##_4, fj_##OPN##_##FORM##_##S##_5, fj_##OPN##_##FORM##_##S##_6, fj_##OPN##_##FORM##_##S##_7, \
    fj_##OPN##_##FORM##_##S##_8, fj_##OPN##_##FORM##_##S##_9, fj_##OPN##_##FORM##_##S##_10, fj_##OPN##_##FORM##_##S##_11, \
    fj_##OPN##_##FORM##_##S##_12, fj_##OPN##_##FORM##_##S##_13, fj_##OPN##_##FORM##_##S##_14, fj_##OPN##_##FORM##_##S##_15 }
#define TAB_FJ(OPN) { { TAB_FJ_CC(OPN, RR, 32), TAB_FJ_CC(OPN, RR, 64) }, { TAB_FJ_CC(OPN, RI, 32), TAB_FJ_CC(OPN, RI, 64) } }
const OpFn fxi_fjcc_tab[2][2][2][16] = { TAB_FJ(cmp), TAB_FJ(test) };

// ---------------------------------------------------------------------------
// Stack, misc
// ---------------------------------------------------------------------------
static void op_push_R(FxiCpu *c, Uop *u) { uint64_t v = c->r[u->src >> 3]; c->r[R_SP] -= 8; st64(c->r[R_SP], v); FXI_NEXT(c, u); }
static void op_push_I(FxiCpu *c, Uop *u) { c->r[R_SP] -= 8; st64(c->r[R_SP], u->imm); FXI_NEXT(c, u); }
static void op_push_M(FxiCpu *c, Uop *u) { uint64_t v = ld64(fxi_ea(c, u)); c->r[R_SP] -= 8; st64(c->r[R_SP], v); FXI_NEXT(c, u); }
static void op_pop_R(FxiCpu *c, Uop *u) { uint64_t v = ld64(c->r[R_SP]); c->r[R_SP] += 8; c->r[u->dst >> 3] = v; FXI_NEXT(c, u); }
static void op_pop_M(FxiCpu *c, Uop *u) { uint64_t v = ld64(c->r[R_SP]); c->r[R_SP] += 8; st64(fxi_ea(c, u), v); FXI_NEXT(c, u); }
static void op_leave(FxiCpu *c, Uop *u) { c->r[R_SP] = c->r[R_BP]; c->r[R_BP] = ld64(c->r[R_SP]); c->r[R_SP] += 8; FXI_NEXT(c, u); }
static void op_pushf(FxiCpu *c, Uop *u) { c->r[R_SP] -= 8; st64(c->r[R_SP], fxi_rflags(c)); FXI_NEXT(c, u); }
static void op_popf(FxiCpu *c, Uop *u) { fxi_set_rflags(c, ld64(c->r[R_SP])); c->r[R_SP] += 8; FXI_NEXT(c, u); }
static void op_nop(FxiCpu *c, Uop *u) { FXI_NEXT(c, u); }

static void op_cbw(FxiCpu *c, Uop *u) { wr16(c, 0, (uint64_t)(int64_t)(int8_t)c->r[R_AX]); FXI_NEXT(c, u); }
static void op_cwde(FxiCpu *c, Uop *u) { wr32(c, 0, (uint64_t)(int64_t)(int16_t)c->r[R_AX]); FXI_NEXT(c, u); }
static void op_cdqe(FxiCpu *c, Uop *u) { c->r[R_AX] = (uint64_t)(int64_t)(int32_t)c->r[R_AX]; FXI_NEXT(c, u); }
static void op_cwd(FxiCpu *c, Uop *u) { wr16(c, R_DX * 8, (int16_t)c->r[R_AX] < 0 ? 0xffff : 0); FXI_NEXT(c, u); }
static void op_cdq(FxiCpu *c, Uop *u) { wr32(c, R_DX * 8, (int32_t)c->r[R_AX] < 0 ? 0xffffffffu : 0); FXI_NEXT(c, u); }
static void op_cqo(FxiCpu *c, Uop *u) { c->r[R_DX] = (int64_t)c->r[R_AX] < 0 ? ~0ull : 0; FXI_NEXT(c, u); }

static void op_bswap32(FxiCpu *c, Uop *u) { wr32(c, u->dst, __builtin_bswap32((uint32_t)c->r[u->dst >> 3])); FXI_NEXT(c, u); }
static void op_bswap64(FxiCpu *c, Uop *u) { c->r[u->dst >> 3] = __builtin_bswap64(c->r[u->dst >> 3]); FXI_NEXT(c, u); }

// xchg: [R/M][size]
#define DEF_XCHG(S)                                                                        \
    static void xchg_R_##S(FxiCpu *c, Uop *u) { uint64_t a = rd##S(c, u->dst), b = rd##S(c, u->src); wr##S(c, u->dst, b); wr##S(c, u->src, a); FXI_NEXT(c, u); } \
    static void xchg_M_##S(FxiCpu *c, Uop *u) { uint64_t ad = fxi_ea(c, u); uint64_t a = ld##S(ad), b = rd##S(c, u->src); st##S(ad, b); wr##S(c, u->src, a); FXI_NEXT(c, u); }
DEF_XCHG(8) DEF_XCHG(16) DEF_XCHG(32) DEF_XCHG(64)

// Bit scans and counts (16/32/64): the decoder puts the operand size index in u->cc.
static uint64_t bitsrc(FxiCpu *c, Uop *u, int mem) {
    unsigned si = u->cc;
    uint64_t v = mem ? (si == 1 ? ld16(fxi_ea(c, u)) : si == 2 ? ld32(fxi_ea(c, u)) : ld64(fxi_ea(c, u)))
                     : (si == 1 ? rd16(c, u->src) : si == 2 ? rd32(c, u->src) : rd64(c, u->src));
    return v;
}
static void bitdst(FxiCpu *c, Uop *u, uint64_t v) {
    unsigned si = u->cc;
    if (si == 1) wr16(c, u->dst, v); else if (si == 2) wr32(c, u->dst, v); else wr64(c, u->dst, v);
}
#define BIT_OP(NAME, BODY)                                                                 \
    static void NAME##_R(FxiCpu *c, Uop *u) { unsigned bits = 8u << u->cc; uint64_t v = bitsrc(c, u, 0); (void)bits; BODY; FXI_NEXT(c, u); } \
    static void NAME##_M(FxiCpu *c, Uop *u) { unsigned bits = 8u << u->cc; uint64_t v = bitsrc(c, u, 1); (void)bits; BODY; FXI_NEXT(c, u); }
BIT_OP(bsf, { uint64_t f = fxi_rflags(c) & ~0x40ull; if (!v) f |= 0x40; else bitdst(c, u, (uint64_t)__builtin_ctzll(v)); fxi_set_rflags(c, f); })
BIT_OP(bsr, { uint64_t f = fxi_rflags(c) & ~0x40ull; if (!v) f |= 0x40; else bitdst(c, u, 63u - (unsigned)__builtin_clzll(v)); fxi_set_rflags(c, f); })
BIT_OP(tzcnt, { uint64_t r = v ? (uint64_t)__builtin_ctzll(v) : bits; bitdst(c, u, r);
               fxi_set_rflags(c, (fxi_rflags(c) & ~0x41ull) | (v == 0) | (uint64_t)(r == 0) << 6); })
BIT_OP(lzcnt, { uint64_t r = v ? (uint64_t)(__builtin_clzll(v) - (64 - bits)) : bits; bitdst(c, u, r);
               fxi_set_rflags(c, (fxi_rflags(c) & ~0x41ull) | (v == 0) | (uint64_t)(r == 0) << 6); })
BIT_OP(popcnt, { bitdst(c, u, (uint64_t)__builtin_popcountll(v)); fxi_set_rflags(c, (uint64_t)(v == 0) << 6); })

// bt r, r|imm (register operand only; size index in u->cc, bit index from
// register u->src or, when u->src == 0xffff, u->imm). Only CF changes.
static void bt_R(FxiCpu *c, Uop *u) {
    unsigned si = u->cc, bits = 8u << si;
    uint64_t v = si == 1 ? rd16(c, u->dst) : si == 2 ? rd32(c, u->dst) : rd64(c, u->dst);
    unsigned n = (u->src == 0xffff ? (unsigned)u->imm : (unsigned)c->r[u->src >> 3]) & (bits - 1);
    fxi_set_rflags(c, (fxi_rflags(c) & ~1ull) | ((v >> n) & 1));
    FXI_NEXT(c, u);
}

static void op_cld(FxiCpu *c, Uop *u) { c->df = 0; FXI_NEXT(c, u); }
static void op_std(FxiCpu *c, Uop *u) { c->df = 1; FXI_NEXT(c, u); }
static void op_clc(FxiCpu *c, Uop *u) { fxi_set_rflags(c, fxi_rflags(c) & ~1ull); FXI_NEXT(c, u); }
static void op_stc(FxiCpu *c, Uop *u) { fxi_set_rflags(c, fxi_rflags(c) | 1ull); FXI_NEXT(c, u); }
static void op_cmc(FxiCpu *c, Uop *u) { fxi_set_rflags(c, fxi_rflags(c) ^ 1ull); FXI_NEXT(c, u); }
static void op_lahf(FxiCpu *c, Uop *u) { wr8(c, R_AX * 8 + 1, fxi_rflags(c)); FXI_NEXT(c, u); }
static void op_sahf(FxiCpu *c, Uop *u) { fxi_set_rflags(c, (fxi_rflags(c) & ~0xffull) | ((c->r[R_AX] >> 8) & 0xd5)); FXI_NEXT(c, u); }

// CPUID: an honest identity, SSE/SSE2 only (what FXI implements).
static void op_cpuid(FxiCpu *c, Uop *u) {
    uint32_t leaf = (uint32_t)c->r[R_AX], a = 0, b = 0, cc = 0, d = 0;
    static const char brand[48] = "MYIOSDECK FXI x86-64 interpreter (no JIT)";
    switch (leaf) {
    case 0: a = 1; memcpy(&b, "FXII", 4); memcpy(&d, "nter", 4); memcpy(&cc, "pret", 4); break;
    case 1: a = 0x000306a9; d = 1u << 0 | 1u << 4 | 1u << 8 | 1u << 15 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26; break;
    case 0x80000000: a = 0x80000004; break;
    case 0x80000001: d = 1u << 29; /* long mode */ break;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        const uint32_t *p = (const uint32_t *)(brand + (leaf - 0x80000002) * 16);
        a = p[0]; b = p[1]; cc = p[2]; d = p[3];
        break;
    }
    }
    wr32(c, R_AX * 8, a); wr32(c, R_BX * 8, b); wr32(c, R_CX * 8, cc); wr32(c, R_DX * 8, d);
    FXI_NEXT(c, u);
}

#include <time.h>
static void op_rdtsc(FxiCpu *c, Uop *u) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    wr32(c, R_AX * 8, t); wr32(c, R_DX * 8, t >> 32);
    FXI_NEXT(c, u);
}

// String ops (rep and single): u->scale = element size index, u->cc = 1 for rep.
static void op_stos(FxiCpu *c, Uop *u) {
    unsigned sz = 1u << u->scale;
    uint64_t n = u->cc ? c->r[R_CX] : 1, v = c->r[R_AX], di = c->r[R_DI];
    int64_t step = c->df ? -(int64_t)sz : (int64_t)sz;
    if (sz == 1 && !c->df && u->cc) { memset((void *)(uintptr_t)di, (int)(uint8_t)v, n); di += n; }
    else for (uint64_t i = 0; i < n; i++, di += (uint64_t)step) memcpy((void *)(uintptr_t)di, &v, sz);
    c->r[R_DI] = di;
    if (u->cc) c->r[R_CX] = 0;
    FXI_NEXT(c, u);
}
static void op_movs(FxiCpu *c, Uop *u) {
    unsigned sz = 1u << u->scale;
    uint64_t n = u->cc ? c->r[R_CX] : 1, si = c->r[R_SI], di = c->r[R_DI];
    int64_t step = c->df ? -(int64_t)sz : (int64_t)sz;
    if (!c->df && u->cc && (di >= si + n * sz || si >= di + n * sz)) {
        memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)si, n * sz); si += n * sz; di += n * sz;
    } else {
        for (uint64_t i = 0; i < n; i++, si += (uint64_t)step, di += (uint64_t)step)
            memmove((void *)(uintptr_t)di, (const void *)(uintptr_t)si, sz);
    }
    c->r[R_SI] = si; c->r[R_DI] = di;
    if (u->cc) c->r[R_CX] = 0;
    FXI_NEXT(c, u);
}

// ---------------------------------------------------------------------------
// Named handlers for the decoder
// ---------------------------------------------------------------------------
static const struct { const char *name; OpFn fn; } kNamed[] = {
    { "jmp", op_jmp }, { "jcc", op_jcc }, { "call", op_call }, { "call_R", op_call_R }, { "call_M", op_call_M },
    { "jmp_R", op_jmp_R }, { "jmp_M", op_jmp_M }, { "ret", op_ret }, { "goto", op_goto }, { "stop", op_stop }, { "ec_exit", op_ec_exit },
    { "fail_ud", op_fail_ud }, { "hlt", op_hlt }, { "syscall", op_syscall },
    { "push_R", op_push_R }, { "push_I", op_push_I }, { "push_M", op_push_M }, { "pop_R", op_pop_R }, { "pop_M", op_pop_M },
    { "leave", op_leave }, { "pushf", op_pushf }, { "popf", op_popf }, { "nop", op_nop },
    { "cbw", op_cbw }, { "cwde", op_cwde }, { "cdqe", op_cdqe }, { "cwd", op_cwd }, { "cdq", op_cdq }, { "cqo", op_cqo },
    { "bswap32", op_bswap32 }, { "bswap64", op_bswap64 },
    { "xchg_R_8", xchg_R_8 }, { "xchg_R_16", xchg_R_16 }, { "xchg_R_32", xchg_R_32 }, { "xchg_R_64", xchg_R_64 },
    { "xchg_M_8", xchg_M_8 }, { "xchg_M_16", xchg_M_16 }, { "xchg_M_32", xchg_M_32 }, { "xchg_M_64", xchg_M_64 },
    { "bsf_R", bsf_R }, { "bsf_M", bsf_M }, { "bsr_R", bsr_R }, { "bsr_M", bsr_M },
    { "tzcnt_R", tzcnt_R }, { "tzcnt_M", tzcnt_M }, { "lzcnt_R", lzcnt_R }, { "lzcnt_M", lzcnt_M },
    { "popcnt_R", popcnt_R }, { "popcnt_M", popcnt_M }, { "bt_R", bt_R },
    { "cld", op_cld }, { "std", op_std }, { "clc", op_clc }, { "stc", op_stc }, { "cmc", op_cmc },
    { "lahf", op_lahf }, { "sahf", op_sahf }, { "cpuid", op_cpuid }, { "rdtsc", op_rdtsc },
    { "stos", op_stos }, { "movs", op_movs },
};

OpFn fxi_sse_named(const char *name);   // fxi_sse.c

OpFn fxi_named(const char *name) {
    for (size_t i = 0; i < sizeof kNamed / sizeof kNamed[0]; i++)
        if (!strcmp(kNamed[i].name, name)) return kNamed[i].fn;
    return fxi_sse_named(name);
}
