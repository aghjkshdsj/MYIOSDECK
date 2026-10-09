// SPDX-License-Identifier: GPL-3.0-or-later
// FXI internals: CPU state, micro-ops, lazy flags. See docs/FAST_INTERPRETER.md.

#ifndef FXI_INTERNAL_H
#define FXI_INTERNAL_H

#include <stdint.h>
#include <string.h>

#include "fxi.h"

#if !defined(__clang__)
#error FXI needs clang (musttail threaded dispatch)
#endif

#define FXI_LIKELY(x) __builtin_expect(!!(x), 1)
#define FXI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define FXI_INLINE static inline __attribute__((always_inline))

typedef struct FxiCpu FxiCpu;
typedef struct Uop Uop;
typedef struct Block Block;

// A 128-bit XMM register, viewed per lane width.
typedef union {
    uint8_t b[16]; uint16_t w[8]; uint32_t d[4]; uint64_t q[2];
    int8_t sb[16]; int16_t sw[8]; int32_t sd[4]; int64_t sq[2];
    float f[4]; double g[2];
} X128;

// FXR: FXI's handlers are plain functions here (they return instead of tail-calling);
// FXR's pinned handlers (fxr_pin.c) run the chain and call them as a fallback.
typedef void (*OpFn)(FxiCpu *c, Uop *u);

// FXR dispatch: the 16 guest registers, the EA temporary T and the four lazy-flag words
// travel between handlers as arguments, which preserve_none keeps in host registers
// (23 on ARM64: c, u and 21 values). ARM64 preserve_none assigns x20-x28, x0-x7, x10-x14, x9
// in that order: R15 and the uop take x20/x21, the other guest registers x22-x28 and x0-x7, so
// none is in x8-x17, the registers the compiler takes first as scratch (a guest register about
// to be overwritten would serve as scratch before a load that may fault, and its value at the
// fault would be lost: fxr_win_host_state). The CPU pointer (never dead), T and the flag words
// take x10-x14 and x9.
#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none)   // x86 hosts: too few argument registers
#define FXR_CC __attribute__((preserve_none))
#else
#define FXR_CC
#endif
// XMM0-7 travel the same way as vector arguments (v0-v7 on ARM64); XMM8-15 stay in c->xmm.
typedef uint64_t FxrV __attribute__((vector_size(16)));
#define FXR_PARAMS uint64_t g15, Uop *u, uint64_t g0, uint64_t g1, uint64_t g2, uint64_t g6, uint64_t g7, \
    uint64_t g4, uint64_t g3, uint64_t g5, uint64_t g8, uint64_t g9, uint64_t g10, uint64_t g11, uint64_t g12, \
    uint64_t g13, uint64_t g14, FxiCpu *c, uint64_t T, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, \
    FxrV x0, FxrV x1, FxrV x2, FxrV x3, FxrV x4, FxrV x5, FxrV x6, FxrV x7
// The arguments of a handler call for uop U (FXR_PARAMS' order)
#define FXR_CALL(U) (g15, (U), g0, g1, g2, g6, g7, g4, g3, g5, g8, g9, g10, g11, g12, g13, g14, c, T, F0, F1, F2, F3, \
    x0, x1, x2, x3, x4, x5, x6, x7)
typedef FXR_CC void (*PFn)(FXR_PARAMS);

// Register slots. GPRs 0-15 in x86 order (RAX RCX RDX RBX RSP RBP RSI RDI R8-R15);
// slot 16 is always zero, so a missing base or index costs nothing in an EA. Slots 17/18
// hold the FS/GS segment bases: a segment override becomes the EA's free base or index.
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI, R_ZERO = 16, R_FS = 17, R_GS = 18, R_COUNT = 19 };

// Lazy flag kinds (combined with the size index 0-3 for 8/16/32/64 bits).
enum {
    LF_ADD = 0, LF_ADC, LF_SUB, LF_SBB, LF_LOGIC, LF_INC, LF_DEC, LF_NEG,
    LF_SHL, LF_SHR, LF_SAR, LF_MUL, LF_ROL, LF_ROR, LF_RAW,
};
#define LF(kind, szi) ((kind) << 2 | (szi))

