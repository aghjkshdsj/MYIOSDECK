// SPDX-License-Identifier: GPL-3.0-or-later
// Generated from fxi_sse.c (the DEF_X definition block, minus comiss/comisd and set_fcmp_flags):
// FXR includes it with DEF_X redefined, so its pinned SSE handlers have FXI's exact semantics.
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
DEF_X(cvtpd2ps, 16, { float a = (float)S->g[0], b = (float)S->g[1]; D->f[0] = a; D->f[1] = b; D->q[1] = 0; })
DEF_X(cvtps2pd, 8, { double a = S->f[0], b = S->f[1]; D->g[0] = a; D->g[1] = b; })
DEF_X(movx, 16, *D = *S)
