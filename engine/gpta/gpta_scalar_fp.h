// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GPTA_SCALAR_FP_H
#define GPTA_SCALAR_FP_H
// Keep scalar SSE arithmetic scalar in the ARM instruction stream. Widening
// the arithmetic to a vector makes preserving the untouched upper lanes more
// expensive. Only the low result is inserted into the original destination.
#if defined(__aarch64__)
#define GFP_DEFINE(N,INS,OP) \
    FXI_INLINE GptaV gpta_scalar_##N##ss(GptaV d, GptaV s) { \
        float z; __asm__(INS " %s0, %s1, %s2" : "=w"(z) : "w"(LF0(d)), "w"(LF0(s))); \
        return xlo_f(d,z); } \
    FXI_INLINE GptaV gpta_scalar_##N##sd(GptaV d, GptaV s) { \
        double z; __asm__(INS " %d0, %d1, %d2" : "=w"(z) : "w"(LD0(d)), "w"(LD0(s))); \
        return xlo_d(d,z); }
#else
#define GFP_DEFINE(N,INS,OP) \
    FXI_INLINE GptaV gpta_scalar_##N##ss(GptaV d, GptaV s) { return xlo_f(d,LF0(d) OP LF0(s)); } \
    FXI_INLINE GptaV gpta_scalar_##N##sd(GptaV d, GptaV s) { return xlo_d(d,LD0(d) OP LD0(s)); }
#endif
GFP_DEFINE(add,"fadd",+)
GFP_DEFINE(sub,"fsub",-)
GFP_DEFINE(mul,"fmul",*)
GFP_DEFINE(div,"fdiv",/)
#undef GFP_DEFINE
#endif
