// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: FXI with the guest's general registers pinned in host registers.
//
// FXI keeps the x86 register file in memory, so every instruction loads and stores it.
// Here the 16 guest GPRs, an effective-address temporary T and the four lazy-flag words
// are the arguments of every handler, and clang's preserve_none calling convention passes
// all of them in host registers (ARM64: 23 argument registers). Each hot instruction form
// has a handler specialised on its register operands, so `add rax, rbx` compiles to one
// ARM64 add plus the dispatch. Everything else runs FXI's own handler through p_slow,
// which spills the registers around the call: full coverage, FXI's exact semantics.
// No loop or idiom recognition: every specialisation is per instruction (or per
// compare-and-branch pair, which FXI's decoder fuses).
//
// Hot handlers never call a function: a call would give them a stack frame and spill
// pinned registers on the fast path. Rare cases tail-call an out-of-line handler instead:
// p_miss_* (first use of a block link, indirect-branch cache misses), p_jcc_slow (a
// condition on an unusual flag kind) and p_slow (redo the instruction with FXI's handler).
// Flags: the decoder's liveness pass, which also looks into the successor blocks, marks
// each uop with whether anything reads the flags after it (u->flive); dead flags are not
// recorded, and conditions are specialised on their condition code.

#include "fxr_pin.h"

static uint16_t k_cctab[16];   // cmov truth tables per condition code (pcond_tab)

#define DEF_MISS(NAME, SLOT, RIP)                                                          \
    PX NAME(FXR_PARAMS) {                                                                  \
        Block *b = fxi_lookup(c, (RIP));                                                   \
        if (b != fxi_stop) __atomic_store_n(&(SLOT), b->u, __ATOMIC_RELEASE);              \
        PGO(b->u);                                                                         \
    }
DEF_MISS(p_miss_t, u->ulink, u->imm)     // jump, call or taken branch: the target in imm
DEF_MISS(p_miss_f, u->ulink2, u->aux)    // a conditional branch's fallthrough (aux)
DEF_MISS(p_miss_g, u->ulink, u->aux)     // goto (a block split) and syscall: the next instruction
PX p_miss_ind(FXR_PARAMS) {
    Block *b = fxi_lookup(c, T);
    if (b != fxi_stop) { __typeof__(c->fxr_ibtc[0]) *e = IBTC(T); e->u = b->u; e->rip = T; }
    PGO(b->u);
}

// Leaving the chain (exit, error): the CPU state goes back to memory.
PH p_stop(FXR_PARAMS) { SPILL_R(); SPILL_F(); SPILL_X(); }
PH p_nop(FXR_PARAMS) { PNEXT(); }
PH p_jmp(FXR_PARAMS) { PCHAIN(u->ulink, p_miss_t); }
PH p_goto(FXR_PARAMS) { PCHAIN(u->ulink, p_miss_g); }
PH p_call(FXR_PARAMS) { g4 -= 8; st64(g4, u->aux); PCHAIN(u->ulink, p_miss_t); }
PH p_ret(FXR_PARAMS) { uint64_t t = ld64(g4); g4 += 8 + u->aux; PIND(t); }
PH p_call_T(FXR_PARAMS) { uint64_t t = ld64(T); g4 -= 8; st64(g4, u->aux); PIND(t); }
PH p_jmp_T(FXR_PARAMS) { PIND(ld64(T)); }
// An fs/gs operand: the segment base lives in memory, so the address is computed FXI's way.
PH p_call_M(FXR_PARAMS) { SPILL_R(); uint64_t t = ld64(fxi_ea(c, u)); g4 -= 8; st64(g4, u->aux); PIND(t); }
PH p_jmp_M(FXR_PARAMS) { SPILL_R(); PIND(ld64(fxi_ea(c, u))); }
// Conditional branch on the lazy flags, one handler per condition code.
PX p_jcc_slow(FXR_PARAMS) {
    SPILL_F();
    if (fxi_cond(c, u->cc)) PCHAIN(u->ulink, p_miss_t);
    PCHAIN(u->ulink2, p_miss_f);
}
#define DEF_JCC(CC, _)                                                                     \
    PH p_jcc_##CC(FXR_PARAMS) {                                                            \
        int s_ = 0, t_ = pcond(CC, F0, F1, F2, F3, &s_);                                   \
        if (FXI_UNLIKELY(s_)) PTAIL(p_jcc_slow);                                           \
        if (t_) PCHAIN(u->ulink, p_miss_t);                                                \
        PCHAIN(u->ulink2, p_miss_f);                                                       \
    }
