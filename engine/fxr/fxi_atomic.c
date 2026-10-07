// SPDX-License-Identifier: GPL-3.0-or-later
// FXI atomic and bit-test instructions: cmpxchg, xadd, xchg with memory, cmpxchg8b/16b,
// lock-prefixed read-modify-write ALU ops, bt/bts/btr/btc.
//
// Windows threads share memory, so every memory form here is a real host atomic
// (sequentially consistent, like x86's locked operations). An unlocked cmpxchg/xadd/btX on
// memory is executed atomically too: a stronger guarantee than x86 gives, never a weaker one.
// A misaligned operand cannot be a host atomic on ARM64; it falls back to one global lock
// (x86 allows such "split locks"; compilers never emit them for the Interlocked* APIs).
//
// Uop fields: u->cc = operand size index (0-3), u->dst = register operand of the R forms,
// u->src = source register (0xffff: immediate in u->imm), u->aux = operation.

#include "fxi_internal.h"

static const uint64_t kMask[4] = { 0xffull, 0xffffull, 0xffffffffull, ~0ull };

FXI_INLINE uint64_t rd_si(FxiCpu *c, unsigned off, unsigned si) {
    return si == 0 ? rd8(c, off) : si == 1 ? rd16(c, off) : si == 2 ? rd32(c, off) : rd64(c, off);
}
FXI_INLINE void wr_si(FxiCpu *c, unsigned off, unsigned si, uint64_t v) {
    if (si == 0) wr8(c, off, v); else if (si == 1) wr16(c, off, v); else if (si == 2) wr32(c, off, v); else wr64(c, off, v);
}
FXI_INLINE uint64_t ld_si(uint64_t a, unsigned si) {
    return si == 0 ? ld8(a) : si == 1 ? ld16(a) : si == 2 ? ld32(a) : ld64(a);
}
FXI_INLINE void st_si(uint64_t a, unsigned si, uint64_t v) {
    if (si == 0) st8(a, v); else if (si == 1) st16(a, v); else if (si == 2) st32(a, v); else st64(a, v);
}

// ---- split locks (misaligned atomics) ----
static int g_split;
static void split_lock(void) { while (__atomic_test_and_set(&g_split, __ATOMIC_ACQUIRE)) { } }
static void split_unlock(void) { __atomic_clear(&g_split, __ATOMIC_RELEASE); }

