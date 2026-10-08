// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: superinstructions, adjacent instruction pairs run as one uop. The families were chosen
// from the instruction-pair frequencies of a corpus of real x86-64 code (engine/fxr/corpus.sh:
// system tools and libraries, game-adjacent libraries, Madeira's Windows test programs), never
// from the benchmarks:
//   compare-and-branch forms FXI does not fuse: a memory operand, 8/16-bit registers, (u)comis
//   ALU with an immediate + jcc on its flags; dec/inc + je/jne/js/jns
//   argument setup + call (mov r,r / lea r,[base+disp] / mov r,imm / xor r,r)
//   pop + pop, pop + ret, push + push; a load + test of the loaded register + jcc
// Each is specialised like a single instruction (registers, condition code); the lowering in
// fxr_pin.c pairs them, only when nothing reads the flags after the pair: they are never recorded.

#include "fxr_pin.h"

// ---- cmp/test [mem], imm + jcc. [base + disp] specialised on the base, else the address in T.
// The immediate rides in fimm. ----
#define DEF_FMI_B(B, SZ, CC)                                                               \
    PH p_fmci_##SZ##_##CC##_##B(FXR_PARAMS) {                                              \
        uint64_t a = ld##SZ(g##B + (uint64_t)u->disp), b = (uint64_t)(int64_t)u->fimm & M##SZ; \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fmti_##SZ##_##CC##_##B(FXR_PARAMS) {                                              \
        uint64_t a = ld##SZ(g##B + (uint64_t)u->disp), b = (uint64_t)(int64_t)u->fimm & M##SZ, r = a & b; \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }
#define DEF_FMI_CC(CC, SZ) R17(DEF_FMI_B, SZ, CC)                                          \
    PH p_fmcit_##SZ##_##CC(FXR_PARAMS) {                                                   \
        uint64_t a = ld##SZ(T), b = (uint64_t)(int64_t)u->fimm & M##SZ;                    \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fmtit_##SZ##_##CC(FXR_PARAMS) {                                                   \
        uint64_t a = ld##SZ(T), b = (uint64_t)(int64_t)u->fimm & M##SZ, r = a & b;         \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }
C16(DEF_FMI_CC, 8) C16(DEF_FMI_CC, 16) C16(DEF_FMI_CC, 32) C16(DEF_FMI_CC, 64)
#define E_FMI(B, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##B,
#define CC_FMI(CC, SZ, NAME) { R17(E_FMI, SZ, CC, NAME) },
#define SZ_FMI(NAME) { { C16(CC_FMI, 8, NAME) }, { C16(CC_FMI, 16, NAME) }, { C16(CC_FMI, 32, NAME) }, { C16(CC_FMI, 64, NAME) } }
const PFn t_fmi[2][4][16][17] = { SZ_FMI(fmci), SZ_FMI(fmti) };   // [test?][size][cc][base]
#define E_FMIT(CC, SZ, NAME) p_##NAME##_##SZ##_##CC,
#define SZ_FMIT(NAME) { { C16(E_FMIT, 8, NAME) }, { C16(E_FMIT, 16, NAME) }, { C16(E_FMIT, 32, NAME) }, { C16(E_FMIT, 64, NAME) } }
const PFn t_fmit[2][4][16] = { SZ_FMIT(fmcit), SZ_FMIT(fmtit) };

// ---- cmp [mem], reg / cmp reg, [mem] / test [mem], reg + jcc (address in T) ----
#define DEF_FMR(R, SZ, CC)                                                                 \
    PH p_fmcm_##SZ##_##CC##_##R(FXR_PARAMS) {   /* cmp [mem], reg */                       \
        uint64_t a = ld##SZ(T), b = g##R & M##SZ;                                          \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fmcr_##SZ##_##CC##_##R(FXR_PARAMS) {   /* cmp reg, [mem] */                       \
        uint64_t a = g##R & M##SZ, b = ld##SZ(T);                                          \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fmtm_##SZ##_##CC##_##R(FXR_PARAMS) {   /* test [mem], reg */                      \
        uint64_t a = ld##SZ(T), b = g##R & M##SZ, r = a & b;                               \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }
#define DEF_FMR_CC(CC, SZ) R16(DEF_FMR, SZ, CC)
C16(DEF_FMR_CC, 8) C16(DEF_FMR_CC, 16) C16(DEF_FMR_CC, 32) C16(DEF_FMR_CC, 64)
#define E_FMR(R, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##R,
#define CC_FMR(CC, SZ, NAME) { R16(E_FMR, SZ, CC, NAME) },
#define SZ_FMR(NAME) { { C16(CC_FMR, 8, NAME) }, { C16(CC_FMR, 16, NAME) }, { C16(CC_FMR, 32, NAME) }, { C16(CC_FMR, 64, NAME) } }
const PFn t_fmr[3][4][16][16] = { SZ_FMR(fmcm), SZ_FMR(fmcr), SZ_FMR(fmtm) };   // [cmp m,r | cmp r,m | test m,r][size][cc][reg]