C16(DEF_JCC, _)
PH p_syscall(FXR_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X();
    c->rip = u->aux;
    long r = fxi_syscall(c);
    if (c->stop) return;
    RELOAD_R();
    g0 = (uint64_t)r;
    g1 = u->aux;                  // syscall clobbers RCX (return rip) and R11 (rflags)
    g11 = fxi_rflags(c);
    PCHAIN(u->ulink, p_miss_g);
}
// Anything without a pinned form: FXI's handler, with the registers in memory around it.
PX p_slow(FXR_PARAMS) {
    SPILL_R(); SPILL_F(); SPILL_X();
    u->fn(c, u);
    if (FXI_UNLIKELY(c->stop)) return;
    RELOAD_R(); RELOAD_F(); RELOAD_X();
    PNEXT();
}

#define DEF_PUSHPOP(R, _)                                                                  \
    PH p_push_##R(FXR_PARAMS) { uint64_t v = g##R; g4 -= 8; st64(g4, v); PNEXT(); }        \
    PH p_pop_##R(FXR_PARAMS) { uint64_t v = ld64(g4); g4 += 8; g##R = v; PNEXT(); }       \
    PH p_call_R_##R(FXR_PARAMS) { uint64_t t = g##R; g4 -= 8; st64(g4, u->aux); PIND(t); } \
    PH p_jmp_R_##R(FXR_PARAMS) { PIND(g##R); }
R16(DEF_PUSHPOP, _)
static const PFn t_push[16] = { R16(E1, push) }, t_pop[16] = { R16(E1, pop) };
static const PFn t_call_R[16] = { R16(E1, call_R) }, t_jmp_R[16] = { R16(E1, jmp_R) };
static const PFn t_jcc[16] = { C16(E1, jcc) };
// setcc, specialised on cc and register
#define DEF_SETCC(R, CC)                                                                   \
    PH p_setcc_##CC##_##R(FXR_PARAMS) {                                                    \
        int s_ = 0, t_ = pcond(CC, F0, F1, F2, F3, &s_);                                   \
        if (FXI_UNLIKELY(s_)) PTAIL(p_slow);                                               \
        g##R = (g##R & ~0xffull) | (uint64_t)t_; PNEXT();                                  \
    }
#define DEF_SETCC_CC(CC, _) R16(DEF_SETCC, CC)
C16(DEF_SETCC_CC, _)
#define E_SETCC(R, CC) p_setcc_##CC##_##R,
#define ROW_SETCC(CC, _) { R16(E_SETCC, CC) },
const PFn t_setcc[16][16] = { C16(ROW_SETCC, _) };   // [cc][reg]
// ===========================================================================
// Lowering: FXI's decoded uops -> pinned handlers
// ===========================================================================
enum { FAM_NONE, FAM_ALU, FAM_MOV, FAM_LEA, FAM_SHIFT, FAM_UNARY, FAM_EXT, FAM_CMOV, FAM_SETCC, FAM_IMUL2,
       FAM_IMUL3, FAM_FJCC, FAM_NAMED, FAM_X, FAM_XI, FAM_XS };
enum { N_JMP, N_JCC, N_CALL, N_CALL_R, N_CALL_M, N_JMP_R, N_JMP_M, N_RET, N_GOTO, N_SYSCALL, N_STOP, N_PUSH_R,
       N_POP_R, N_NOP };
// FAM_XS: SSE loads/stores and the ops between general registers and XMM
enum { XS_MOVX_MR, XS_MOVSS_RM, XS_MOVSD_RM, XS_MOVSS_MR, XS_MOVSD_MR, XS_MOVLPS_RM, XS_MOVHPS_RM, XS_MOVHPS_MR,
       XS_XG = 16, XS_XGM = 32, XS_GX = 48 };
