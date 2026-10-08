// SPDX-License-Identifier: GPL-3.0-or-later
// FXI SSE/SSE2 handlers. Lane loops over 128-bit registers: the compiler turns
// them into NEON (ARM64) or SSE (x86 host) vector code. dst/src are XMM indices,
// except where a general register is involved (then a GPR byte offset).

#include <math.h>

#include "fxi_internal.h"

#define XR(i) (&c->xmm[(i)])

// Binary/unary op on XMM dst with source S (a copy, so D == S is safe). The
// memory form reads W bytes (16 packed, 4 or 8 scalar).
// Variadic body: bodies may contain commas (declarations, initialisers).
#define DEF_X(NAME, W, ...)                                                                \
    static void NAME##_RR(FxiCpu *c, Uop *u) {                                             \
        X128 *D = XR(u->dst); X128 t = c->xmm[u->src]; const X128 *S = &t; (void)D; (void)S; \
        __VA_ARGS__; FXI_NEXT(c, u);                                                       \
    }                                                                                      \
    static void NAME##_RM(FxiCpu *c, Uop *u) {                                             \
        X128 *D = XR(u->dst); X128 t; memset(&t, 0, sizeof t);                             \
        memcpy(&t, (const void *)(uintptr_t)fxi_ea(c, u), (W)); const X128 *S = &t; (void)D; (void)S; \
        __VA_ARGS__; FXI_NEXT(c, u);                                                       \
    }

#define LANES4(...) for (int i = 0; i < 4; i++) { __VA_ARGS__; }
#define LANES2(...) for (int i = 0; i < 2; i++) { __VA_ARGS__; }

// ---- float arithmetic ----
DEF_X(addps, 16, LANES4(D->f[i] += S->f[i]))
DEF_X(subps, 16, LANES4(D->f[i] -= S->f[i]))
DEF_X(mulps, 16, LANES4(D->f[i] *= S->f[i]))
DEF_X(divps, 16, LANES4(D->f[i] /= S->f[i]))
DEF_X(minps, 16, LANES4(D->f[i] = D->f[i] < S->f[i] ? D->f[i] : S->f[i]))
DEF_X(maxps, 16, LANES4(D->f[i] = D->f[i] > S->f[i] ? D->f[i] : S->f[i]))
DEF_X(sqrtps, 16, LANES4(D->f[i] = sqrtf(S->f[i])))
DEF_X(addpd, 16, LANES2(D->g[i] += S->g[i]))
DEF_X(subpd, 16, LANES2(D->g[i] -= S->g[i]))
DEF_X(mulpd, 16, LANES2(D->g[i] *= S->g[i]))
DEF_X(divpd, 16, LANES2(D->g[i] /= S->g[i]))
DEF_X(minpd, 16, LANES2(D->g[i] = D->g[i] < S->g[i] ? D->g[i] : S->g[i]))
DEF_X(maxpd, 16, LANES2(D->g[i] = D->g[i] > S->g[i] ? D->g[i] : S->g[i]))
DEF_X(sqrtpd, 16, LANES2(D->g[i] = sqrt(S->g[i])))
DEF_X(addss, 4, D->f[0] += S->f[0])
DEF_X(subss, 4, D->f[0] -= S->f[0])
DEF_X(mulss, 4, D->f[0] *= S->f[0])
DEF_X(divss, 4, D->f[0] /= S->f[0])
DEF_X(minss, 4, D->f[0] = D->f[0] < S->f[0] ? D->f[0] : S->f[0])
DEF_X(maxss, 4, D->f[0] = D->f[0] > S->f[0] ? D->f[0] : S->f[0])
DEF_X(sqrtss, 4, D->f[0] = sqrtf(S->f[0]))
DEF_X(addsd, 8, D->g[0] += S->g[0])
DEF_X(subsd, 8, D->g[0] -= S->g[0])
DEF_X(mulsd, 8, D->g[0] *= S->g[0])
DEF_X(divsd, 8, D->g[0] /= S->g[0])
DEF_X(minsd, 8, D->g[0] = D->g[0] < S->g[0] ? D->g[0] : S->g[0])
DEF_X(maxsd, 8, D->g[0] = D->g[0] > S->g[0] ? D->g[0] : S->g[0])
DEF_X(sqrtsd, 8, D->g[0] = sqrt(S->g[0]))