// ---- 8/16-bit register cmp/test + jcc (FXI fuses 32/64 only). RI: the immediate in disp. ----
#define DEF_FJS(D, SZ, CC)                                                                 \
    PH p_fjsc_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                           \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ;                          \
        FJ_BR(cc_sub(CC, a, b, SI##SZ)); }                                                 \
    PH p_fjst_ri_##SZ##_##CC##_##D(FXR_PARAMS) {                                           \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = a & b;               \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }                                                  \
    PH p_fjst_rr_##SZ##_##CC##_##D(FXR_PARAMS) {   /* test r, r: the same register */      \
        uint64_t a = g##D & M##SZ;                                                         \
        FJ_BR(cc_logic(CC, a, SI##SZ)); }
#define DEF_FJS_CC(CC, SZ) R16(DEF_FJS, SZ, CC)
C16(DEF_FJS_CC, 8) C16(DEF_FJS_CC, 16)
// two different registers (cmp or test): the condition code at run time
#define DEF_FJSX(S, D, SZ)                                                                 \
    PH p_fjscx_##SZ##_##D##_##S(FXR_PARAMS) {                                              \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ;                                       \
        FJ_BR(cc_sub(u->cc, a, b, SI##SZ)); }                                              \
    PH p_fjstx_##SZ##_##D##_##S(FXR_PARAMS) {                                              \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ, r = a & b;                            \
        FJ_BR(cc_logic(u->cc, r, SI##SZ)); }
#define DEF_FJSX_ROW(D, SZ) R16B(DEF_FJSX, D, SZ)
R16(DEF_FJSX_ROW, 8) R16(DEF_FJSX_ROW, 16)
#define E_FJS(D, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##D,
#define CC_FJS(CC, SZ, NAME) { R16(E_FJS, SZ, CC, NAME) },
const PFn t_fjs_ri[2][2][16][16] = {   // [test?][16-bit?][cc][D]
    { { C16(CC_FJS, 8, fjsc_ri) }, { C16(CC_FJS, 16, fjsc_ri) } },
    { { C16(CC_FJS, 8, fjst_ri) }, { C16(CC_FJS, 16, fjst_ri) } } };
const PFn t_fjs_rr[2][2][16][16] = {   // test r, r: [unused][16-bit?][cc][D]
    { { C16(CC_FJS, 8, fjst_rr) }, { C16(CC_FJS, 16, fjst_rr) } },
    { { C16(CC_FJS, 8, fjst_rr) }, { C16(CC_FJS, 16, fjst_rr) } } };
const PFn t_fjs_rrx[2][2][16][16] = {   // [test?][16-bit?][D][S]
    { { R16(ROW_RR, fjscx_8) }, { R16(ROW_RR, fjscx_16) } },
    { { R16(ROW_RR, fjstx_8) }, { R16(ROW_RR, fjstx_16) } } };

// ---- (u)comiss / (u)comisd + jcc: x86's flags after the compare are CF = less or unordered,
// ZF = equal or unordered, PF = unordered, OF = SF = 0 ----
FXI_INLINE int fcj_cond(unsigned cc, double a, double b) {
    int un = (a != a) | (b != b);
    return cc_flags(cc, 0, !(a >= b), !(a < b || a > b), 0, un);
}
#define DEF_FCJ(S, D, CC)                                                                  \
    PH p_fcjd_##CC##_##D##_##S(FXR_PARAMS) {                                               \
        double a = LD0(XD_##D), b = LD0(XS_##S);                                           \
        FJ_BR(fcj_cond(CC, a, b)); }                                                       \
    PH p_fcjs_##CC##_##D##_##S(FXR_PARAMS) {                                               \
        double a = LF0(XD_##D), b = LF0(XS_##S);                                           \
        FJ_BR(fcj_cond(CC, a, b)); }
#define DEF_FCJ_ROW(D, CC) X9B(DEF_FCJ, D, CC)                                             \
    PH p_fcjdt_##CC##_##D(FXR_PARAMS) {                                                    \
        double a = LD0(XD_##D), b = LD0(xldn(T, 8));                                       \
        FJ_BR(fcj_cond(CC, a, b)); }                                                       \
    PH p_fcjst_##CC##_##D(FXR_PARAMS) {                                                    \
        double a = LF0(XD_##D), b = LF0(xldn(T, 4));                                       \
        FJ_BR(fcj_cond(CC, a, b)); }
#define DEF_FCJ_CC(CC, _) X9(DEF_FCJ_ROW, CC)
C16(DEF_FCJ_CC, _)
#define E_FCJ(S, D, CC, NAME) p_##NAME##_##CC##_##D##_##S,
#define ROW_FCJ(D, CC, NAME) { X9B(E_FCJ, D, CC, NAME) },
#define CC_FCJ(CC, NAME) { X9(ROW_FCJ, CC, NAME) },
const PFn t_fcj[2][16][9][9] = { { C16(CC_FCJ, fcjs) }, { C16(CC_FCJ, fcjd) } };   // [double?][cc][D][S]
#define E_FCJT(D, CC, NAME) p_##NAME##_##CC##_##D,
#define CC_FCJT(CC, NAME) { X9(E_FCJT, CC, NAME) },
const PFn t_fcjt[2][16][9] = { { C16(CC_FCJT, fcjst) }, { C16(CC_FCJT, fcjdt) } };

// ---- ALU reg, imm + jcc on its flags (add sub and or xor; 32/64). The immediate in disp. ----
#define FX_add(a, b) ((a) + (b))
#define FX_sub(a, b) ((a) - (b))
#define FX_and(a, b) ((a) & (b))
#define FX_or(a, b) ((a) | (b))
#define FX_xor(a, b) ((a) ^ (b))
#define FK_add LF_ADD
#define FK_sub LF_SUB
#define FK_and LF_LOGIC
#define FK_or LF_LOGIC
#define FK_xor LF_LOGIC
FXI_INLINE int fa_add_cond(unsigned cc, uint64_t a, uint64_t b, uint64_t r, unsigned si) {
    uint64_t sb = kSign[si];
    return cc_flags(cc, (((a ^ r) & (b ^ r)) & sb) != 0, r < a, r == 0, (r & sb) != 0, PAR(r));
}
#define FC_add(CC, a, b, r, SI) fa_add_cond(CC, a, b, r, SI)
#define FC_sub(CC, a, b, r, SI) cc_sub(CC, a, b, SI)
#define FC_and(CC, a, b, r, SI) cc_logic(CC, r, SI)
#define FC_or(CC, a, b, r, SI) cc_logic(CC, r, SI)
#define FC_xor(CC, a, b, r, SI) cc_logic(CC, r, SI)
#define DEF_FAI(D, SZ, CC, OPN)                                                            \
    PH p_fai_##OPN##_##SZ##_##CC##_##D(FXR_PARAMS) {                                       \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, r = FX_##OPN(a, b) & M##SZ; \
        g##D = r;                                                                          \
        FJ_BR(FC_##OPN(CC, a, b, r, SI##SZ)); }
#define DEF_FAI_CC(CC, SZ, OPN) R16(DEF_FAI, SZ, CC, OPN)
#define DEF_FAI_OP(OPN) C16(DEF_FAI_CC, 32, OPN) C16(DEF_FAI_CC, 64, OPN)
DEF_FAI_OP(add) DEF_FAI_OP(sub) DEF_FAI_OP(and) DEF_FAI_OP(or) DEF_FAI_OP(xor)
#define E_FAI(D, SZ, CC, OPN) p_fai_##OPN##_##SZ##_##CC##_##D,
#define CC_FAI(CC, SZ, OPN) { R16(E_FAI, SZ, CC, OPN) },
#define OP_FAI(OPN) { { C16(CC_FAI, 32, OPN) }, { C16(CC_FAI, 64, OPN) } },
const PFn t_fai[5][2][16][16] = { OP_FAI(add) OP_FAI(sub) OP_FAI(and) OP_FAI(or) OP_FAI(xor) };   // [op][64?][cc][D]

// ---- dec/inc + je/jne/js/jns (conditions on ZF/SF only; the flags must be dead after) ----
#define DEF_FID(D, SZ, CC)                                                                 \
    PH p_fdec_##SZ##_##CC##_##D(FXR_PARAMS) { uint64_t r = (g##D - 1) & M##SZ; g##D = r; FJ_BR(cc_logic(CC, r, SI##SZ)); } \
    PH p_finc_##SZ##_##CC##_##D(FXR_PARAMS) { uint64_t r = (g##D + 1) & M##SZ; g##D = r; FJ_BR(cc_logic(CC, r, SI##SZ)); }
#define C4ZS(M, ...) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__)
#define DEF_FID_CC(CC, SZ) R16(DEF_FID, SZ, CC)
C4ZS(DEF_FID_CC, 32) C4ZS(DEF_FID_CC, 64)
#define E_FID(D, SZ, CC, NAME) p_##NAME##_##SZ##_##CC##_##D,
#define CC_FID(CC, SZ, NAME) { R16(E_FID, SZ, CC, NAME) },
const PFn t_fid[2][2][4][16] = {   // [inc?][64?][cc: e ne s ns][D]
    { { C4ZS(CC_FID, 32, fdec) }, { C4ZS(CC_FID, 64, fdec) } },
    { { C4ZS(CC_FID, 32, finc) }, { C4ZS(CC_FID, 64, finc) } } };

// ---- argument setup + call (direct call: imm = target, aux = return address) ----
#define CALL_TAIL() do { g4 -= 8; st64(g4, u->aux); PCHAIN(u->ulink, p_miss_t); } while (0)
#define DEF_AMR(S, D, SZ) PH p_amr_##SZ##_##D##_##S(FXR_PARAMS) { g##D = g##S & M##SZ; CALL_TAIL(); }
#define DEF_AMR_ROW(D, SZ) R16B(DEF_AMR, D, SZ)
R16(DEF_AMR_ROW, 32) R16(DEF_AMR_ROW, 64)
const PFn t_amr[2][16][16] = { { R16(ROW_RR, amr_32) }, { R16(ROW_RR, amr_64) } };   // mov D, S; call
#define DEF_ALEA(D, B) PH p_alea_##B##_##D(FXR_PARAMS) { g##D = g##B + (uint64_t)u->disp; CALL_TAIL(); }
#define DEF_ALEA_B(B, _) R16B(DEF_ALEA, B)
R17(DEF_ALEA_B, _)
#define E_ALEA(D, B) p_alea_##B##_##D,
#define ROW_ALEA(B, _) { R16B(E_ALEA, B) },
const PFn t_alea[17][16] = { R17(ROW_ALEA, _) };   // lea D, [B + disp]; call
#define DEF_AMI(D, _)                                                                      \
    PH p_ami_32_##D(FXR_PARAMS) { g##D = (uint64_t)(uint32_t)u->fimm; CALL_TAIL(); }       \
    PH p_ami_64_##D(FXR_PARAMS) { g##D = (uint64_t)(int64_t)u->fimm; CALL_TAIL(); }        \
    PH p_axz_##D(FXR_PARAMS) { g##D = 0; CALL_TAIL(); }
R16(DEF_AMI, _)
const PFn t_ami[2][16] = { { R16(E1, ami_32) }, { R16(E1, ami_64) } };   // mov D, imm; call
const PFn t_axz[16] = { R16(E1, axz) };   // xor D32, D32 (flags dead); call

// ---- pop + pop, push + push (never rsp), pop + ret ----
#define DEF_POP2(B, A)                                                                     \
    PH p_pop2_##A##_##B(FXR_PARAMS) { uint64_t va = ld64(g4), vb = ld64(g4 + 8); g4 += 16; g##A = va; g##B = vb; PNEXT(); } \
    PH p_push2_##A##_##B(FXR_PARAMS) { uint64_t va = g##A, vb = g##B; st64(g4 - 8, va); st64(g4 - 16, vb); g4 -= 16; PNEXT(); }
#define DEF_POP2_ROW(A, _) R16B(DEF_POP2, A)
R16(DEF_POP2_ROW, _)
#define E_P2(B, A, NAME) p_##NAME##_##A##_##B,
#define ROW_P2(A, NAME) { R16B(E_P2, A, NAME) },
const PFn t_pop2[16][16] = { R16(ROW_P2, pop2) };
const PFn t_push2[16][16] = { R16(ROW_P2, push2) };
#define DEF_POPRET(A, _) PH p_popret_##A(FXR_PARAMS) { uint64_t va = ld64(g4), t = ld64(g4 + 8); g4 += 16 + u->aux; g##A = va; PIND(t); }
R16(DEF_POPRET, _)
const PFn t_popret[16] = { R16(E1, popret) };

// ---- mov D, [B + disp]; test D, D; jcc (je jne js jns jle jg) ----
#define C6(M, ...) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define DEF_LTJ(D, B, SZ, CC)                                                              \
    PH p_ltj_##SZ##_##CC##_##B##_##D(FXR_PARAMS) {                                         \
        uint64_t r = ld##SZ(g##B + (uint64_t)u->disp); g##D = r;                           \
        FJ_BR(cc_logic(CC, r, SI##SZ)); }
#define DEF_LTJ_B(B, SZ, CC) R16B(DEF_LTJ, B, SZ, CC)
#define DEF_LTJ_CC(CC, SZ) R17(DEF_LTJ_B, SZ, CC)
C6(DEF_LTJ_CC, 32) C6(DEF_LTJ_CC, 64)
#define E_LTJ(D, B, SZ, CC) p_ltj_##SZ##_##CC##_##B##_##D,
#define ROW_LTJ(B, SZ, CC) { R16B(E_LTJ, B, SZ, CC) },
#define CC_LTJ(CC, SZ) { R17(ROW_LTJ, SZ, CC) },
const PFn t_ltj[2][6][17][16] = { { C6(CC_LTJ, 32) }, { C6(CC_LTJ, 64) } };   // [64?][cc index][base][D]