// Compare-and-swap of 1/2/4/8 bytes. On failure *expected receives the current value.
static int cas(uint64_t addr, unsigned si, uint64_t *expected, uint64_t desired) {
    if (FXI_LIKELY((addr & ((1u << si) - 1)) == 0)) {
        void *p = (void *)(uintptr_t)addr;
        int ok;
        switch (si) {
        case 0: { uint8_t e = (uint8_t)*expected;
                  ok = __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                  *expected = e; return ok; }
        case 1: { uint16_t e = (uint16_t)*expected;
                  ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                  *expected = e; return ok; }
        case 2: { uint32_t e = (uint32_t)*expected;
                  ok = __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                  *expected = e; return ok; }
        default: { uint64_t e = *expected;
                  ok = __atomic_compare_exchange_n((uint64_t *)p, &e, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                  *expected = e; return ok; }
        }
    }
    split_lock();
    uint64_t cur = ld_si(addr, si);
    int ok = cur == (*expected & kMask[si]);
    if (ok) st_si(addr, si, desired); else *expected = cur;
    split_unlock();
    return ok;
}

// ALU operations (ALU_* from fxi_internal.h, plus NEG) and their lazy-flag kinds.
enum { OP_NEG = ALU_COUNT };
static const uint8_t kAluKind[ALU_COUNT + 1] = {
    [ALU_ADD] = LF_ADD, [ALU_OR] = LF_LOGIC, [ALU_ADC] = LF_ADC, [ALU_SBB] = LF_SBB,
    [ALU_AND] = LF_LOGIC, [ALU_SUB] = LF_SUB, [ALU_XOR] = LF_LOGIC, [OP_NEG] = LF_NEG,
};
FXI_INLINE uint64_t alu(unsigned op, uint64_t a, uint64_t b, uint64_t cin) {
    switch (op) {
    case ALU_ADD: return a + b;
    case ALU_OR: return a | b;
    case ALU_ADC: return a + b + cin;
    case ALU_SBB: return a - b - cin;
    case ALU_AND: return a & b;
    case ALU_SUB: return a - b;
    case ALU_XOR: return a ^ b;
    default: return 0 - a;   // OP_NEG
    }
}

// Atomic read-modify-write; returns the old value, *res the new one.
static uint64_t rmw(uint64_t addr, unsigned si, unsigned op, uint64_t b, uint64_t cin, uint64_t *res) {
    uint64_t old = ld_si(addr, si);
    for (;;) {
        uint64_t r = alu(op, old, b, cin) & kMask[si];
        if (cas(addr, si, &old, r)) { *res = r; return old; }
    }
}

FXI_INLINE void set_lf(FxiCpu *c, unsigned kind, unsigned si, uint64_t a, uint64_t b, uint64_t res, uint64_t cin) {
    c->lf_op = LF(kind, si); c->lf_a = a; c->lf_b = b; c->lf_res = res; c->lf_cin = (uint32_t)cin;
}

// lock add/or/adc/sbb/and/sub/xor m, r|imm (u->aux = ALU op)
static void op_lock_alu(FxiCpu *c, Uop *u) {
    unsigned si = u->cc, op = (unsigned)u->aux;
    uint64_t cin = (op == ALU_ADC || op == ALU_SBB) ? fxi_flag_cf(c) : 0;
    uint64_t b = (u->src == 0xffff ? u->imm : rd_si(c, u->src, si)) & kMask[si], res;
    uint64_t old = rmw(fxi_ea(c, u), si, op, b, cin, &res);
    set_lf(c, kAluKind[op], si, old, b, res, cin);
    FXI_NEXT(c, u);
}

// lock inc/dec/not/neg m (u->aux = U_INC/U_DEC/U_NOT/U_NEG)
static void op_lock_unary(FxiCpu *c, Uop *u) {
    unsigned si = u->cc, op = (unsigned)u->aux;
    uint64_t addr = fxi_ea(c, u), res, old;
    switch (op) {
    case U_INC: { uint64_t cf = fxi_flag_cf(c); old = rmw(addr, si, ALU_ADD, 1, 0, &res); set_lf(c, LF_INC, si, old, 1, res, cf); break; }
    case U_DEC: { uint64_t cf = fxi_flag_cf(c); old = rmw(addr, si, ALU_SUB, 1, 0, &res); set_lf(c, LF_DEC, si, old, 1, res, cf); break; }
    case U_NOT: rmw(addr, si, ALU_XOR, kMask[si], 0, &res); break;
    default: old = rmw(addr, si, OP_NEG, 0, 0, &res); set_lf(c, LF_NEG, si, old, 0, res, 0); break;
    }
    FXI_NEXT(c, u);
}

// xchg m, r: always locked on x86
static void op_xchg_M(FxiCpu *c, Uop *u) {
    unsigned si = u->cc;
    uint64_t addr = fxi_ea(c, u), v = rd_si(c, u->src, si), old = ld_si(addr, si);
    while (!cas(addr, si, &old, v)) { }
    wr_si(c, u->src, si, old);
    FXI_NEXT(c, u);
}

// cmpxchg r/m, r: compare the accumulator with the destination (flags as cmp acc, dest);
// equal: dest = src; else: acc = dest.
static void op_cmpxchg_M(FxiCpu *c, Uop *u) {
    unsigned si = u->cc;
    uint64_t acc = rd_si(c, R_AX * 8, si), old = acc;
    int ok = cas(fxi_ea(c, u), si, &old, rd_si(c, u->src, si));
    set_lf(c, LF_SUB, si, acc, old, (acc - old) & kMask[si], 0);
    if (!ok) wr_si(c, R_AX * 8, si, old);
    FXI_NEXT(c, u);
}
static void op_cmpxchg_R(FxiCpu *c, Uop *u) {
    unsigned si = u->cc;
    uint64_t acc = rd_si(c, R_AX * 8, si), old = rd_si(c, u->dst, si);
    set_lf(c, LF_SUB, si, acc, old, (acc - old) & kMask[si], 0);
    if (acc == old) wr_si(c, u->dst, si, rd_si(c, u->src, si));
    else { wr_si(c, R_AX * 8, si, old); wr_si(c, u->dst, si, old); }   // dest is written back either way
    FXI_NEXT(c, u);
}

// xadd r/m, r: tmp = dest + src; src = dest; dest = tmp
static void op_xadd_M(FxiCpu *c, Uop *u) {
    unsigned si = u->cc;
    uint64_t b = rd_si(c, u->src, si), res;
    uint64_t old = rmw(fxi_ea(c, u), si, ALU_ADD, b, 0, &res);
    wr_si(c, u->src, si, old);
    set_lf(c, LF_ADD, si, old, b, res, 0);
    FXI_NEXT(c, u);
}
static void op_xadd_R(FxiCpu *c, Uop *u) {
    unsigned si = u->cc;
    uint64_t a = rd_si(c, u->dst, si), b = rd_si(c, u->src, si), res = (a + b) & kMask[si];
    wr_si(c, u->src, si, a);
    wr_si(c, u->dst, si, res);
    set_lf(c, LF_ADD, si, a, b, res, 0);
    FXI_NEXT(c, u);
}

// cmpxchg8b / cmpxchg16b m: compare rDX:rAX with m; equal: m = rCX:rBX, ZF = 1;
// else rDX:rAX = m, ZF = 0. No other flag changes.
static void set_zf(FxiCpu *c, int zf) { fxi_set_rflags(c, (fxi_rflags(c) & ~0x40ull) | (uint64_t)(zf != 0) << 6); }

static void op_cmpxchg8b(FxiCpu *c, Uop *u) {
    uint64_t addr = fxi_ea(c, u);
    uint64_t exp = (c->r[R_DX] & 0xffffffffull) << 32 | (c->r[R_AX] & 0xffffffffull), old = exp;
    uint64_t des = (c->r[R_CX] & 0xffffffffull) << 32 | (c->r[R_BX] & 0xffffffffull);
    int ok = cas(addr, 3, &old, des);
    if (!ok) { c->r[R_AX] = (uint32_t)old; c->r[R_DX] = old >> 32; }
    set_zf(c, ok);
    FXI_NEXT(c, u);
}

static void op_cmpxchg16b(FxiCpu *c, Uop *u) {
    uint64_t addr = fxi_ea(c, u);
    unsigned __int128 exp = (unsigned __int128)c->r[R_DX] << 64 | c->r[R_AX];
    unsigned __int128 des = (unsigned __int128)c->r[R_CX] << 64 | c->r[R_BX];
    unsigned __int128 old = exp;
    int ok;
    if (FXI_LIKELY((addr & 15) == 0)) {
        ok = __atomic_compare_exchange_n((unsigned __int128 *)(uintptr_t)addr, &old, des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else {   // #GP on x86; keep going under the split lock
        split_lock();
        memcpy(&old, (const void *)(uintptr_t)addr, 16);
        ok = old == exp;
        if (ok) memcpy((void *)(uintptr_t)addr, &des, 16);
        split_unlock();
    }
    if (!ok) { c->r[R_AX] = (uint64_t)old; c->r[R_DX] = (uint64_t)(old >> 64); }
    set_zf(c, ok);
    FXI_NEXT(c, u);
}

// bt/bts/btr/btc (u->aux = 0 bt, 1 bts, 2 btr, 3 btc). Only CF changes.
enum { BT_TEST, BT_SET, BT_RESET, BT_COMPLEMENT };
static void set_cf(FxiCpu *c, uint64_t cf) { fxi_set_rflags(c, (fxi_rflags(c) & ~1ull) | (cf & 1)); }

// Register destination: the bit index wraps at the operand size.
static void op_bitop_R(FxiCpu *c, Uop *u) {
    unsigned si = u->cc, bits = 8u << si, op = (unsigned)u->aux;
    uint64_t v = rd_si(c, u->dst, si);
    unsigned n = (unsigned)(u->src == 0xffff ? u->imm : c->r[u->src >> 3]) & (bits - 1);
    uint64_t m = 1ull << n;
    set_cf(c, v >> n);
    if (op == BT_SET) wr_si(c, u->dst, si, v | m);
    else if (op == BT_RESET) wr_si(c, u->dst, si, v & ~m);
    else if (op == BT_COMPLEMENT) wr_si(c, u->dst, si, v ^ m);
    FXI_NEXT(c, u);
}

// Memory destination: an immediate index wraps at the operand size; a register index is a
// signed bit offset from the operand (bt [rbx], rax with rax = 70 tests bit 6 of [rbx+8]).
static void op_bitop_M(FxiCpu *c, Uop *u) {
    unsigned si = u->cc, bits = 8u << si, op = (unsigned)u->aux;
    uint64_t addr = fxi_ea(c, u);
    unsigned n;
    if (u->src == 0xffff) n = (unsigned)u->imm & (bits - 1);
    else {
        uint64_t raw = rd_si(c, u->src, si);
        int64_t off = si == 1 ? (int16_t)raw : si == 2 ? (int32_t)raw : (int64_t)raw;
        addr += (uint64_t)((off >> (si + 3)) << si);
        n = (unsigned)off & (bits - 1);
    }
    uint64_t m = 1ull << n, old, res;
    if (op == BT_TEST) old = ld_si(addr, si);
    else old = rmw(addr, si, op == BT_SET ? ALU_OR : op == BT_RESET ? ALU_AND : ALU_XOR,
                   op == BT_RESET ? ~m & kMask[si] : m, 0, &res);
    set_cf(c, old >> n);
    FXI_NEXT(c, u);
}

static const struct { const char *name; OpFn fn; } kAtomic[] = {
    { "lock_alu", op_lock_alu }, { "lock_unary", op_lock_unary }, { "xchg_M", op_xchg_M },
    { "cmpxchg_M", op_cmpxchg_M }, { "cmpxchg_R", op_cmpxchg_R }, { "xadd_M", op_xadd_M }, { "xadd_R", op_xadd_R },
    { "cmpxchg8b", op_cmpxchg8b }, { "cmpxchg16b", op_cmpxchg16b }, { "bitop_R", op_bitop_R }, { "bitop_M", op_bitop_M },
};

OpFn fxi_atomic_named(const char *name) {
    for (size_t i = 0; i < sizeof kAtomic / sizeof kAtomic[0]; i++)
        if (!strcmp(kAtomic[i].name, name)) return kAtomic[i].fn;
    return 0;
}