typedef struct { OpFn key; uint8_t fam, a, b, c2, d; } Desc;
#define DESC_SLOTS 16384
static Desc g_desc[DESC_SLOTS];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint64_t hptr(OpFn f) { return ((uint64_t)(uintptr_t)f * 0x9E3779B97F4A7C15ull) >> 50; }
static void put(OpFn f, uint8_t fam, uint8_t a, uint8_t b, uint8_t c2, uint8_t d) {
    if (!f) return;
    uint64_t h = hptr(f) & (DESC_SLOTS - 1);
    while (g_desc[h].key && g_desc[h].key != f) h = (h + 1) & (DESC_SLOTS - 1);
    g_desc[h] = (Desc){ f, fam, a, b, c2, d };
}
static const Desc *find(OpFn f) {
    uint64_t h = hptr(f) & (DESC_SLOTS - 1);
    while (g_desc[h].key) { if (g_desc[h].key == f) return &g_desc[h]; h = (h + 1) & (DESC_SLOTS - 1); }
    return 0;
}
static int is_shuffle(const char *n) {
    return !strcmp(n, "pshufd") || !strcmp(n, "pshuflw") || !strcmp(n, "pshufhw") || !strcmp(n, "shufps") ||
           !strcmp(n, "shufpd") || !strcmp(n, "psrldq") || !strcmp(n, "pslldq");
}
static void init_desc(void) {
    for (int cc = 0; cc < 16; cc++)
        for (int i = 0; i < 16; i++)
            k_cctab[cc] |= (uint16_t)(cc_flags((unsigned)cc, (i >> 3) & 1, (i >> 1) & 1, i & 1, (i >> 2) & 1, 0) << i);
    for (int op = 0; op < ALU_COUNT; op++)
        for (int fm = 0; fm < F_COUNT; fm++)
            for (int si = 0; si < 4; si++)
                for (int fl = 0; fl < 2; fl++) put(fxi_alu_tab[op][fm][si][fl], FAM_ALU, op, fm, si, 0);
    for (int fm = 0; fm < F_COUNT; fm++) for (int si = 0; si < 4; si++) put(fxi_mov_tab[fm][si], FAM_MOV, fm, si, 0, 0);
    for (int si = 1; si < 4; si++) put(fxi_lea_tab[si], FAM_LEA, si, 0, 0, 0);
    for (int op = 0; op < 8; op++) for (int rm = 0; rm < 2; rm++) for (int si = 0; si < 4; si++)
        put(fxi_shift_tab[op][rm][si], FAM_SHIFT, op, rm, si, 0);
    for (int op = 0; op < 4; op++) for (int rm = 0; rm < 2; rm++) for (int si = 0; si < 4; si++)
        put(fxi_unary_tab[op][rm][si], FAM_UNARY, op, rm, si, 0);
    for (int sg = 0; sg < 2; sg++) for (int rm = 0; rm < 2; rm++) for (int ss = 0; ss < 3; ss++) for (int ds = 1; ds < 4; ds++)
        put(fxi_ext_tab[sg][rm][ss][ds], FAM_EXT, sg, rm, ss, ds);
    for (int rm = 0; rm < 2; rm++) for (int si = 1; si < 4; si++) {
        put(fxi_cmov_tab[rm][si], FAM_CMOV, rm, si, 0, 0);
        put(fxi_imul2_tab[rm][si], FAM_IMUL2, rm, si, 0, 0);
        put(fxi_imul3_tab[rm][si], FAM_IMUL3, rm, si, 0, 0);
    }
    put(fxi_setcc_tab[0], FAM_SETCC, 0, 0, 0, 0);
    for (int t = 0; t < 2; t++) for (int ri = 0; ri < 2; ri++) for (int s = 0; s < 2; s++) for (int cc = 0; cc < 16; cc++)
        put(fxi_fjcc_tab[t][ri][s][cc], FAM_FJCC, t, ri, s, cc);
    static const struct { const char *n; int id; } named[] = {
        { "jmp", N_JMP }, { "jcc", N_JCC }, { "call", N_CALL }, { "call_R", N_CALL_R }, { "call_M", N_CALL_M },
        { "jmp_R", N_JMP_R }, { "jmp_M", N_JMP_M }, { "ret", N_RET }, { "goto", N_GOTO }, { "syscall", N_SYSCALL },
        { "stop", N_STOP }, { "push_R", N_PUSH_R }, { "pop_R", N_POP_R }, { "nop", N_NOP },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) put(fxi_named(named[i].n), FAM_NAMED, (uint8_t)named[i].id, 0, 0, 0);
    // SSE: two-operand ops (register form; memory form when the op has one), c2 = shuffle, d = comis
    char nm[48];
    for (size_t i = 0; i < fxr_n_xops; i++) {
        const char *n = fxr_xops[i].name;
        int shuf = is_shuffle(n), com = !strcmp(n, "comiss") || !strcmp(n, "comisd");
        snprintf(nm, sizeof nm, "%s_RR", n); put(fxi_named(nm), FAM_X, (uint8_t)i, 0, (uint8_t)shuf, (uint8_t)com);
        if (fxr_xops[i].rt[0]) { snprintf(nm, sizeof nm, "%s_RM", n); put(fxi_named(nm), FAM_X, (uint8_t)i, 1, (uint8_t)shuf, (uint8_t)com); }
    }
    for (size_t i = 0; i < fxr_n_xshift; i++) {
        snprintf(nm, sizeof nm, "%s_RI", fxr_xshift[i].name);
        put(fxi_named(nm), FAM_XI, (uint8_t)i, 0, (uint8_t)is_shuffle(fxr_xshift[i].name), 0);
    }
    static const struct { const char *n; int id; } xnamed[] = {
        { "movx_MR", XS_MOVX_MR }, { "movss_RM", XS_MOVSS_RM }, { "movsd_RM", XS_MOVSD_RM }, { "movss_MR", XS_MOVSS_MR },
        { "movsd_MR", XS_MOVSD_MR }, { "movlps_RM", XS_MOVLPS_RM }, { "movhps_RM", XS_MOVHPS_RM }, { "movhps_MR", XS_MOVHPS_MR },
        { "cvtsi2ss_R32", XS_XG + XG_SI2SS32 }, { "cvtsi2ss_R64", XS_XG + XG_SI2SS64 },
        { "cvtsi2sd_R32", XS_XG + XG_SI2SD32 }, { "cvtsi2sd_R64", XS_XG + XG_SI2SD64 },
        { "movd_XR32", XS_XG + XG_MOVD32 }, { "movd_XR64", XS_XG + XG_MOVD64 },
        { "cvtsi2ss_M32", XS_XGM + 0 }, { "cvtsi2ss_M64", XS_XGM + 1 }, { "cvtsi2sd_M32", XS_XGM + 2 }, { "cvtsi2sd_M64", XS_XGM + 3 },
        { "cvttss2si_R32", XS_GX + GX_TSS32 }, { "cvtss2si_R32", XS_GX + GX_SS32 },
        { "cvttsd2si_R32", XS_GX + GX_TSD32 }, { "cvtsd2si_R32", XS_GX + GX_SD32 },
        { "cvttss2si_R64", XS_GX + GX_TSS64 }, { "cvtss2si_R64", XS_GX + GX_SS64 },
        { "cvttsd2si_R64", XS_GX + GX_TSD64 }, { "cvtsd2si_R64", XS_GX + GX_SD64 },
        { "movd_RX32", XS_GX + GX_MOVD32 }, { "movd_RX64", XS_GX + GX_MOVD64 },
        { "pmovmskb_RR", XS_GX + GX_PMOVMSKB }, { "movmskps_RR", XS_GX + GX_MOVMSKPS }, { "movmskpd_RR", XS_GX + GX_MOVMSKPD },
    };
    for (size_t i = 0; i < sizeof xnamed / sizeof xnamed[0]; i++) put(fxi_named(xnamed[i].n), FAM_XS, (uint8_t)xnamed[i].id, 0, 0, 0);
}

