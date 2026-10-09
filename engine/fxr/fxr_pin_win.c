// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: pinned forms of instructions Windows games run often that had none, so they ran FXI's
// handler through p_slow (every register spilled and reloaded around the call, the time of about
// 15 pinned uops). Build 118's Stick Fight profile named them: inc/dec of a counter in memory
// (Mono), 8/16-bit loads into a register's low part, loads from gs:[disp] (the TEB: __chkstk's
// stack limit, the TLS slots Mono's generated code reads), and one-operand mul/imul/div/idiv
// (compilers divide by constants with mul; hash tables take remainders with div).
// Same results and lazy flags as FXI's handlers; each writes guest state only after its memory
// access (exact faults, precise.py). See fxr_pin.c.

#include "fxr_pin.h"

// ---- inc dec not neg [T] (8/16/32/64). inc/dec keep CF, so recording their flags reads it; not
// leaves the flags alone. ----
#define UNT_inc(a, SZ) (((a) + 1) & M##SZ)
#define UNT_dec(a, SZ) (((a) - 1) & M##SZ)
#define UNT_not(a, SZ) (~(a) & M##SZ)
#define UNT_neg(a, SZ) ((0 - (a)) & M##SZ)
#define UNTCF_inc 1
#define UNTCF_dec 1
#define UNTCF_not 0
#define UNTCF_neg 0
#define UNTF_inc(a, r, ci, SZ) SETF(LF_INC, SI##SZ, a, 1, r, ci)
#define UNTF_dec(a, r, ci, SZ) SETF(LF_DEC, SI##SZ, a, 1, r, ci)
#define UNTF_not(a, r, ci, SZ) ((void)0)
#define UNTF_neg(a, r, ci, SZ) SETF(LF_NEG, SI##SZ, a, 0, r, 0)
#define DEF_UNT(OPN, SZ, FL)                                                               \
    PH p_unt_##OPN##_##SZ##FL(FXR_PARAMS) {                                                \
        uint64_t ci = 0;                                                                   \
        if (FLV_##FL && UNTCF_##OPN) { int s_ = 0; ci = pcf(F0, F1, F2, F3, &s_); if (FXI_UNLIKELY(s_)) PTAIL(p_slow); } \
        uint64_t a = ld##SZ(T), r = UNT_##OPN(a, SZ);                                      \
        st##SZ(T, r);                                                                      \
        (void)ci;                                                                          \
        if (FLV_##FL) { KEEP_F(); UNTF_##OPN(a, r, ci, SZ); }                              \
        PNEXT();                                                                           \
    }
#define DEF_UNT_SZ(OPN, FL) DEF_UNT(OPN, 8, FL) DEF_UNT(OPN, 16, FL) DEF_UNT(OPN, 32, FL) DEF_UNT(OPN, 64, FL)
#define DEF_UNT_FL(FL) DEF_UNT_SZ(inc, FL) DEF_UNT_SZ(dec, FL) DEF_UNT_SZ(not, FL) DEF_UNT_SZ(neg, FL)
DEF_UNT_FL(F) DEF_UNT_FL(N)
#define T_UNT_OP(OPN, FL) { p_unt_##OPN##_8##FL, p_unt_##OPN##_16##FL, p_unt_##OPN##_32##FL, p_unt_##OPN##_64##FL }
#define T_UNT(FL) { T_UNT_OP(inc, FL), T_UNT_OP(dec, FL), T_UNT_OP(not, FL), T_UNT_OP(neg, FL) }
const PFn t_unt[2][4][4] = { T_UNT(N), T_UNT(F) };   // [flags live][inc dec not neg][8 16 32 64], operand at T

// ---- mov r8/r16, [T]: the low byte/word of D, the rest kept ----
#define DEF_LDP(D, _)                                                                      \
    PH p_ldp8_##D(FXR_PARAMS) { uint64_t v = ld8(T); KEEP_R(D); g##D = (g##D & ~M8) | v; PNEXT(); }   \
    PH p_ldp16_##D(FXR_PARAMS) { uint64_t v = ld16(T); KEEP_R(D); g##D = (g##D & ~M16) | v; PNEXT(); }
R16(DEF_LDP, _)
const PFn t_ldp[2][16] = { { R16(E1, ldp8) }, { R16(E1, ldp16) } };   // [8 16][D]

// ---- mov r32/r64, gs:[disp] (Windows: GS is the TEB, in the CPU structure) ----
// (D is kept until the load is done: the compiler would hold the TEB's address in it.)
#define DEF_LDGS(D, _)                                                                     \
    PH p_ldgs32_##D(FXR_PARAMS) { uint64_t v = ld32(c->r[R_GS] + (uint64_t)u->disp); KEEP_R(D); g##D = v; PNEXT(); } \
    PH p_ldgs64_##D(FXR_PARAMS) { uint64_t v = ld64(c->r[R_GS] + (uint64_t)u->disp); KEEP_R(D); g##D = v; PNEXT(); }
R16(DEF_LDGS, _)
const PFn t_ldgs[2][16] = { { R16(E1, ldgs32) }, { R16(E1, ldgs64) } };   // [32 64][D]

// ---- one-operand mul imul div idiv, 32/64-bit: rDX:rAX with a register (0-15) or [T] (16) ----
// Flags as FXI records them: mul/imul CF = OF = the high half is not the low half's extension
// (LF_MUL), div/idiv leave them. A divide error, a 64-bit div whose dividend needs rdx (a 128-bit
// division would be a library call here) and idiv by -1 run FXI's handler, before anything changed.
#define MDS_R(S, SZ) (g##S & M##SZ)
#define MDS_T(S, SZ) ld##SZ(T)
#define DEF_MD(S, FORM, SZ, FL)                                                            \
    PH p_mul##FORM##_##SZ##FL##_##S(FXR_PARAMS) {                                          \
        uint64_t b = MDS_##FORM(S, SZ), lo, hi;                                            \
        if (SZ == 64) { unsigned __int128 p = (unsigned __int128)g0 * b; lo = (uint64_t)p; hi = (uint64_t)(p >> 64); } \
        else { uint64_t p = (g0 & M32) * b; lo = p & M32; hi = p >> 32; }                  \
        KEEP_R(0); KEEP_R(2); if (FLV_##FL) KEEP_F();                                                        \
        g0 = lo; g2 = hi;                                                                  \
        if (FLV_##FL) SETF(LF_MUL, SI##SZ, 0, 0, lo, hi != 0);                             \
        PNEXT();                                                                           \
    }                                                                                      \
    PH p_imul##FORM##_##SZ##FL##_##S(FXR_PARAMS) {                                         \
        uint64_t b = MDS_##FORM(S, SZ), lo, hi;                                            \
        int ov;                                                                            \
        if (SZ == 64) {                                                                    \
            __int128 p = (__int128)(int64_t)g0 * (int64_t)b;                               \
            lo = (uint64_t)p; hi = (uint64_t)((unsigned __int128)p >> 64); ov = p != (int64_t)p; \
        } else {                                                                           \
            int64_t p = (int64_t)(int32_t)g0 * (int32_t)b;                                 \
            lo = (uint64_t)p & M32; hi = ((uint64_t)p >> 32) & M32; ov = p != (int32_t)p;  \
        }                                                                                  \
        KEEP_R(0); KEEP_R(2); if (FLV_##FL) KEEP_F();                                                        \
        g0 = lo; g2 = hi;                                                                  \
        if (FLV_##FL) SETF(LF_MUL, SI##SZ, 0, 0, lo, ov);                                  \
        PNEXT();                                                                           \
    }
#define DEF_DIV(S, FORM, SZ)                                                               \
    PH p_div##FORM##_##SZ##_##S(FXR_PARAMS) {                                              \
        uint64_t d = MDS_##FORM(S, SZ), q, r;                                              \
        if (SZ == 64) {                                                                    \
            if (FXI_UNLIKELY(!d || g2)) PTAIL(p_slow);                                     \
            q = g0 / d; r = g0 % d;                                                        \
        } else {                                                                           \
            uint64_t n = (g2 & M32) << 32 | (g0 & M32);                                    \
            if (FXI_UNLIKELY(!d || n / d > M32)) PTAIL(p_slow);                            \
            q = n / d; r = n % d;                                                          \
        }                                                                                  \
        KEEP_R(0); KEEP_R(2);                                                              \
        g0 = q; g2 = r;                                                                    \
        PNEXT();                                                                           \
    }                                                                                      \
    PH p_idiv##FORM##_##SZ##_##S(FXR_PARAMS) {                                             \
        int64_t q, r;                                                                      \
        if (SZ == 64) {                                                                    \
            int64_t d = (int64_t)MDS_##FORM(S, 64), n = (int64_t)g0;                       \
            if (FXI_UNLIKELY(!d || d == -1 || g2 != (uint64_t)(n >> 63))) PTAIL(p_slow);   \
            q = n / d; r = n % d;                                                          \
        } else {                                                                           \
            int64_t d = (int32_t)MDS_##FORM(S, 32), n = (int64_t)((g2 & M32) << 32 | (g0 & M32)); \
            if (FXI_UNLIKELY(!d || d == -1 || n / d > INT32_MAX || n / d < INT32_MIN)) PTAIL(p_slow); \
            q = n / d; r = n % d;                                                          \
        }                                                                                  \
        KEEP_R(0); KEEP_R(2);                                                              \
        g0 = (uint64_t)q & M##SZ; g2 = (uint64_t)r & M##SZ;                                \
        PNEXT();                                                                           \
    }
#define DEF_MD_R(S, _) DEF_MD(S, R, 32, F) DEF_MD(S, R, 64, F) DEF_MD(S, R, 32, N) DEF_MD(S, R, 64, N) \
    DEF_DIV(S, R, 32) DEF_DIV(S, R, 64)
R16(DEF_MD_R, _)
DEF_MD(M, T, 32, F) DEF_MD(M, T, 64, F) DEF_MD(M, T, 32, N) DEF_MD(M, T, 64, N) DEF_DIV(M, T, 32) DEF_DIV(M, T, 64)
#define E_MDR(S, NAME, SZFL) p_##NAME##R_##SZFL##_##S,
#define T_MD(NAME, SZFL) { R16(E_MDR, NAME, SZFL) p_##NAME##T_##SZFL##_M }
const PFn t_mul[2][2][2][17] = {   // [mul imul][64?][flags live][source register, 16 = [T]]
    { { T_MD(mul, 32N), T_MD(mul, 32F) }, { T_MD(mul, 64N), T_MD(mul, 64F) } },
    { { T_MD(imul, 32N), T_MD(imul, 32F) }, { T_MD(imul, 64N), T_MD(imul, 64F) } },
};
const PFn t_div[2][2][17] = {   // [div idiv][64?][source register, 16 = [T]]
    { T_MD(div, 32), T_MD(div, 64) }, { T_MD(idiv, 32), T_MD(idiv, 64) },
};
