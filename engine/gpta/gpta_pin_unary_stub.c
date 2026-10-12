// SPDX-License-Identifier: GPL-3.0-or-later
// INC/DEC keeps incoming CF. On a fused branch's flag-reading edge, recover CF
// from the unchanged incoming descriptor, then reconstruct the unary operands.
#include "gpta_pin.h"
#include "gpta_pin_unary_stub.h"

FXI_INLINE uint64_t unary_stub_cf(uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3) {
    unsigned op = (uint32_t)F0, kind = op >> 2, si = op & 3;
    uint64_t m = kMask[si], a = F1 & m, b = F2 & m, r = F3 & m, ci = (F0 >> 32) & 1;
    unsigned n = (unsigned)F2, bits = 8u << si;
    switch (kind) {
    case LF_ADD: return r < a;
    case LF_ADC: return ci ? r <= a : r < a;
    case LF_SUB: return a < b;
    case LF_SBB: return ci ? a <= b : a < b;
    case LF_LOGIC: return 0;
    case LF_INC: case LF_DEC: case LF_MUL: case LF_RAW: return ci;
    case LF_NEG: return a != 0;
    case LF_SHL: return n && n <= bits ? (a >> (bits - n)) & 1 : 0;
    case LF_SHR: return n && n <= 64 ? (a >> (n - 1)) & 1 : 0;
    case LF_SAR: {
        int64_t s = (int64_t)(a << (64 - bits)) >> (64 - bits);
        return n >= 64 ? (uint64_t)(s < 0) : n ? (uint64_t)(s >> (n - 1)) & 1 : 0;
    }
    default: return 0;
    }
}
#define DEF_UNARY_STUB(D,SZ) \
    PH p_fs_dec_##SZ##_##D(GPTA_PARAMS) { \
        uint64_t ci = unary_stub_cf(F0,F1,F2,F3), r = g##D & M##SZ, a = (r + 1) & M##SZ; \
        SETF(LF_DEC,SI##SZ,a,1,r,ci); PCHAIN(u->ulink,p_miss_t); } \
    PH p_fs_inc_##SZ##_##D(GPTA_PARAMS) { \
        uint64_t ci = unary_stub_cf(F0,F1,F2,F3), r = g##D & M##SZ, a = (r - 1) & M##SZ; \
        SETF(LF_INC,SI##SZ,a,1,r,ci); PCHAIN(u->ulink,p_miss_t); }
R16(DEF_UNARY_STUB,32) R16(DEF_UNARY_STUB,64)
#define E_UNARY_STUB(D,OP,SZ) p_fs_##OP##_##SZ##_##D,
#define T_UNARY_STUB(OP) { { R16(E_UNARY_STUB,OP,32) }, { R16(E_UNARY_STUB,OP,64) } }
const PFn t_fs_unary[2][2][16] = { T_UNARY_STUB(dec), T_UNARY_STUB(inc) };