static int greg(unsigned off) { return (off & 7) == 0 && off < 128 ? (int)(off >> 3) : -1; }   // pinnable GPR
static int simple_mem(const Uop *u) { return u->index == R_ZERO && u->base <= R_ZERO; }        // [base + disp]
static int ea_ok(const Uop *u) { return u->base <= R_ZERO && u->index <= R_ZERO; }

typedef struct { Uop *out; int n; } Out;
static Uop *put_uop(Out *o, const Uop *src, PFn p) { Uop *x = &o->out[o->n++]; *x = *src; x->p = p; return x; }
// T = EA of src, then the op itself
static Uop *with_ea(Out *o, const Uop *src, PFn p) {
    put_uop(o, src, t_ea[src->base][src->index]);
    return put_uop(o, src, p);
}

static void lower_one(Out *o, const Uop *u) {
    const Desc *d = find(u->fn);
    int D = greg(u->dst), S = greg(u->src), fl = u->flive != 0;
    if (d) switch (d->fam) {
    case FAM_ALU: {
        int op = d->a, fm = d->b, si = d->c2;
        // cmp/test of registers whose flags nothing reads: no effect at all
        if (!fl && (op == ALU_CMP || op == ALU_TEST) && (fm == F_RR || fm == F_RI)) return;
        if (fm == F_RR && si >= 2 && D >= 0 && S >= 0) { put_uop(o, u, t_alu_rr[fl][op][si - 2][D][S]); return; }
        if (fm == F_RI && si >= 2 && D >= 0) { put_uop(o, u, t_alu_ri[fl][op][si - 2][D]); return; }
        if (fm == F_RM && si >= 2 && D >= 0 && ea_ok(u)) { with_ea(o, u, t_alu_rt[fl][op][si - 2][D]); return; }
        if (fm == F_MR && S >= 0 && ea_ok(u)) { with_ea(o, u, t_alu_tr[fl][op][si][S]); return; }
        if (fm == F_MI && ea_ok(u)) { with_ea(o, u, t_alu_ti[fl][op][si]); return; }
        break;
    }
    case FAM_MOV: {
        int fm = d->a, si = d->b;
        if (fm == F_RR && si >= 2 && D >= 0 && S >= 0) { put_uop(o, u, t_mov_rr[si - 2][D][S]); return; }
        if (fm == F_RI && si >= 2 && D >= 0) { put_uop(o, u, t_mov_ri[si - 2][D]); return; }
        if (fm == F_RM && si >= 2 && D >= 0) {
            if (simple_mem(u)) { put_uop(o, u, t_ld[si - 2][u->base][D]); return; }
            if (ea_ok(u)) { put_uop(o, u, t_ldi[si == 3 ? FL_LD64 : FL_LD32][u->base][u->index][D]); return; }
        }
        if (fm == F_MR && S >= 0) {
            if (simple_mem(u)) { put_uop(o, u, t_st[si][u->base][S]); return; }
            if (ea_ok(u) && si != 1) { put_uop(o, u, t_ldi[si == 0 ? FL_ST8 : si == 2 ? FL_ST32 : FL_ST64][u->base][u->index][S]); return; }
            if (ea_ok(u)) { with_ea(o, u, t_stt[si][S]); return; }
        }
        if (fm == F_MI) {
            if (simple_mem(u)) { put_uop(o, u, t_sti[si][u->base]); return; }
            if (ea_ok(u)) { put_uop(o, u, t_sti_bi[si][u->base][u->index]); return; }
        }
        break;
    }
    case FAM_LEA: {
        int si = d->a;
        if (si >= 2 && D >= 0) {
            if (simple_mem(u)) { put_uop(o, u, t_lea[si - 2][u->base][D]); return; }
            if (ea_ok(u)) { with_ea(o, u, t_movt[si - 2][D]); return; }
        }
        break;
    }
    case FAM_EXT: {
        int sg = d->a, rm = d->b, ss = d->c2, ds = d->d, k = -1;
        if (ds < 2) break;
        if (!sg) k = ss == 0 ? KZ8 : ss == 1 ? KZ16 : -1;
        else if (ss == 0) k = ds == 2 ? KS8_32 : KS8_64;
        else if (ss == 1) k = ds == 2 ? KS16_32 : KS16_64;
        else if (ds == 3) k = KS32_64;
        if (k < 0 || D < 0) break;
        if (!rm && S >= 0) { put_uop(o, u, t_ext[k][D][S]); return; }
        if (rm && k == KZ8 && simple_mem(u)) { put_uop(o, u, t_ldz8s[u->base][D]); return; }
        if (rm && k == KZ8 && ea_ok(u)) { put_uop(o, u, t_ldi[FL_LDZ8][u->base][u->index][D]); return; }
        if (rm && ea_ok(u)) { with_ea(o, u, t_ldt[k][D]); return; }
        break;
    }
    case FAM_SHIFT: {
        int op = d->a, rm = d->b, si = d->c2, ps = -1;
        if (rm || si < 2 || D < 0) break;
        if (op == SH_SHL || op == SH_SAL) ps = PS_SHL;
        else if (op == SH_SHR) ps = PS_SHR;
        else if (op == SH_SAR) ps = PS_SAR;
        else if (op == SH_ROL && !u->cc) ps = PS_ROL;
        else if (op == SH_ROR && !u->cc) ps = PS_ROR;
        if (ps < 0) break;
        if (u->src == 0xffff) { put_uop(o, u, t_sh[fl][ps][si - 2][1][D]); return; }
        unsigned n = (unsigned)u->imm & (si == 3 ? 63u : 31u);
        if (!n) { put_uop(o, u, si == 2 ? t_mov_rr[0][D][D] : p_nop); return; }   // flags unchanged; a 32-bit register still zero-extends
        put_uop(o, u, t_sh[fl][ps][si - 2][0][D])->imm = n;
        return;
    }
    case FAM_UNARY:
        if (!d->b && d->c2 >= 2 && D >= 0) { put_uop(o, u, t_un[fl][d->c2 - 2][d->a][D]); return; }
        break;
    case FAM_IMUL2: case FAM_IMUL3: {
        int rm = d->a, si = d->b;
        if (si < 2 || D < 0) break;
        const PFn (*rr)[16][16] = d->fam == FAM_IMUL2 ? t_imul2[fl] : t_imul3[fl];
        const PFn (*rt)[16] = d->fam == FAM_IMUL2 ? t_imul2t[fl] : t_imul3t[fl];
        if (!rm && S >= 0) { put_uop(o, u, rr[si - 2][D][S]); return; }
        if (rm && ea_ok(u)) { with_ea(o, u, rt[si - 2][D]); return; }
        break;
    }
    case FAM_CMOV: {
        int rm = d->a, si = d->b;
        if (si < 2 || D < 0 || u->cc == 10 || u->cc == 11) break;   // parity: FXI's handler
        Uop *x = 0;
        if (!rm && S >= 0) x = put_uop(o, u, t_cmov[si - 2][D][S]);
        else if (rm && ea_ok(u)) x = with_ea(o, u, t_cmovt[si - 2][D]);
        if (!x) break;
        x->imm = k_cctab[u->cc & 15];
        return;
    }
    case FAM_SETCC:
        if (D >= 0) { put_uop(o, u, t_setcc[u->cc & 15][D]); return; }
        break;
    case FAM_FJCC: {
        int t = d->a, ri = d->b, s64 = d->c2, cc = d->d;
        if (D < 0 || (!ri && S < 0)) { fprintf(stderr, "fxr: fused branch without register operands\n"); abort(); }
        Uop *x;
        if (ri) x = put_uop(o, u, t ? t_fjt_ri[s64][cc][D] : t_fjc_ri[s64][cc][D]);
        else if (!t) x = put_uop(o, u, t_fjc_rr[s64][cc][D][S]);
        else x = put_uop(o, u, D == S ? t_fjt_rr[s64][cc][D] : t_fjt_rrx[s64][D][S]);
        x->cc = (uint8_t)cc;
        return;
    }
    case FAM_NAMED:
        switch (d->a) {
        case N_JMP: put_uop(o, u, p_jmp); return;
        case N_JCC: put_uop(o, u, t_jcc[u->cc & 15]); return;
        case N_CALL: put_uop(o, u, p_call); return;
        case N_CALL_R: if (S >= 0) { put_uop(o, u, t_call_R[S]); return; } break;
        case N_JMP_R: if (S >= 0) { put_uop(o, u, t_jmp_R[S]); return; } break;
        case N_CALL_M: if (ea_ok(u)) with_ea(o, u, p_call_T); else put_uop(o, u, p_call_M); return;
        case N_JMP_M: if (ea_ok(u)) with_ea(o, u, p_jmp_T); else put_uop(o, u, p_jmp_M); return;
        case N_RET: put_uop(o, u, p_ret); return;
        case N_GOTO: put_uop(o, u, p_goto); return;
        case N_SYSCALL: put_uop(o, u, p_syscall); return;
        case N_STOP: put_uop(o, u, p_stop); return;
        case N_NOP: put_uop(o, u, p_nop); return;
        case N_PUSH_R: if (S >= 0) { put_uop(o, u, t_push[S]); return; } break;
        case N_POP_R: if (D >= 0) { put_uop(o, u, t_pop[D]); return; } break;
        }
        // Control flow has no FXI fallback here (FXI's handlers would continue FXI's own chain).
        if (d->a <= N_STOP) { fprintf(stderr, "fxr: no pinned form for control-flow uop %d\n", d->a); abort(); }
        break;
    case FAM_X: {   // two-operand SSE: dst XMM, src XMM or memory
        const XOp *x = &fxr_xops[d->a];
        int dc = xcls(u->dst);
        Uop *y;
        if (!d->b) {
            if (d->d && !fl) return;   // (u)comis of registers whose flags nothing reads
            y = put_uop(o, u, x->rr[dc][xcls(u->src)]);
        } else {
            if (!ea_ok(u)) break;      // fs/gs operand: FXI's handler
            if (!strcmp(x->name, "movx")) { put_uop(o, u, t_xl_movx[dc][u->base][u->index]); return; }
            y = with_ea(o, u, x->rt[dc]);
        }
        if (d->c2) { uint8_t ix[16]; fxr_xshuffle_index(x->name, (unsigned)u->imm, ix); memcpy(y->xidx, ix, 16); }
        return;
    }
    case FAM_XI: {   // shift by an immediate
        Uop *y = put_uop(o, u, fxr_xshift[d->a].h[xcls(u->dst)]);
        if (d->c2) { uint8_t ix[16]; fxr_xshuffle_index(fxr_xshift[d->a].name, (unsigned)u->imm, ix); memcpy(y->xidx, ix, 16); }
        return;
    }
    case FAM_XS: {   // loads, stores, general register <-> XMM
        int id = d->a, dc = xcls(u->dst), sc = xcls(u->src);
        if (id >= XS_GX) {   // GPR <- XMM: dst is a GPR byte offset, src an XMM
            if (D < 0) break;
            put_uop(o, u, t_gx[id - XS_GX][sc][D]);
            return;
        }
        if (id >= XS_XGM) { if (!ea_ok(u)) break; with_ea(o, u, t_xgt[id - XS_XGM][dc]); return; }
        if (id >= XS_XG) { if (S < 0) break; put_uop(o, u, t_xg[id - XS_XG][dc][S]); return; }
        if (!ea_ok(u)) break;
        switch (id) {
        case XS_MOVX_MR: put_uop(o, u, t_xs_movx[sc][u->base][u->index]); return;
        case XS_MOVSS_RM: if (simple_mem(u)) put_uop(o, u, t_xl_movss[dc][u->base]); else with_ea(o, u, t_xlt_movss[dc]); return;
        case XS_MOVSD_RM: if (simple_mem(u)) put_uop(o, u, t_xl_movsd[dc][u->base]); else with_ea(o, u, t_xlt_movsd[dc]); return;
        case XS_MOVSS_MR: if (simple_mem(u)) put_uop(o, u, t_xs_movss[sc][u->base]); else with_ea(o, u, t_xst_movss[sc]); return;
        case XS_MOVSD_MR: if (simple_mem(u)) put_uop(o, u, t_xs_movsd[sc][u->base]); else with_ea(o, u, t_xst_movsd[sc]); return;
        case XS_MOVLPS_RM: with_ea(o, u, t_xlt_movlps[dc]); return;
        case XS_MOVHPS_RM: with_ea(o, u, t_xlt_movhps[dc]); return;
        case XS_MOVHPS_MR: with_ea(o, u, t_xst_movhps[sc]); return;
        }
        break;
    }
    }
    put_uop(o, u, p_slow);
}

