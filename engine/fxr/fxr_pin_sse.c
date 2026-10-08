// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: pinned SSE handlers (XMM0-7 in host vector registers). See fxr_pin.c.

#include "fxr_pin.h"

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

#define E_XRR(S, D, OP) xr_##OP##_##D##_##S,
#define ROW_XRR(D, OP) { X9B(E_XRR, D, OP) },
#define E_XRT(D, OP) xt_##OP##_##D,
#define XOP_ENTRY(OP, W) { #OP, { X9(ROW_XRR, OP) }, { X9(E_XRT, OP) } },
#define XROP_ENTRY(OP) { #OP, { X9(ROW_XRR, OP) }, { 0 } },
const XOp fxr_xops[] = { XLIST(XOP_ENTRY) XRLIST(XROP_ENTRY) XOP_ENTRY(comiss, 4) XOP_ENTRY(comisd, 8) };
const size_t fxr_n_xops = sizeof fxr_xops / sizeof fxr_xops[0];

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
const XShift fxr_xshift[] = { XILIST(XI_ENTRY) };
const size_t fxr_n_xshift = sizeof fxr_xshift / sizeof fxr_xshift[0];

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
const PFn t_xl_movx[9][17][17] = { X9(X_XLS, xl_movx) }, t_xs_movx[9][17][17] = { X9(X_XLS, xs_movx) };   // [xmm][base][index]
#define E_XLS1(B, X, NAME) NAME##_##B##_##X,
#define X_XLS1(X, NAME) { R17(E_XLS1, X, NAME) },
const PFn t_xl_movss[9][17] = { X9(X_XLS1, xl_movss) }, t_xl_movsd[9][17] = { X9(X_XLS1, xl_movsd) },
    t_xs_movss[9][17] = { X9(X_XLS1, xs_movss) }, t_xs_movsd[9][17] = { X9(X_XLS1, xs_movsd) };
#define E_X1(X, NAME) NAME##_##X,
const PFn t_xlt_movss[9] = { X9(E_X1, xlt_movss) }, t_xlt_movsd[9] = { X9(E_X1, xlt_movsd) },
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
#define E_XG(G, X, NAME) xg_##NAME##_##X##_##G,
#define ROW_XG(X, NAME) { R16(E_XG, X, NAME) },
#define T_XG(NAME) { X9(ROW_XG, NAME) },
const PFn t_xg[XG_COUNT][9][16] = { T_XG(cvtsi2ss32) T_XG(cvtsi2ss64) T_XG(cvtsi2sd32) T_XG(cvtsi2sd64) T_XG(movd32) T_XG(movd64) };
#define E_GX(G, X, NAME) gx_##NAME##_##G##_##X,
#define ROW_GX(X, NAME) { R16(E_GX, X, NAME) },
#define T_GX(NAME) { X9(ROW_GX, NAME) },
const PFn t_gx[GX_COUNT][9][16] = {   // [kind][xmm][gpr]
    T_GX(cvttss2si32) T_GX(cvtss2si32) T_GX(cvttsd2si32) T_GX(cvtsd2si32) T_GX(cvttss2si64) T_GX(cvtss2si64)
    T_GX(cvttsd2si64) T_GX(cvtsd2si64) T_GX(movd32) T_GX(movd64) T_GX(pmovmskb) T_GX(movmskps) T_GX(movmskpd) };
const PFn t_xgt[4][9] = { { X9(E_X1, xgt_cvtsi2ss32) }, { X9(E_X1, xgt_cvtsi2ss64) },
                                 { X9(E_X1, xgt_cvtsi2sd32) }, { X9(E_X1, xgt_cvtsi2sd64) } };

// Byte indices for the tbl-based shuffles, from the instruction's immediate
void fxr_xshuffle_index(const char *op, unsigned imm, uint8_t *x) {
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
