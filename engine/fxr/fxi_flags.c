// SPDX-License-Identifier: GPL-3.0-or-later
// FXI lazy flags: a flag is computed from the last flag-writing operation's
// inputs and result only when something reads it.

#include "fxi_internal.h"

static const unsigned kBits[4] = { 8, 16, 32, 64 };
static inline uint64_t mask_of(unsigned si) { return si == 3 ? ~0ull : ((1ull << kBits[si]) - 1); }
static inline uint64_t sign_of(unsigned si) { return 1ull << (kBits[si] - 1); }
static inline int64_t sext(uint64_t v, unsigned si) {
    unsigned sh = 64 - kBits[si];
    return (int64_t)(v << sh) >> sh;
}

uint64_t fxi_flag_cf(FxiCpu *c) {
    unsigned k = c->lf_op >> 2, si = c->lf_op & 3;
    uint64_t m = mask_of(si), a = c->lf_a & m, b = c->lf_b & m, r = c->lf_res & m;
    switch (k) {
    case LF_ADD: return r < a;
    case LF_ADC: return c->lf_cin ? r <= a : r < a;
    case LF_SUB: return a < b;
    case LF_SBB: return c->lf_cin ? a <= b : a < b;
    case LF_LOGIC: return 0;
    case LF_INC: case LF_DEC: return c->lf_cin & 1;
    case LF_NEG: return a != 0;
    case LF_SHL: { unsigned n = (unsigned)c->lf_b; return n && n <= kBits[si] ? (a >> (kBits[si] - n)) & 1 : 0; }
    case LF_SHR: { unsigned n = (unsigned)c->lf_b; return n && n <= 64 ? (a >> (n - 1)) & 1 : 0; }
    case LF_SAR: { unsigned n = (unsigned)c->lf_b; int64_t s = sext(a, si);
                   return n >= 64 ? (uint64_t)(s < 0) : (uint64_t)(s >> (n - 1)) & 1; }
    case LF_MUL: return c->lf_cin & 1;
    case LF_RAW: return c->lf_cin & 1;
    }
    return 0;
}

uint64_t fxi_flag_of(FxiCpu *c) {
    unsigned k = c->lf_op >> 2, si = c->lf_op & 3;
    uint64_t m = mask_of(si), sb = sign_of(si), a = c->lf_a & m, b = c->lf_b & m, r = c->lf_res & m;
    switch (k) {
    case LF_ADD: case LF_ADC: return (((a ^ r) & (b ^ r)) & sb) != 0;
    case LF_SUB: case LF_SBB: return (((a ^ b) & (a ^ r)) & sb) != 0;
    case LF_LOGIC: return 0;
    case LF_INC: return r == sb;
    case LF_DEC: return r == sb - 1;
    case LF_NEG: return a == sb;
    case LF_SHL: return ((r & sb) != 0) ^ fxi_flag_cf(c);
    case LF_SHR: return (a & sb) != 0;
    case LF_SAR: return 0;
    case LF_MUL: return c->lf_cin & 1;
    case LF_RAW: return (c->lf_cin >> 11) & 1;
    }
    return 0;
}

uint64_t fxi_flag_zf(FxiCpu *c) {
    if ((c->lf_op >> 2) == LF_RAW) return (c->lf_cin >> 6) & 1;
    return (c->lf_res & mask_of(c->lf_op & 3)) == 0;
}

uint64_t fxi_flag_sf(FxiCpu *c) {
    if ((c->lf_op >> 2) == LF_RAW) return (c->lf_cin >> 7) & 1;
    return (c->lf_res & sign_of(c->lf_op & 3)) != 0;
}

uint64_t fxi_flag_pf(FxiCpu *c) {
    if ((c->lf_op >> 2) == LF_RAW) return (c->lf_cin >> 2) & 1;
    return !__builtin_parity((unsigned)(c->lf_res & 0xff));
}

uint64_t fxi_flag_af(FxiCpu *c) {
    unsigned k = c->lf_op >> 2;
    switch (k) {
    case LF_ADD: case LF_ADC: case LF_SUB: case LF_SBB: case LF_INC: case LF_DEC:
        return ((c->lf_a ^ c->lf_b ^ c->lf_res) >> 4) & 1;
    case LF_NEG: return ((c->lf_a ^ c->lf_res) >> 4) & 1;
    case LF_RAW: return (c->lf_cin >> 4) & 1;
    }
    return 0;
}

uint64_t fxi_rflags(FxiCpu *c) {
    return fxi_flag_cf(c) | 1u << 1 | fxi_flag_pf(c) << 2 | fxi_flag_af(c) << 4 | fxi_flag_zf(c) << 6 |
           fxi_flag_sf(c) << 7 | (uint64_t)1 << 9 /* IF */ | (uint64_t)(c->df & 1) << 10 | fxi_flag_of(c) << 11;
}

void fxi_set_rflags(FxiCpu *c, uint64_t f) {
    c->lf_op = LF(LF_RAW, 3);
    c->lf_cin = (uint32_t)(f & 0x8d5);   // CF PF AF ZF SF OF
    c->df = (f >> 10) & 1;
}

int fxi_cond(FxiCpu *c, unsigned cc) {
    unsigned k = c->lf_op >> 2, si = c->lf_op & 3;
    if (k == LF_SUB) {
        uint64_t m = mask_of(si), a = c->lf_a & m, b = c->lf_b & m;
        int64_t sa = sext(a, si), sb = sext(b, si);
        switch (cc) {
        case 2: return a < b;   case 3: return a >= b;
        case 4: return a == b;  case 5: return a != b;
        case 6: return a <= b;  case 7: return a > b;
        case 12: return sa < sb; case 13: return sa >= sb;
        case 14: return sa <= sb; case 15: return sa > sb;
        }
    } else if (k == LF_LOGIC) {
        uint64_t r = c->lf_res & mask_of(si);
        int neg = (r & sign_of(si)) != 0;
        switch (cc) {
        case 0: return 0; case 1: return 1; case 2: return 0; case 3: return 1;
        case 4: return r == 0; case 5: return r != 0;
        case 6: return r == 0; case 7: return r != 0;
        case 8: return neg; case 9: return !neg;
        case 12: return neg; case 13: return !neg;
        case 14: return r == 0 || neg; case 15: return r != 0 && !neg;
        }
    }
    int v;
    switch (cc >> 1) {
    case 0: v = (int)fxi_flag_of(c); break;
    case 1: v = (int)fxi_flag_cf(c); break;
    case 2: v = (int)fxi_flag_zf(c); break;
    case 3: v = (int)(fxi_flag_cf(c) | fxi_flag_zf(c)); break;
    case 4: v = (int)fxi_flag_sf(c); break;
    case 5: v = (int)fxi_flag_pf(c); break;
    case 6: v = fxi_flag_sf(c) != fxi_flag_of(c); break;
    default: v = fxi_flag_zf(c) || fxi_flag_sf(c) != fxi_flag_of(c); break;
    }
    return v ^ (int)(cc & 1);
}