// Superinstructions (fxr_pin_fuse.c): u and the uop after it as one uop. Returns 2 when fused.
// The fused uop starts as a copy of the second uop (a branch keeps its targets and links).
static int is_named(const Desc *d, int id) { return d && d->fam == FAM_NAMED && d->a == id; }
static int try_fuse(Out *o, const Uop *u, uint32_t left) {
    if (left < 2) return 0;
    const Uop *v = u + 1;
    const Desc *a = find(u->fn), *b = find(v->fn);
    if (!a || !b) return 0;
    int D = greg(u->dst), S = greg(u->src);
    Uop *x = 0;
    if (is_named(b, N_JCC)) {   // compare-and-branch forms FXI does not fuse, ALU + jcc
        unsigned cc = v->cc & 15;
        if (a->fam == FAM_ALU && (a->a == ALU_CMP || a->a == ALU_TEST)) {
            int t = a->a == ALU_TEST, fm = a->b, si = a->c2;
            if (fm == F_MI && ea_ok(u)) {
                if (simple_mem(u)) { x = put_uop(o, v, t_fmi[t][si][cc][u->base]); x->disp = u->disp; }
                else { put_uop(o, u, t_ea[u->base][u->index]); x = put_uop(o, v, t_fmit[t][si][cc]); }
                x->fimm = (int32_t)u->imm;
                return 2;
            }
            if (fm == F_MR && S >= 0 && ea_ok(u)) {   // cmp/test [mem], reg
                put_uop(o, u, t_ea[u->base][u->index]);
                put_uop(o, v, t_fmr[t ? 2 : 0][si][cc][S]);
                return 2;
            }
            if (fm == F_RM && !t && D >= 0 && ea_ok(u)) {   // cmp reg, [mem]
                put_uop(o, u, t_ea[u->base][u->index]);
                put_uop(o, v, t_fmr[1][si][cc][D]);
                return 2;
            }
            if (fm == F_RI && si < 2 && D >= 0) {   // 8/16-bit register
                x = put_uop(o, v, t_fjs_ri[t][si][cc][D]);
                x->disp = (int64_t)u->imm;
                return 2;
            }
            if (fm == F_RR && si < 2 && D >= 0 && S >= 0) {
                x = put_uop(o, v, t && D == S ? t_fjs_rr[0][si][cc][D] : t_fjs_rrx[t][si][D][S]);
                x->dst = u->dst; x->src = u->src;
                return 2;
            }
            return 0;
        }
        if (a->fam == FAM_X && a->d) {   // (u)comiss / (u)comisd
            int dbl = !strcmp(fxr_xops[a->a].name, "comisd");
            if (!a->b) x = put_uop(o, v, t_fcj[dbl][cc][xcls(u->dst)][xcls(u->src)]);
            else if (ea_ok(u)) { put_uop(o, u, t_ea[u->base][u->index]); x = put_uop(o, v, t_fcjt[dbl][cc][xcls(u->dst)]); }
            else return 0;
            x->dst = u->dst; x->src = u->src;
            return 2;
        }
        if (a->fam == FAM_ALU && a->b == F_RI && a->c2 >= 2 && D >= 0) {   // add/sub/and/or/xor reg, imm
            int k = a->a == ALU_ADD ? 0 : a->a == ALU_SUB ? 1 : a->a == ALU_AND ? 2 : a->a == ALU_OR ? 3 : a->a == ALU_XOR ? 4 : -1;
            if (k < 0) return 0;
            x = put_uop(o, v, t_fai[k][a->c2 - 2][cc][D]);
            x->disp = (int64_t)u->imm; x->dst = u->dst;
            return 2;
        }
        if (a->fam == FAM_UNARY && !a->b && a->c2 >= 2 && D >= 0 && (a->a == U_INC || a->a == U_DEC) && !v->flive) {
            int ci = cc == 4 ? 0 : cc == 5 ? 1 : cc == 8 ? 2 : cc == 9 ? 3 : -1;
            if (ci < 0) return 0;
            x = put_uop(o, v, t_fid[a->a == U_INC][a->c2 - 2][ci][D]);
            x->dst = u->dst;
            return 2;
        }
        return 0;
    }
    // mov D, [base + disp] + FXI's fused test D, D / jcc
    if (a->fam == FAM_MOV && a->a == F_RM && a->b >= 2 && D >= 0 && simple_mem(u) && b->fam == FAM_FJCC &&
        b->a == 1 && !b->b && b->c2 == (a->b == 3) && greg(v->dst) == D && greg(v->src) == D) {
        unsigned cc = b->d;
        int ci = cc == 4 ? 0 : cc == 5 ? 1 : cc == 8 ? 2 : cc == 9 ? 3 : cc == 14 ? 4 : cc == 15 ? 5 : -1;
        if (ci < 0) return 0;
        x = put_uop(o, v, t_ltj[a->b == 3][ci][u->base][D]);
        x->disp = u->disp;
        return 2;
    }
    if (is_named(b, N_CALL)) {   // argument setup + direct call
        if (a->fam == FAM_MOV && a->a == F_RR && a->b >= 2 && D >= 0 && S >= 0) x = put_uop(o, v, t_amr[a->b - 2][D][S]);
        else if (a->fam == FAM_LEA && a->a == 3 && D >= 0 && simple_mem(u)) { x = put_uop(o, v, t_alea[u->base][D]); x->disp = u->disp; }
        else if (a->fam == FAM_MOV && a->a == F_RI && a->b >= 2 && D >= 0 &&
                 (a->b == 2 || (int64_t)(int32_t)u->imm == (int64_t)u->imm)) { x = put_uop(o, v, t_ami[a->b - 2][D]); x->fimm = (int32_t)u->imm; }
        else if (a->fam == FAM_ALU && a->a == ALU_XOR && a->b == F_RR && a->c2 == 2 && D >= 0 && D == S && !u->flive)
            x = put_uop(o, v, t_axz[D]);
        return x ? 2 : 0;
    }
    if (is_named(a, N_POP_R) && D >= 0 && D != R_SP) {
        int D2 = greg(v->dst);
        if (is_named(b, N_POP_R) && D2 >= 0 && D2 != R_SP) { put_uop(o, u, t_pop2[D][D2]); return 2; }
        if (is_named(b, N_RET)) { x = put_uop(o, v, t_popret[D]); x->dst = u->dst; return 2; }
        return 0;
    }
    if (is_named(a, N_PUSH_R) && is_named(b, N_PUSH_R) && S >= 0 && S != R_SP) {
        int S2 = greg(v->src);
        if (S2 >= 0 && S2 != R_SP) { put_uop(o, u, t_push2[S][S2]); return 2; }
    }
    return 0;
}