// The first three members have fixed offsets: the Windows transition glue
// (App/Sources/Native/fxi_win_glue.S) saves and restores them directly.
struct FxiCpu {
    uint64_t r[R_COUNT];     // 0x00: GPRs, zero slot, FS/GS base; 8-bit regs are byte offsets into this
    uint64_t rip;            // 0x98: only exact at block boundaries and on errors
    _Alignas(16) X128 xmm[16];   // 0xa0
    // Lazy flags: the last flag-writing operation's inputs and result.
    uint64_t lf_res, lf_a, lf_b;
    uint32_t lf_op;          // LF(kind, size index)
    uint32_t lf_cin;         // carry in (ADC/SBB/INC/DEC), CF=OF for MUL, raw RFLAGS for LF_RAW
    uint32_t df;             // direction flag
    uint32_t mxcsr;
    // x87 (fxi_x87.c), values as double: ST(i) = st[(fpu_top + i) & 7]
    double st[8];
    uint32_t fpu_top;
    uint32_t fpu_tags;       // bit p set: physical register p is empty
    uint16_t fcw, fsw;       // control word; status word condition bits (top lives in fpu_top)
    // Run state
    Uop *cur;                // last uop that touched guest memory: a host fault's exact rip (fxi_ea)
    uint32_t exc_code, exc_flags, exc_nparams;   // FXI_STOP_EXCEPTION: the Windows exception
    uint64_t exc_info[2];
    uint64_t trail[16];      // diagnostics: the last block lookups (rip); trail_n is the count
    uint32_t trail_n;
    uint64_t df_rip;         // diagnostics: where DF was last set (df_how: 1 std, 2 popf, 3 context)
    uint32_t df_how;
    uint32_t sysflags;       // EFLAGS.ID (bit 21) and AC (bit 18) as popf left them: CPUID detection toggles ID
    uint64_t n_lookups, n_exits;   // profiler counters: block lookups (first-time edges), exits to native code
    // Return-address prediction: call pushes (return rip, its call uop's link2 slot), ret pops.
    struct { uint64_t rip; Block **slot; } ras[32];
    uint32_t ras_top;
    // Profiler: calls into native code per target (direct-mapped by target, collisions dropped).
    struct { uint64_t target, n; } exit_tab[256];
    int stop;                // nonzero: leave the dispatch chain
    long long exit_code;
    char *err;               // fxi_result.error
    struct Fxi *vm;
    // FXR: indirect branch target cache (per thread, so no races): target rip -> first uop of its
    // block, direct-mapped. Returns, indirect jumps and calls look here before the block table.
    int fxr_ready;
    struct { uint64_t rip; struct Uop *u; } fxr_ibtc[1024];
};

// One decoded instruction (or fused pair). FXR: 80 bytes.
#define FXR_NOBOUND 4      // FXR: Uop.fdir bit
struct Uop {
    PFn p;                   // FXR: the pinned handler that runs this uop (first: the step to the next
                             // uop is one load with writeback and an indirect branch)
    OpFn fn;
    union { Block *link; Uop *ulink; };     // chained successor (taken / call target / inline-cache block);
                                            // FXR's pinned handlers keep the successor's first uop
    union {
        struct {
            uint64_t imm;    // immediate, absolute branch target, or inline-cache rip
            int64_t disp;    // EA displacement (RIP-relative already absolute)
        };
        uint8_t xidx[16];    // FXR: a pinned SSE shuffle's byte indices (the lowering computes them)
    };
    uint64_t aux;            // fallthrough / return address / next rip
    union { Block *link2; Uop *ulink2; };   // fallthrough successor of a conditional branch
    uint16_t dst, src;       // register byte offsets (GPR) or XMM indices
    uint8_t base, index, scale, cc;
    uint64_t rip;            // the instruction's address (exceptions, faults)
    uint8_t flive;           // FXR: arithmetic flags live after this uop (decoder liveness, across blocks)
    uint8_t fdir;            // FXR: a block-ending branch: flags live at its target (bit 0), fallthrough (bit 1);
                             // FXR_NOBOUND: no x64 instruction boundary at this uop's start (fxr_pin.c)
    int32_t fimm;            // FXR: a fused uop's second immediate (imm and disp are taken)
#ifdef FXR_PROFILE
    uint64_t prof;           // FXR diagnostic build: times this uop was dispatched (fxr_profile_dump)
#endif
};

struct Block {
    uint64_t rip;
    uint32_t n;              // uops (including the terminator)
    Uop u[];                 // flexible array
};

// c->stop values: why the dispatch chain was left.
enum { FXI_STOP_ERROR = 1, FXI_STOP_EXIT = 2, FXI_STOP_EC = 3, FXI_STOP_EXCEPTION = 4 };