// ---- bitwise (ps/pd/integer forms are the same bits) ----
DEF_X(pand, 16, LANES2(D->q[i] &= S->q[i]))
DEF_X(pandn, 16, LANES2(D->q[i] = ~D->q[i] & S->q[i]))
DEF_X(por, 16, LANES2(D->q[i] |= S->q[i]))
DEF_X(pxor, 16, LANES2(D->q[i] ^= S->q[i]))

// ---- integer ----
DEF_X(paddb, 16, for (int i = 0; i < 16; i++) D->b[i] += S->b[i])
DEF_X(paddw, 16, for (int i = 0; i < 8; i++) D->w[i] += S->w[i])
DEF_X(paddd, 16, LANES4(D->d[i] += S->d[i]))
DEF_X(paddq, 16, LANES2(D->q[i] += S->q[i]))
DEF_X(psubb, 16, for (int i = 0; i < 16; i++) D->b[i] -= S->b[i])
DEF_X(psubw, 16, for (int i = 0; i < 8; i++) D->w[i] -= S->w[i])
DEF_X(psubd, 16, LANES4(D->d[i] -= S->d[i]))
DEF_X(psubq, 16, LANES2(D->q[i] -= S->q[i]))
DEF_X(pcmpeqb, 16, for (int i = 0; i < 16; i++) D->b[i] = D->b[i] == S->b[i] ? 0xff : 0)
DEF_X(pcmpeqw, 16, for (int i = 0; i < 8; i++) D->w[i] = D->w[i] == S->w[i] ? 0xffff : 0)
DEF_X(pcmpeqd, 16, LANES4(D->d[i] = D->d[i] == S->d[i] ? 0xffffffffu : 0))
DEF_X(pcmpgtb, 16, for (int i = 0; i < 16; i++) D->b[i] = D->sb[i] > S->sb[i] ? 0xff : 0)
DEF_X(pcmpgtw, 16, for (int i = 0; i < 8; i++) D->w[i] = D->sw[i] > S->sw[i] ? 0xffff : 0)
DEF_X(pcmpgtd, 16, LANES4(D->d[i] = D->sd[i] > S->sd[i] ? 0xffffffffu : 0))
DEF_X(pmuludq, 16, LANES2(D->q[i] = (uint64_t)D->d[2 * i] * S->d[2 * i]))
DEF_X(pmullw, 16, for (int i = 0; i < 8; i++) D->w[i] = (uint16_t)(D->w[i] * S->w[i]))
DEF_X(pmaxub, 16, for (int i = 0; i < 16; i++) D->b[i] = D->b[i] > S->b[i] ? D->b[i] : S->b[i])
DEF_X(pminub, 16, for (int i = 0; i < 16; i++) D->b[i] = D->b[i] < S->b[i] ? D->b[i] : S->b[i])
DEF_X(pmaxsw, 16, for (int i = 0; i < 8; i++) D->sw[i] = D->sw[i] > S->sw[i] ? D->sw[i] : S->sw[i])
DEF_X(pminsw, 16, for (int i = 0; i < 8; i++) D->sw[i] = D->sw[i] < S->sw[i] ? D->sw[i] : S->sw[i])
DEF_X(psadbw, 16, LANES2({ uint64_t s = 0; for (int j = 0; j < 8; j++) { int dlt = (int)D->b[8 * i + j] - (int)S->b[8 * i + j]; s += (uint64_t)(dlt < 0 ? -dlt : dlt); } D->q[i] = s; }))

