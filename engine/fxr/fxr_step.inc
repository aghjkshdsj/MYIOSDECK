// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: loop-step superinstructions (fxr_pin_step1.c, fxr_pin_step2.c, fxr_pin_step3.c), chosen from
// the corpus (engine/fxr/corpus.sh: "add/sub r,i ; cmp r" and "add/sub r,r ; cmp r" in loop bodies):
// a counted loop's induction step and its exit test as one uop, after FXI fused the cmp with the
// jcc. Specialised on the registers and on the 10 conditions loops exit on (C10).
//   sic   add/sub D, imm (fimm) ; cmp D, S ; jcc
//   sicr  add/sub D, imm (fimm) ; cmp S, D ; jcc
//   sii   add/sub D, imm (fimm) ; cmp D, imm (disp) ; jcc
//   sri   add/sub D, R          ; cmp D, imm (disp) ; jcc
// The flags are recorded (as the cmp leaves them) only if something after the branch reads them.

#define SOP_add(a, b) ((a) + (b))
#define SOP_sub(a, b) ((a) - (b))
#define STEP_BR(SZ, CC, A, B) do {                                                         \
        uint64_t a_ = (A) & M##SZ, b_ = (B) & M##SZ;                                       \
        if (u->flive) SETF(LF_SUB, SI##SZ, a_, b_, (a_ - b_) & M##SZ, 0);                  \
        FJ_BR(cc_sub(CC, a_, b_, SI##SZ));                                                 \
    } while (0)
#define STEP_IMM(OP, D, SZ) (g##D = SOP_##OP(g##D, (uint64_t)(int64_t)u->fimm) & M##SZ)

#define DEF_SIC(S, D, OP, SZ, CC)                                                          \
    PH p_sic_##OP##_##SZ##_##CC##_##D##_##S(FXR_PARAMS) { STEP_IMM(OP, D, SZ); STEP_BR(SZ, CC, g##D, g##S); }
#define DEF_SICR(S, D, OP, SZ, CC)                                                         \
    PH p_sicr_##OP##_##SZ##_##CC##_##D##_##S(FXR_PARAMS) { STEP_IMM(OP, D, SZ); STEP_BR(SZ, CC, g##S, g##D); }
#define DEF_SRI(S, D, OP, SZ, CC)                                                          \
    PH p_sri_##OP##_##SZ##_##CC##_##D##_##S(FXR_PARAMS) {                                  \
        g##D = SOP_##OP(g##D, g##S) & M##SZ; STEP_BR(SZ, CC, g##D, (uint64_t)u->disp); }
#define DEF_SII(D, OP, SZ, CC)                                                             \
    PH p_sii_##OP##_##SZ##_##CC##_##D(FXR_PARAMS) { STEP_IMM(OP, D, SZ); STEP_BR(SZ, CC, g##D, (uint64_t)u->disp); }

// Definitions and tables for one kind K (sic, sicr, sri: [op][64?][cc][D][S])
#define STEP_ROW(D, K, OP, SZ, CC) R16B(DEF_##K, D, OP, SZ, CC)
#define STEP_CC(CC, K, OP, SZ) R16(STEP_ROW, K, OP, SZ, CC)
#define STEP_DEFS(K) C10(STEP_CC, K, add, 32) C10(STEP_CC, K, add, 64) C10(STEP_CC, K, sub, 32) C10(STEP_CC, K, sub, 64)
#define E_STEP(S, D, K, OP, SZ, CC) p_##K##_##OP##_##SZ##_##CC##_##D##_##S,
#define ROW_STEP(D, K, OP, SZ, CC) { R16B(E_STEP, D, K, OP, SZ, CC) },
#define CC_STEP(CC, K, OP, SZ) { R16(ROW_STEP, K, OP, SZ, CC) },
#define STEP_TABLE(K) { { { C10(CC_STEP, K, add, 32) }, { C10(CC_STEP, K, add, 64) } }, \
                        { { C10(CC_STEP, K, sub, 32) }, { C10(CC_STEP, K, sub, 64) } } }