// Open-addressed rip -> Block. Replaced (never freed) when it grows, so readers on other
// threads can keep using the table they loaded.
typedef struct BlockTable {
    uint64_t mask, count;
    Block *slot[];
} BlockTable;

// Interpreter instance: one guest program (ELF mode), or one Windows process whose threads
// each have their own FxiCpu (Windows mode, fxi_win.c).
struct Fxi {
    FxiCpu cpu;
    BlockTable *table;
    int windows;             // Windows mode: no image bounds, native ARM64EC code ends a run
    unsigned long long blocks, syscalls;
    fxi_result *out;
    // Guest image
    uint8_t *image; size_t image_size;
    uint8_t *stack; size_t stack_size;
    uint64_t brk;
};

// ---- Register file access (byte offsets) ----
#define REGP(c, off) ((uint8_t *)(c)->r + (off))
FXI_INLINE uint64_t rd8(FxiCpu *c, unsigned off) { return *REGP(c, off); }
FXI_INLINE uint64_t rd16(FxiCpu *c, unsigned off) { uint16_t v; memcpy(&v, REGP(c, off), 2); return v; }
FXI_INLINE uint64_t rd32(FxiCpu *c, unsigned off) { return (uint32_t)c->r[off >> 3]; }
FXI_INLINE uint64_t rd64(FxiCpu *c, unsigned off) { return c->r[off >> 3]; }
FXI_INLINE void wr8(FxiCpu *c, unsigned off, uint64_t v) { *REGP(c, off) = (uint8_t)v; }
FXI_INLINE void wr16(FxiCpu *c, unsigned off, uint64_t v) { uint16_t x = (uint16_t)v; memcpy(REGP(c, off), &x, 2); }
FXI_INLINE void wr32(FxiCpu *c, unsigned off, uint64_t v) { c->r[off >> 3] = (uint32_t)v; }  // zero-extends, as x86-64
FXI_INLINE void wr64(FxiCpu *c, unsigned off, uint64_t v) { c->r[off >> 3] = v; }

// ---- Memory (identity-mapped; unaligned allowed) ----
FXI_INLINE uint64_t ld8(uint64_t a) { return *(const uint8_t *)(uintptr_t)a; }
FXI_INLINE uint64_t ld16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
FXI_INLINE uint64_t ld32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
FXI_INLINE uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
FXI_INLINE void st8(uint64_t a, uint64_t v) { *(uint8_t *)(uintptr_t)a = (uint8_t)v; }
FXI_INLINE void st16(uint64_t a, uint64_t v) { uint16_t x = (uint16_t)v; memcpy((void *)(uintptr_t)a, &x, 2); }
FXI_INLINE void st32(uint64_t a, uint64_t v) { uint32_t x = (uint32_t)v; memcpy((void *)(uintptr_t)a, &x, 4); }
FXI_INLINE void st64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

// Branchless effective address: base/index are register slots (16 = zero). Every guest memory
// operand goes through here, so it also records the uop: a host fault on the access that
// follows is reported at that instruction (Windows mode, docs/NO_JIT_WINDOWS.md).
FXI_INLINE uint64_t fxi_ea(FxiCpu *c, const Uop *u) {
    c->cur = (Uop *)u;
    return c->r[u->base] + (c->r[u->index] << u->scale) + (uint64_t)u->disp;
}
#define FXI_TOUCH(c, u) ((c)->cur = (u))   // implicit memory operands (stack, strings)

// ---- Lazy flags ----
uint64_t fxi_flag_cf(FxiCpu *c);
uint64_t fxi_flag_of(FxiCpu *c);
uint64_t fxi_flag_pf(FxiCpu *c);
uint64_t fxi_flag_af(FxiCpu *c);
uint64_t fxi_flag_zf(FxiCpu *c);
uint64_t fxi_flag_sf(FxiCpu *c);
uint64_t fxi_rflags(FxiCpu *c);              // materialised RFLAGS
void fxi_set_rflags(FxiCpu *c, uint64_t f);  // explicit write (popf, sahf, stc...)
int fxi_cond(FxiCpu *c, unsigned cc);        // x86 condition code 0-15