// ---- saturating add/sub, averages, high multiplies, multiply-add (build 83: Stick Fight) ----
static inline int satS8(int v) { return v > 127 ? 127 : v < -128 ? -128 : v; }
static inline int satU8(int v) { return v > 255 ? 255 : v < 0 ? 0 : v; }
static inline int satS16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int satU16(int v) { return v > 65535 ? 65535 : v < 0 ? 0 : v; }
DEF_X(paddsb, 16, for (int i = 0; i < 16; i++) D->sb[i] = (int8_t)satS8(D->sb[i] + S->sb[i]))
DEF_X(paddsw, 16, for (int i = 0; i < 8; i++) D->sw[i] = (int16_t)satS16(D->sw[i] + S->sw[i]))
DEF_X(paddusb, 16, for (int i = 0; i < 16; i++) D->b[i] = (uint8_t)satU8(D->b[i] + S->b[i]))
DEF_X(paddusw, 16, for (int i = 0; i < 8; i++) D->w[i] = (uint16_t)satU16(D->w[i] + S->w[i]))
DEF_X(psubsb, 16, for (int i = 0; i < 16; i++) D->sb[i] = (int8_t)satS8(D->sb[i] - S->sb[i]))
DEF_X(psubsw, 16, for (int i = 0; i < 8; i++) D->sw[i] = (int16_t)satS16(D->sw[i] - S->sw[i]))
DEF_X(psubusb, 16, for (int i = 0; i < 16; i++) D->b[i] = (uint8_t)satU8(D->b[i] - S->b[i]))
DEF_X(psubusw, 16, for (int i = 0; i < 8; i++) D->w[i] = (uint16_t)satU16(D->w[i] - S->w[i]))
DEF_X(pavgb, 16, for (int i = 0; i < 16; i++) D->b[i] = (uint8_t)((D->b[i] + S->b[i] + 1) >> 1))
DEF_X(pavgw, 16, for (int i = 0; i < 8; i++) D->w[i] = (uint16_t)((D->w[i] + S->w[i] + 1) >> 1))
DEF_X(pmulhw, 16, for (int i = 0; i < 8; i++) D->sw[i] = (int16_t)(((int32_t)D->sw[i] * S->sw[i]) >> 16))
DEF_X(pmulhuw, 16, for (int i = 0; i < 8; i++) D->w[i] = (uint16_t)(((uint32_t)D->w[i] * S->w[i]) >> 16))
// Two products per lane, summed with 32-bit wraparound (-32768*-32768*2 = 0x80000000, as on x86).
DEF_X(pmaddwd, 16, { X128 r; LANES4(r.d[i] = (uint32_t)((int32_t)D->sw[2 * i] * S->sw[2 * i]) +
                                              (uint32_t)((int32_t)D->sw[2 * i + 1] * S->sw[2 * i + 1])); *D = r; })

// ---- shifts by the count in the low 64 bits of an XMM register or memory ----
DEF_X(psllwx, 16, { uint64_t n = S->q[0]; for (int i = 0; i < 8; i++) D->w[i] = n > 15 ? 0 : (uint16_t)(D->w[i] << n); })
DEF_X(pslldx, 16, { uint64_t n = S->q[0]; LANES4(D->d[i] = n > 31 ? 0 : D->d[i] << n); })
DEF_X(psllqx, 16, { uint64_t n = S->q[0]; LANES2(D->q[i] = n > 63 ? 0 : D->q[i] << n); })
DEF_X(psrlwx, 16, { uint64_t n = S->q[0]; for (int i = 0; i < 8; i++) D->w[i] = n > 15 ? 0 : (uint16_t)(D->w[i] >> n); })
DEF_X(psrldx, 16, { uint64_t n = S->q[0]; LANES4(D->d[i] = n > 31 ? 0 : D->d[i] >> n); })
DEF_X(psrlqx, 16, { uint64_t n = S->q[0]; LANES2(D->q[i] = n > 63 ? 0 : D->q[i] >> n); })
DEF_X(psrawx, 16, { uint64_t n = S->q[0]; for (int i = 0; i < 8; i++) D->sw[i] = (int16_t)(D->sw[i] >> (n > 15 ? 15 : n)); })
DEF_X(psradx, 16, { uint64_t n = S->q[0]; LANES4(D->sd[i] = D->sd[i] >> (n > 31 ? 31 : n)); })

// ---- unpack / pack ----
#define UNPACK(NAME, T, N, HI)                                                             \
    DEF_X(NAME, 16, { X128 r; for (int i = 0; i < (N) / 2; i++) { r.T[2 * i] = D->T[i + ((HI) ? (N) / 2 : 0)]; r.T[2 * i + 1] = S->T[i + ((HI) ? (N) / 2 : 0)]; } *D = r; })