Block *fxr_lower(struct Fxi *vm, Block *b) {
    (void)vm;
    pthread_once(&g_once, init_desc);
    Out o = { malloc(sizeof(Uop) * (2 * (size_t)b->n + 1)), 0 };
    for (uint32_t i = 0; i < b->n; i++) {
        if (try_fuse(&o, &b->u[i], b->n - i)) { i++; continue; }
        lower_one(&o, &b->u[i]);
    }
    Block *nb = malloc(sizeof(Block) + sizeof(Uop) * (size_t)o.n);
    nb->rip = b->rip;
    nb->n = (uint32_t)o.n;
    memcpy(nb->u, o.out, sizeof(Uop) * (size_t)o.n);
    free(o.out);
    free(b);
    return nb;
}

void fxr_init_stop(Block *b) { b->u[0].p = p_stop; }

void fxr_enter(FxiCpu *c, Block *b) {
    if (!c->fxr_ready) {
        for (unsigned i = 0; i <= IBTC_MASK; i++) { c->fxr_ibtc[i].rip = ~0ull; c->fxr_ibtc[i].u = 0; }
        c->fxr_ready = 1;
    }
    uint64_t g0 = c->r[0], g1 = c->r[1], g2 = c->r[2], g3 = c->r[3], g4 = c->r[4], g5 = c->r[5], g6 = c->r[6],
             g7 = c->r[7], g8 = c->r[8], g9 = c->r[9], g10 = c->r[10], g11 = c->r[11], g12 = c->r[12], g13 = c->r[13],
             g14 = c->r[14], g15 = c->r[15], T = 0, F0, F1, F2, F3;
    FxrV x0, x1, x2, x3, x4, x5, x6, x7;
    RELOAD_F();
    RELOAD_X();
    b->u[0].p(c, b->u, FXR_ARGS);
}