// ---- Dispatch ----
#define FXI_NEXT(c, u) do { (void)(c); (void)(u); return; } while (0)   // FXR: back to the pinned caller
Block *fxr_lower(struct Fxi *vm, Block *b);     // FXR: pinned handlers for a translated block
Block *fxr_decode(struct Fxi *vm, uint64_t rip); // FXR: a decoded block (FXI uops, flag liveness), not lowered
void fxr_enter(FxiCpu *c, Block *b);            // FXR: run the pinned chain from b until it stops
#ifdef FXR_PROFILE
void fxr_profile_dump(struct Fxi *vm);          // FXR diagnostic build: the hottest blocks, uop by uop
#endif
void fxr_init_stop(Block *b);                   // FXR: the stop block's pinned handler
#define FXI_GOTO_BLOCK(c, b) do { Block *fxi_gb_ = (b); __attribute__((musttail)) return fxi_gb_->u[0].fn((c), fxi_gb_->u); } while (0)

extern Block *fxi_stop;                         // one "stop" uop: leaves the dispatch chain
Block *fxi_lookup(FxiCpu *c, uint64_t rip);     // translate on miss; fxi_stop on error
Block *fxi_translate(struct Fxi *vm, uint64_t rip);
int fxi_insn_length(const uint8_t *p, int *op_end);   // x86-64 length (0: invalid)
int fxi_probe(const uint8_t *p, uint64_t rip, char *why, size_t why_len);   // 1: FXI decodes it
// x87 (fxi_x87.c)
void fxi_x87_init(FxiCpu *c);                                   // fninit state
void fxi_x87_fxsave(FxiCpu *c, uint8_t *p, int with_xmm);       // fxsave / CONTEXT FltSave layout
void fxi_x87_fxrstor(FxiCpu *c, const uint8_t *p, int with_xmm);
double fxi_f80_load(const uint8_t *p);
void fxi_f80_store(uint8_t *p, double v);
void fxi_fail(FxiCpu *c, const char *fmt, ...); // set c->stop and the error text
// A CPU exception at rip (#BP, #UD, #DE, ...): a Windows exception in Windows mode
// (FXI_STOP_EXCEPTION, raised by the host), an error in ELF mode.
void fxi_raise(FxiCpu *c, uint64_t rip, uint32_t code, uint32_t flags, uint32_t nparams, uint64_t i0, uint64_t i1);
long fxi_syscall(FxiCpu *c);                    // Linux syscall in RAX/RDI/...; returns result
struct Fxi *fxi_vm_new(void);                   // empty block cache, stop block ready
// Windows mode (fxi_win.c): nonzero when rip is native ARM64EC code (PEB->EcCodeBitMap).
int fxi_win_is_ec(FxiCpu *c, uint64_t rip);
Block *fxi_win_exit_block(uint64_t rip);        // one uop: c->rip = rip, stop = FXI_STOP_EC

// Handler tables the decoder picks from (fxi_ops.c).
enum { F_RR, F_RI, F_RM, F_MR, F_MI, F_COUNT };           // operand forms
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP, ALU_TEST, ALU_COUNT };
extern const OpFn fxi_alu_tab[ALU_COUNT][F_COUNT][4][2];  // [op][form][size][flags?]
extern const OpFn fxi_mov_tab[F_COUNT][4];
enum { SH_ROL, SH_ROR, SH_RCL, SH_RCR, SH_SHL, SH_SHR, SH_SAL, SH_SAR };
extern const OpFn fxi_shift_tab[8][2][4];                 // [op][R/M][size]
enum { U_INC, U_DEC, U_NOT, U_NEG };
extern const OpFn fxi_unary_tab[4][2][4];                 // [op][R/M][size]
extern const OpFn fxi_ext_tab[2][2][3][4];                // movzx/movsx [signed][R/M][src 8/16/32][dst size]
extern const OpFn fxi_cmov_tab[2][4];                     // [R/M][size] (cc in u->cc)
extern const OpFn fxi_setcc_tab[2];                       // [R/M]
extern const OpFn fxi_lea_tab[4];
extern const OpFn fxi_muldiv_tab[4][2][4];                // [mul imul div idiv][R/M][size]
extern const OpFn fxi_imul2_tab[2][4];                    // imul r, r/m [R/M][size]
extern const OpFn fxi_imul3_tab[2][4];                    // imul r, r/m, imm
extern const OpFn fxi_fjcc_tab[2][2][2][16];              // fused cmp/test+jcc [cmp/test][RR/RI][32/64][cc]
OpFn fxi_named(const char *name);                         // misc handlers by name (decoder)

#endif