UNPACK(punpcklbw, b, 16, 0) UNPACK(punpckhbw, b, 16, 1)
UNPACK(punpcklwd, w, 8, 0) UNPACK(punpckhwd, w, 8, 1)
UNPACK(punpckldq, d, 4, 0) UNPACK(punpckhdq, d, 4, 1)
UNPACK(punpcklqdq, q, 2, 0) UNPACK(punpckhqdq, q, 2, 1)
static inline int8_t sat8(int v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
static inline uint8_t satu8(int v) { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
static inline int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
DEF_X(packsswb, 16, { X128 r; for (int i = 0; i < 8; i++) { r.sb[i] = sat8(D->sw[i]); r.sb[i + 8] = sat8(S->sw[i]); } *D = r; })
DEF_X(packuswb, 16, { X128 r; for (int i = 0; i < 8; i++) { r.b[i] = satu8(D->sw[i]); r.b[i + 8] = satu8(S->sw[i]); } *D = r; })
DEF_X(packssdw, 16, { X128 r; for (int i = 0; i < 4; i++) { r.sw[i] = sat16(D->sd[i]); r.sw[i + 4] = sat16(S->sd[i]); } *D = r; })

// ---- shuffles with an immediate (u->imm) ----
DEF_X(pshufd, 16, { X128 r; for (int i = 0; i < 4; i++) r.d[i] = S->d[(u->imm >> (2 * i)) & 3]; *D = r; })
DEF_X(pshuflw, 16, { X128 r = *S; for (int i = 0; i < 4; i++) r.w[i] = S->w[(u->imm >> (2 * i)) & 3]; *D = r; })
DEF_X(pshufhw, 16, { X128 r = *S; for (int i = 0; i < 4; i++) r.w[4 + i] = S->w[4 + ((u->imm >> (2 * i)) & 3)]; *D = r; })
DEF_X(shufps, 16, { X128 r; r.d[0] = D->d[u->imm & 3]; r.d[1] = D->d[(u->imm >> 2) & 3];
                    r.d[2] = S->d[(u->imm >> 4) & 3]; r.d[3] = S->d[(u->imm >> 6) & 3]; *D = r; })
DEF_X(shufpd, 16, { X128 r; r.q[0] = D->q[u->imm & 1]; r.q[1] = S->q[(u->imm >> 1) & 1]; *D = r; })

// ---- compares with a predicate (u->imm: eq lt le unord neq nlt nle ord) ----
static inline int fcmp(double a, double b, unsigned p) {
    switch (p & 7) {
    case 0: return a == b; case 1: return a < b; case 2: return a <= b; case 3: return a != a || b != b;
    case 4: return !(a == b); case 5: return !(a < b); case 6: return !(a <= b); default: return a == a && b == b;
    }
}
DEF_X(cmpps, 16, LANES4(D->d[i] = fcmp(D->f[i], S->f[i], (unsigned)u->imm) ? 0xffffffffu : 0))
DEF_X(cmppd, 16, LANES2(D->q[i] = fcmp(D->g[i], S->g[i], (unsigned)u->imm) ? ~0ull : 0))
DEF_X(cmpss, 4, D->d[0] = fcmp(D->f[0], S->f[0], (unsigned)u->imm) ? 0xffffffffu : 0)
DEF_X(cmpsd, 8, D->q[0] = fcmp(D->g[0], S->g[0], (unsigned)u->imm) ? ~0ull : 0)

// ---- (u)comiss / (u)comisd: ZF PF CF, others clear ----
static void set_fcmp_flags(FxiCpu *c, double a, double b) {
    uint64_t f = (a != a || b != b) ? 0x45 : a < b ? 0x01 : a == b ? 0x40 : 0;
    fxi_set_rflags(c, (fxi_rflags(c) & ~0x8d5ull) | f);
}
DEF_X(comiss, 4, set_fcmp_flags(c, D->f[0], S->f[0]))
DEF_X(comisd, 8, set_fcmp_flags(c, D->g[0], S->g[0]))

// ---- conversions between float formats and packed ints ----
static inline int32_t cvt_i32(double v, int trunc) {
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return INT32_MIN;
    return (int32_t)(trunc ? v : nearbyint(v));
}
static inline int64_t cvt_i64(double v, int trunc) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)(trunc ? v : nearbyint(v));
}
DEF_X(cvtss2sd, 4, D->g[0] = (double)S->f[0])
DEF_X(cvtsd2ss, 8, D->f[0] = (float)S->g[0])
DEF_X(cvtdq2ps, 16, LANES4(D->f[i] = (float)S->sd[i]))
DEF_X(cvttps2dq, 16, LANES4(D->sd[i] = cvt_i32(S->f[i], 1)))
DEF_X(cvtps2dq, 16, LANES4(D->sd[i] = cvt_i32(S->f[i], 0)))
DEF_X(cvtdq2pd, 8, { double a = S->sd[0], b = S->sd[1]; D->g[0] = a; D->g[1] = b; })
DEF_X(cvttpd2dq, 16, { int32_t a = cvt_i32(S->g[0], 1), b = cvt_i32(S->g[1], 1); D->sd[0] = a; D->sd[1] = b; D->q[1] = 0; })
DEF_X(cvtpd2dq, 16, { int32_t a = cvt_i32(S->g[0], 0), b = cvt_i32(S->g[1], 0); D->sd[0] = a; D->sd[1] = b; D->q[1] = 0; })
DEF_X(cvtpd2ps, 16, { float a = (float)S->g[0], b = (float)S->g[1]; D->f[0] = a; D->f[1] = b; D->q[1] = 0; })
DEF_X(cvtps2pd, 8, { double a = S->f[0], b = S->f[1]; D->g[0] = a; D->g[1] = b; })

