// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA: 3-operand SSE pairs, "movaps/movups/movdqa D, S ; op D, X" run as D = S op X: two-operand
// SSE makes compilers copy a register before an op, and the corpus (engine/gpta/corpus.sh: "SSE: copy
// ; op on the copy") counts the pair. Specialised on the three XMM operand classes (0-7, M: in
// memory, the index in dst / src / base). The ops are FXI's semantics (gpta_pin_sse.c's XO_ forms).

#include "gpta_pin.h"

#define X3_addps(D, S) ((GptaV)((VF)(D) + (VF)(S)))
#define X3_subps(D, S) ((GptaV)((VF)(D) - (VF)(S)))
#define X3_mulps(D, S) ((GptaV)((VF)(D) * (VF)(S)))
#define X3_divps(D, S) ((GptaV)((VF)(D) / (VF)(S)))
#define X3_addpd(D, S) ((GptaV)((VD)(D) + (VD)(S)))
#define X3_subpd(D, S) ((GptaV)((VD)(D) - (VD)(S)))
#define X3_mulpd(D, S) ((GptaV)((VD)(D) * (VD)(S)))
#define X3_divpd(D, S) ((GptaV)((VD)(D) / (VD)(S)))
#define X3_addss(D, S) xlo_f(D, LF0(D) + LF0(S))
#define X3_subss(D, S) xlo_f(D, LF0(D) - LF0(S))
#define X3_mulss(D, S) xlo_f(D, LF0(D) * LF0(S))
#define X3_divss(D, S) xlo_f(D, LF0(D) / LF0(S))
#define X3_addsd(D, S) xlo_d(D, LD0(D) + LD0(S))
#define X3_subsd(D, S) xlo_d(D, LD0(D) - LD0(S))
#define X3_mulsd(D, S) xlo_d(D, LD0(D) * LD0(S))
#define X3_divsd(D, S) xlo_d(D, LD0(D) / LD0(S))
#define X3_pand(D, S) ((D) & (S))
#define X3_pandn(D, S) (~(D) & (S))
#define X3_por(D, S) ((D) | (S))
#define X3_pxor(D, S) ((D) ^ (S))

// D = S (the copy), then D = D op X: X == D was mapped to S by the lowering
#define DEF_X3(X, S, D, OP) PH p_x3_##OP##_##D##_##S##_##X(GPTA_PARAMS) {                    \
        GptaV s_ = XS_##S, x_ = XX_##X; XW_##D(X3_##OP(s_, x_)); PNEXT(); }
#define DEF_X3_S(S, D, OP) X9C(DEF_X3, S, D, OP)
#define DEF_X3_D(D, OP) X9B(DEF_X3_S, D, OP)
#define DEF_X3_OP(OP) X9(DEF_X3_D, OP)
DEF_X3_OP(addps) DEF_X3_OP(subps) DEF_X3_OP(mulps) DEF_X3_OP(divps) DEF_X3_OP(addpd) DEF_X3_OP(subpd)
DEF_X3_OP(mulpd) DEF_X3_OP(divpd) DEF_X3_OP(addss) DEF_X3_OP(subss) DEF_X3_OP(mulss) DEF_X3_OP(divss)
DEF_X3_OP(addsd) DEF_X3_OP(subsd) DEF_X3_OP(mulsd) DEF_X3_OP(divsd) DEF_X3_OP(pand) DEF_X3_OP(pandn)
DEF_X3_OP(por) DEF_X3_OP(pxor)
#define E_X3(X, S, D, OP) p_x3_##OP##_##D##_##S##_##X,
#define ROW_X3_S(S, D, OP) { X9C(E_X3, S, D, OP) },
#define ROW_X3_D(D, OP) { X9B(ROW_X3_S, D, OP) },
#define T_X3(OP) { X9(ROW_X3_D, OP) },
const PFn t_x3[20][9][9][9] = {   // [x3_index][D][S][X]
    T_X3(addps) T_X3(subps) T_X3(mulps) T_X3(divps) T_X3(addpd) T_X3(subpd) T_X3(mulpd) T_X3(divpd)
    T_X3(addss) T_X3(subss) T_X3(mulss) T_X3(divss) T_X3(addsd) T_X3(subsd) T_X3(mulsd) T_X3(divsd)
    T_X3(pand) T_X3(pandn) T_X3(por) T_X3(pxor)
};