// ---- moves ----
DEF_X(movx, 16, *D = *S)                                   // movaps movups movdqa movdqu (load / reg)
static void movx_MR(FxiCpu *c, Uop *u) { memcpy((void *)(uintptr_t)fxi_ea(c, u), XR(u->src), 16); FXI_NEXT(c, u); }
static void movss_RR(FxiCpu *c, Uop *u) { XR(u->dst)->d[0] = XR(u->src)->d[0]; FXI_NEXT(c, u); }
static void movss_RM(FxiCpu *c, Uop *u) { X128 *D = XR(u->dst); D->q[0] = D->q[1] = 0; D->d[0] = (uint32_t)ld32(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void movss_MR(FxiCpu *c, Uop *u) { st32(fxi_ea(c, u), XR(u->src)->d[0]); FXI_NEXT(c, u); }
static void movsd_RR(FxiCpu *c, Uop *u) { XR(u->dst)->q[0] = XR(u->src)->q[0]; FXI_NEXT(c, u); }
static void movsd_RM(FxiCpu *c, Uop *u) { X128 *D = XR(u->dst); D->q[0] = ld64(fxi_ea(c, u)); D->q[1] = 0; FXI_NEXT(c, u); }
static void movsd_MR(FxiCpu *c, Uop *u) { st64(fxi_ea(c, u), XR(u->src)->q[0]); FXI_NEXT(c, u); }
static void movq_RR(FxiCpu *c, Uop *u) { X128 *D = XR(u->dst); D->q[0] = XR(u->src)->q[0]; D->q[1] = 0; FXI_NEXT(c, u); }
static void movlps_RM(FxiCpu *c, Uop *u) { XR(u->dst)->q[0] = ld64(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void movhps_RM(FxiCpu *c, Uop *u) { XR(u->dst)->q[1] = ld64(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void movhps_MR(FxiCpu *c, Uop *u) { st64(fxi_ea(c, u), XR(u->src)->q[1]); FXI_NEXT(c, u); }
static void movhlps_RR(FxiCpu *c, Uop *u) { XR(u->dst)->q[0] = XR(u->src)->q[1]; FXI_NEXT(c, u); }
static void movlhps_RR(FxiCpu *c, Uop *u) { XR(u->dst)->q[1] = XR(u->src)->q[0]; FXI_NEXT(c, u); }
// movd/movq between XMM and general registers (dst/src: XMM index or GPR byte offset)
static void movd_XR32(FxiCpu *c, Uop *u) { X128 *D = XR(u->dst); D->q[0] = (uint32_t)c->r[u->src >> 3]; D->q[1] = 0; FXI_NEXT(c, u); }
static void movd_XR64(FxiCpu *c, Uop *u) { X128 *D = XR(u->dst); D->q[0] = c->r[u->src >> 3]; D->q[1] = 0; FXI_NEXT(c, u); }
static void movd_RX32(FxiCpu *c, Uop *u) { c->r[u->dst >> 3] = XR(u->src)->d[0]; FXI_NEXT(c, u); }
static void movd_RX64(FxiCpu *c, Uop *u) { c->r[u->dst >> 3] = XR(u->src)->q[0]; FXI_NEXT(c, u); }
static void pmovmskb_RR(FxiCpu *c, Uop *u) {
    uint32_t m = 0; X128 *S = XR(u->src);
    for (int i = 0; i < 16; i++) m |= (uint32_t)(S->b[i] >> 7) << i;
    c->r[u->dst >> 3] = m; FXI_NEXT(c, u);
}
static void movmskps_RR(FxiCpu *c, Uop *u) {
    uint32_t m = 0; X128 *S = XR(u->src);
    for (int i = 0; i < 4; i++) m |= (S->d[i] >> 31) << i;
    c->r[u->dst >> 3] = m; FXI_NEXT(c, u);
}
static void movmskpd_RR(FxiCpu *c, Uop *u) {
    X128 *S = XR(u->src);
    c->r[u->dst >> 3] = (uint32_t)(S->q[0] >> 63) | (uint32_t)(S->q[1] >> 63) << 1; FXI_NEXT(c, u);
}

// ---- shifts by immediate (register forms only) ----
#define DEF_XSH(NAME, ...) static void NAME##_RI(FxiCpu *c, Uop *u) { X128 *D = XR(u->dst); unsigned n = (unsigned)u->imm; __VA_ARGS__; FXI_NEXT(c, u); }
DEF_XSH(psllw, for (int i = 0; i < 8; i++) D->w[i] = n > 15 ? 0 : (uint16_t)(D->w[i] << n))
DEF_XSH(pslld, LANES4(D->d[i] = n > 31 ? 0 : D->d[i] << n))
DEF_XSH(psllq, LANES2(D->q[i] = n > 63 ? 0 : D->q[i] << n))
DEF_XSH(psrlw, for (int i = 0; i < 8; i++) D->w[i] = n > 15 ? 0 : (uint16_t)(D->w[i] >> n))
DEF_XSH(psrld, LANES4(D->d[i] = n > 31 ? 0 : D->d[i] >> n))
DEF_XSH(psrlq, LANES2(D->q[i] = n > 63 ? 0 : D->q[i] >> n))
DEF_XSH(psraw, for (int i = 0; i < 8; i++) D->sw[i] = (int16_t)(D->sw[i] >> (n > 15 ? 15 : n)))
DEF_XSH(psrad, LANES4(D->sd[i] = D->sd[i] >> (n > 31 ? 31 : n)))
DEF_XSH(pslldq, { X128 r; memset(&r, 0, 16); if (n < 16) memcpy(r.b + n, D->b, 16 - n); *D = r; })
DEF_XSH(psrldq, { X128 r; memset(&r, 0, 16); if (n < 16) memcpy(r.b, D->b + n, 16 - n); *D = r; })

// ---- int <-> float with a general register or memory ----
// cvtsi2ss/sd: dst XMM, src GPR (u->src) or memory; size from the name.
#define DEF_CVTSI(NAME, FIELD, CT)                                                          \
    static void NAME##_R32(FxiCpu *c, Uop *u) { XR(u->dst)->FIELD[0] = (CT)(int32_t)c->r[u->src >> 3]; FXI_NEXT(c, u); } \
    static void NAME##_R64(FxiCpu *c, Uop *u) { XR(u->dst)->FIELD[0] = (CT)(int64_t)c->r[u->src >> 3]; FXI_NEXT(c, u); } \
    static void NAME##_M32(FxiCpu *c, Uop *u) { XR(u->dst)->FIELD[0] = (CT)(int32_t)ld32(fxi_ea(c, u)); FXI_NEXT(c, u); } \
    static void NAME##_M64(FxiCpu *c, Uop *u) { XR(u->dst)->FIELD[0] = (CT)(int64_t)ld64(fxi_ea(c, u)); FXI_NEXT(c, u); }
DEF_CVTSI(cvtsi2ss, f, float)
DEF_CVTSI(cvtsi2sd, g, double)
// cvt(t)ss2si / cvt(t)sd2si: dst GPR, src XMM (R) or memory (M); 32 or 64-bit result.
#define DEF_CVT2SI(NAME, FIELD, LD, TRUNC)                                                  \
    static void NAME##_R32(FxiCpu *c, Uop *u) { wr32(c, u->dst, (uint32_t)cvt_i32(XR(u->src)->FIELD[0], TRUNC)); FXI_NEXT(c, u); } \
    static void NAME##_R64(FxiCpu *c, Uop *u) { wr64(c, u->dst, (uint64_t)cvt_i64(XR(u->src)->FIELD[0], TRUNC)); FXI_NEXT(c, u); } \
    static void NAME##_M32(FxiCpu *c, Uop *u) { X128 t; t.q[0] = LD(fxi_ea(c, u)); wr32(c, u->dst, (uint32_t)cvt_i32(t.FIELD[0], TRUNC)); FXI_NEXT(c, u); } \
    static void NAME##_M64(FxiCpu *c, Uop *u) { X128 t; t.q[0] = LD(fxi_ea(c, u)); wr64(c, u->dst, (uint64_t)cvt_i64(t.FIELD[0], TRUNC)); FXI_NEXT(c, u); }
DEF_CVT2SI(cvttss2si, f, ld32, 1)
DEF_CVT2SI(cvtss2si, f, ld32, 0)
DEF_CVT2SI(cvttsd2si, g, ld64, 1)
DEF_CVT2SI(cvtsd2si, g, ld64, 0)

// ---- SSE4.1 lane insert/extract (u->imm = lane) ----
static void pinsrd_R(FxiCpu *c, Uop *u) { XR(u->dst)->d[u->imm & 3] = (uint32_t)c->r[u->src >> 3]; FXI_NEXT(c, u); }
static void pinsrd_M(FxiCpu *c, Uop *u) { XR(u->dst)->d[u->imm & 3] = (uint32_t)ld32(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void pinsrq_R(FxiCpu *c, Uop *u) { XR(u->dst)->q[u->imm & 1] = c->r[u->src >> 3]; FXI_NEXT(c, u); }
static void pinsrq_M(FxiCpu *c, Uop *u) { XR(u->dst)->q[u->imm & 1] = ld64(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void pextrd_R(FxiCpu *c, Uop *u) { c->r[u->dst >> 3] = XR(u->src)->d[u->imm & 3]; FXI_NEXT(c, u); }
static void pextrd_M(FxiCpu *c, Uop *u) { st32(fxi_ea(c, u), XR(u->src)->d[u->imm & 3]); FXI_NEXT(c, u); }
static void pextrq_R(FxiCpu *c, Uop *u) { c->r[u->dst >> 3] = XR(u->src)->q[u->imm & 1]; FXI_NEXT(c, u); }
static void pextrq_M(FxiCpu *c, Uop *u) { st64(fxi_ea(c, u), XR(u->src)->q[u->imm & 1]); FXI_NEXT(c, u); }

// ---- SSE2 word insert/extract (u->imm = lane) ----
static void pinsrw_R(FxiCpu *c, Uop *u) { XR(u->dst)->w[u->imm & 7] = (uint16_t)c->r[u->src >> 3]; FXI_NEXT(c, u); }
static void pinsrw_M(FxiCpu *c, Uop *u) { XR(u->dst)->w[u->imm & 7] = (uint16_t)ld16(fxi_ea(c, u)); FXI_NEXT(c, u); }
static void pextrw_R(FxiCpu *c, Uop *u) { c->r[u->dst >> 3] = XR(u->src)->w[u->imm & 7]; FXI_NEXT(c, u); }

// ---- name table for the decoder ----
#define E2(n) { #n "_RR", n##_RR }, { #n "_RM", n##_RM }
#define E4CVT(n) { #n "_R32", n##_R32 }, { #n "_R64", n##_R64 }, { #n "_M32", n##_M32 }, { #n "_M64", n##_M64 }
static const struct { const char *name; OpFn fn; } kSse[] = {
    E2(addps), E2(subps), E2(mulps), E2(divps), E2(minps), E2(maxps), E2(sqrtps),
    E2(addpd), E2(subpd), E2(mulpd), E2(divpd), E2(minpd), E2(maxpd), E2(sqrtpd),
    E2(addss), E2(subss), E2(mulss), E2(divss), E2(minss), E2(maxss), E2(sqrtss),
    E2(addsd), E2(subsd), E2(mulsd), E2(divsd), E2(minsd), E2(maxsd), E2(sqrtsd),
    E2(pand), E2(pandn), E2(por), E2(pxor),
    E2(paddb), E2(paddw), E2(paddd), E2(paddq), E2(psubb), E2(psubw), E2(psubd), E2(psubq),
    E2(pcmpeqb), E2(pcmpeqw), E2(pcmpeqd), E2(pcmpgtb), E2(pcmpgtw), E2(pcmpgtd),
    E2(pmuludq), E2(pmullw), E2(pmaxub), E2(pminub), E2(pmaxsw), E2(pminsw), E2(psadbw),
    E2(punpcklbw), E2(punpckhbw), E2(punpcklwd), E2(punpckhwd), E2(punpckldq), E2(punpckhdq),
    E2(punpcklqdq), E2(punpckhqdq), E2(packsswb), E2(packuswb), E2(packssdw),
    E2(pshufd), E2(pshuflw), E2(pshufhw), E2(shufps), E2(shufpd),
    E2(cmpps), E2(cmppd), E2(cmpss), E2(cmpsd), E2(comiss), E2(comisd),
    E2(cvtss2sd), E2(cvtsd2ss), E2(cvtdq2ps), E2(cvttps2dq), E2(cvtps2dq), E2(cvtdq2pd),
    E2(cvttpd2dq), E2(cvtpd2dq), E2(cvtpd2ps), E2(cvtps2pd),
    E2(paddsb), E2(paddsw), E2(paddusb), E2(paddusw), E2(psubsb), E2(psubsw), E2(psubusb), E2(psubusw),
    E2(pavgb), E2(pavgw), E2(pmulhw), E2(pmulhuw), E2(pmaddwd),
    E2(psllwx), E2(pslldx), E2(psllqx), E2(psrlwx), E2(psrldx), E2(psrlqx), E2(psrawx), E2(psradx),
    { "pinsrw_R", pinsrw_R }, { "pinsrw_M", pinsrw_M }, { "pextrw_R", pextrw_R },
    E2(movx), { "movx_MR", movx_MR },
    { "movss_RR", movss_RR }, { "movss_RM", movss_RM }, { "movss_MR", movss_MR },
    { "movsd_RR", movsd_RR }, { "movsd_RM", movsd_RM }, { "movsd_MR", movsd_MR },
    { "movq_RR", movq_RR }, { "movlps_RM", movlps_RM }, { "movhps_RM", movhps_RM }, { "movhps_MR", movhps_MR },
    { "movhlps_RR", movhlps_RR }, { "movlhps_RR", movlhps_RR },
    { "movd_XR32", movd_XR32 }, { "movd_XR64", movd_XR64 }, { "movd_RX32", movd_RX32 }, { "movd_RX64", movd_RX64 },
    { "pmovmskb_RR", pmovmskb_RR }, { "movmskps_RR", movmskps_RR }, { "movmskpd_RR", movmskpd_RR },
    { "psllw_RI", psllw_RI }, { "pslld_RI", pslld_RI }, { "psllq_RI", psllq_RI },
    { "psrlw_RI", psrlw_RI }, { "psrld_RI", psrld_RI }, { "psrlq_RI", psrlq_RI },
    { "psraw_RI", psraw_RI }, { "psrad_RI", psrad_RI }, { "pslldq_RI", pslldq_RI }, { "psrldq_RI", psrldq_RI },
    E4CVT(cvtsi2ss), E4CVT(cvtsi2sd), E4CVT(cvttss2si), E4CVT(cvtss2si), E4CVT(cvttsd2si), E4CVT(cvtsd2si),
    { "pinsrd_R", pinsrd_R }, { "pinsrd_M", pinsrd_M }, { "pinsrq_R", pinsrq_R }, { "pinsrq_M", pinsrq_M },
    { "pextrd_R", pextrd_R }, { "pextrd_M", pextrd_M }, { "pextrq_R", pextrq_R }, { "pextrq_M", pextrq_M },
};

OpFn fxi_sse_named(const char *name) {
    for (size_t i = 0; i < sizeof kSse / sizeof kSse[0]; i++)
        if (!strcmp(kSse[i].name, name)) return kSse[i].fn;
    return 0;
}
