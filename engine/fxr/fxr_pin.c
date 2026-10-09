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

// Windows mode: the state goes to the CPU structure first, so a fault while translating the
// target (fetching its code) has the exact x64 state (fxr_win_host_state).
#define WIN_SPILL() do { if (FXI_UNLIKELY(c->vm->windows)) { SPILL_R(); SPILL_F(); SPILL_X(); } } while (0)
#define DEF_MISS(NAME, SLOT, RIP)                                                          \
    PX NAME(FXR_PARAMS) {                                                                  \
        WIN_SPILL();                                                                       \
        Block *b = fxi_lookup(c, (RIP));                                                   \
        if (b != fxi_stop) __atomic_store_n(&(SLOT), b->u, __ATOMIC_RELEASE);              \
        PGO(b->u);                                                                         \
    }
DEF_MISS(p_miss_t, u->ulink, u->imm)     // jump, call or taken branch: the target in imm
DEF_MISS(p_miss_f, u->ulink2, u->aux)    // a conditional branch's fallthrough (aux)
DEF_MISS(p_miss_g, u->ulink, u->aux)     // goto (a block split) and syscall: the next instruction
PX p_miss_ind(FXR_PARAMS) {
    WIN_SPILL();
    Block *b = fxi_lookup(c, T);
    if (b != fxi_stop) { __typeof__(c->fxr_ibtc[0]) *e = IBTC(T); e->u = b->u; e->rip = T; }
    PGO(b->u);
}

// Leaving the chain (exit, error): the CPU state goes back to memory.
PH p_stop(FXR_PARAMS) { SPILL_R(); SPILL_F(); SPILL_X(); }
PH p_nop(FXR_PARAMS) { PNEXT(); }
PH p_jmp(FXR_PARAMS) { PCHAIN_T(u->ulink, p_miss_t); }
PH p_goto(FXR_PARAMS) { PCHAIN_T(u->ulink, p_miss_g); }
PH p_call(FXR_PARAMS) { g4 -= 8; st64(g4, u->aux); PCHAIN(u->ulink, p_miss_t); }
PH p_ret(FXR_PARAMS) { uint64_t t = ld64(g4); g4 += 8 + u->aux; PIND(t); }
PH p_call_T(FXR_PARAMS) { uint64_t t = ld64(T); g4 -= 8; st64(g4, u->aux); PIND(t); }
PH p_jmp_T(FXR_PARAMS) { PIND(ld64(T)); }
// An fs/gs operand: the segment base lives in memory, so the address is computed FXI's way.
// fxi_ea is a call: these two run outside the pinned section, with the whole state in the CPU
// structure and c->cur set first, so a fault in them is exact (fxr_win_host_state).
#define PO static FXR_CC void
#define CPU_STATE() do { SPILL_R(); SPILL_F(); SPILL_X(); c->cur = u; } while (0)
PO p_call_M(FXR_PARAMS) { CPU_STATE(); uint64_t t = ld64(fxi_ea(c, u)); st64(g4 - 8, u->aux); g4 -= 8; PIND(t); }
PO p_jmp_M(FXR_PARAMS) { CPU_STATE(); PIND(ld64(fxi_ea(c, u))); }
// Conditional branch on the lazy flags, one handler per condition code.
PX p_jcc_slow(FXR_PARAMS) {
    SPILL_F();
    FJ_BR(fxi_cond(c, u->cc));
}
#define DEF_JCC(CC, _)                                                                     \
    PH p_jcc_##CC(FXR_PARAMS) {                                                            \
        int s_ = 0, t_ = pcond(CC, F0, F1, F2, F3, &s_);                                   \
        if (FXI_UNLIKELY(s_)) PTAIL(p_jcc_slow);                                           \
        FJ_BR(t_);                                                                         \
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
// ---- Handler replicas ----
// On Neoverse N2 an indirect branch with several successors costs 2-3 cycles more each time, even
// when predicted right (engine/fxr-probe/dispatch_probe.c, "mix": 3-4 cycles a dispatch against
// 1.3), and a handler used at two places in a loop is such a branch. The hot handler files are
// compiled twice (fxr_pin_*_r1.c: the same code at other addresses), and the lowering runs a
// handler's second place in a block (or in one copy of a loop trace) from the replica, so each
// branch keeps one successor: Ertl and Gregg's replication of interpreter instructions. A handler
// at one place per copy alternates between the copies of a trace, which also spaces out repeats of
// the same branch (the probe: a branch taken again within a few dispatches costs more).
#define RMAP_SLOTS (1u << 17)
typedef struct { PFn p, r; } RPair;
static RPair *g_rmap;
static uint64_t hpfn(PFn p) { return ((uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull) >> 40; }
static void rmap_put(PFn p, PFn r) {
    if (!p || !r || p == r) return;
    uint64_t h = hpfn(p) & (RMAP_SLOTS - 1);
    while (g_rmap[h].p && g_rmap[h].p != p) h = (h + 1) & (RMAP_SLOTS - 1);
    g_rmap[h] = (RPair){ p, r };
}
static PFn rmap_get(PFn p) {
    uint64_t h = hpfn(p) & (RMAP_SLOTS - 1);
    for (; g_rmap[h].p; h = (h + 1) & (RMAP_SLOTS - 1)) if (g_rmap[h].p == p) return g_rmap[h].r;
    return 0;
}
static void rmap_tab(const PFn *a, const PFn *b, size_t n) { for (size_t i = 0; i < n; i++) rmap_put(a[i], b[i]); }
#define RMAP(N) rmap_tab((const PFn *)fxr_##N, (const PFn *)fxr_##N##_r1, sizeof fxr_##N / sizeof(PFn))
static void rmap_init(void) {
    g_rmap = calloc(RMAP_SLOTS, sizeof *g_rmap);
    // fxr_pin_alu.c
    RMAP(t_alu_rr); RMAP(t_alu_ri); RMAP(t_alu_rt); RMAP(t_alu_tr); RMAP(t_alu_ti); RMAP(t_stti); RMAP(t_ld);
    RMAP(t_st); RMAP(t_lea); RMAP(t_sti); RMAP(t_sti_bi); RMAP(t_ea); RMAP(t_eaz); RMAP(t_mov_rr); RMAP(t_mov_ri); RMAP(t_movt);
    RMAP(t_ldt); RMAP(t_stt); RMAP(t_ext); RMAP(t_sh); RMAP(t_un); RMAP(t_imul2); RMAP(t_imul3); RMAP(t_imul2t);
    RMAP(t_imul3t); RMAP(t_cmov); RMAP(t_cmovt);
    // fxr_pin_sse.c
    RMAP(t_xl_movx); RMAP(t_xs_movx); RMAP(t_xl_movxz); RMAP(t_xs_movxz); RMAP(t_xl_movss); RMAP(t_xl_movsd); RMAP(t_xs_movss); RMAP(t_xs_movsd);
    RMAP(t_xlt_movss); RMAP(t_xlt_movsd); RMAP(t_xlt_movlps); RMAP(t_xlt_movhps); RMAP(t_xst_movss); RMAP(t_xst_movsd);
    RMAP(t_xst_movhps); RMAP(t_xst_movx); RMAP(t_xg); RMAP(t_xgt); RMAP(t_gx);
    for (size_t i = 0; i < fxr_n_xops; i++) {
        rmap_tab(&fxr_xops[i].rr[0][0], &fxr_xops_r1[i].rr[0][0], 81);
        rmap_tab(fxr_xops[i].rt, fxr_xops_r1[i].rt, 9);
    }
    for (size_t i = 0; i < fxr_n_xshift; i++) rmap_tab(fxr_xshift[i].h, fxr_xshift_r1[i].h, 9);
    // fxr_pin_mem.c, fxr_pin_mem2.c, fxr_pin_step3.c
    RMAP(t_ldi); RMAP(t_ldz8s); RMAP(t_stx); RMAP(t_sri); RMAP(t_sii); RMAP(t_3op);
    // fxr_pin_br.c, fxr_pin_step1.c, fxr_pin_step2.c: the branches that most often close a loop
    RMAP(t_fjc_rr); RMAP(t_fjc_ri); RMAP(t_fjt_ri); RMAP(t_fjt_rr); RMAP(t_fjt_rrx); RMAP(t_sic); RMAP(t_sicr);
}
// u: `copies` copies of M uops (a block: one copy). A handler at several places in a copy takes
// the replica at every second place. When every handler of the copy has a replica, those at a
// single place take it in every second copy (so each copy's last handler still has one successor;
// with a handler left out, the one before it would get two).
static void replicate(Uop *u, int M, int copies) {
    unsigned char *k = calloc((size_t)M, 1);
    int all = 1;
    for (int j = 0; j < M; j++) {
        int occ = 0, cnt = 0;
        for (int i = 0; i < M; i++) if (u[i].p == u[j].p) { cnt++; if (i < j) occ++; }
        k[j] = (unsigned char)(cnt > 1 ? 1 + (occ & 1) : 0);   // 0: alternate by copy
        if (!rmap_get(u[j].p)) all = 0;
    }
    for (int j = 0; j < M; j++) {
        PFn r = rmap_get(u[j].p);
        if (!r) continue;
        for (int c = 0; c < copies; c++)
            if (k[j] ? k[j] == 2 : all && (c & 1)) u[c * M + j].p = r;
    }
    free(k);
}

// Diagnostic knobs, for A/B runs in CI (the defaults are what ships): FXR_TRACES=0 no loop traces,
// FXR_TRACE_UOPS=n the uop budget for a trace's copies, FXR_REPLICAS=0 no handler replicas.
static int g_traces = 1, g_trace_uops = 48, g_replicas = 1, g_win_force;
// FXR_WINLOWER=1: lower as for Windows mode (the exact-state test, engine/fxr-probe/winstate_test.c)
static void knobs(void) {
    const char *e;
    if ((e = getenv("FXR_TRACES"))) g_traces = atoi(e);
    if ((e = getenv("FXR_TRACE_UOPS"))) g_trace_uops = atoi(e);
    if ((e = getenv("FXR_REPLICAS"))) g_replicas = atoi(e);
    if ((e = getenv("FXR_WINLOWER"))) g_win_force = atoi(e);
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
    rmap_init();
    knobs();
}

static int greg(unsigned off) { return (off & 7) == 0 && off < 128 ? (int)(off >> 3) : -1; }   // pinnable GPR
static int simple_mem(const Uop *u) { return u->index == R_ZERO && u->base <= R_ZERO; }        // [base + disp]
static int ea_ok(const Uop *u) { return u->base <= R_ZERO && u->index <= R_ZERO; }
static int lean(const Uop *u) { return !u->scale && !u->disp; }   // [base + index]: no scale, no displacement
static PFn ea_h(const Uop *u) { return (lean(u) ? t_eaz : t_ea)[u->base][u->index]; }   // T = the EA

typedef struct { int at, edge, stub; } Patch;   // after the copy: out[at].ulink (edge 0) / ulink2 = &out[stub]
typedef struct { Uop *out; int n, np; Patch patch[2]; } Out;
static Uop *put_uop(Out *o, const Uop *src, PFn p) { Uop *x = &o->out[o->n++]; *x = *src; x->p = p; return x; }
// The fused branch just emitted records no flags; for each edge whose successor reads them
// (fdir: bit 0 taken, bit 1 fallthrough), a stub uop (fs, fxr_pin_stub.c) appended to the block
// recomputes them from the registers and chains on to that successor.
static void edge_stubs(Out *o, unsigned fdir, PFn fs) {
    int at = o->n - 1;
    for (int e = 0; e < 2; e++) {
        if (!(fdir & (1u << e))) continue;
        Uop *s = &o->out[o->n++];
        *s = o->out[at];
        s->p = fs;
        s->imm = e ? o->out[at].aux : o->out[at].imm;   // where p_miss_t chains to
        s->ulink = 0; s->ulink2 = 0;
        o->patch[o->np++] = (Patch){ at, e, o->n - 1 };
    }
}
// T = EA of src, then the op itself
static Uop *with_ea(Out *o, const Uop *src, PFn p) {
    put_uop(o, src, ea_h(src));
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
            if (ea_ok(u) && si != 1) { put_uop(o, u, t_stx[si == 0 ? 0 : si == 2 ? 1 : 2][u->base][u->index][S]); return; }
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
        PFn fs = 0;   // the flag stub for an edge that reads them (test of two registers records them itself)
        int si = s64 ? 3 : 2;
        if (ri) { x = put_uop(o, u, t ? t_fjt_ri[s64][cc][D] : t_fjc_ri[s64][cc][D]); fs = t ? t_fs_andi[si][D] : t_fs_subi[si][D]; }
        else if (!t) { x = put_uop(o, u, t_fjc_rr[s64][cc][D][S]); fs = t_fs_sub[si][D][S]; }
        else if (D == S) { x = put_uop(o, u, t_fjt_rr[s64][cc][D]); fs = t_fs_log[si][D]; }
        else x = put_uop(o, u, t_fjt_rrx[s64][D][S]);
        x->cc = (uint8_t)cc;
        if (fs && u->fdir) edge_stubs(o, u->fdir, fs);
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
            if (!strcmp(x->name, "movx")) { put_uop(o, u, (lean(u) ? t_xl_movxz : t_xl_movx)[dc][u->base][u->index]); return; }
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
        case XS_MOVX_MR: put_uop(o, u, (lean(u) ? t_xs_movxz : t_xs_movx)[sc][u->base][u->index]); return;
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
static int g_win;   // lowering for Windows mode (fxr_lower sets it; lowering is serialised)
static int try_fuse(Out *o, const Uop *u, uint32_t left) {
    if (left < 2) return 0;
    const Uop *v = u + 1;
    const Desc *a = find(u->fn), *b = find(v->fn);
    if (!a || !b) return 0;
    // Windows mode: no pair whose second instruction touches memory after the first wrote a
    // register (a fault there would not be exact): argument setup + call, pop + pop/ret, push + push
    if (g_win && (is_named(b, N_CALL) || is_named(a, N_POP_R) || is_named(a, N_PUSH_R))) return 0;
    int D = greg(u->dst), S = greg(u->src);
    Uop *x = 0;
    // Fused handlers never record flags. A pair ending in a branch whose successor reads them
    // (v->fdir) fuses only when a stub can recompute them from registers on that edge (fs); other
    // pairs fuse only when nothing reads the flags after them.
    unsigned fd = v->fdir & 3;
    PFn fs = 0;
    if (is_named(b, N_JCC)) {   // compare-and-branch forms FXI does not fuse, ALU + jcc
        unsigned cc = v->cc & 15;
        if (a->fam == FAM_ALU && (a->a == ALU_CMP || a->a == ALU_TEST)) {
            int t = a->a == ALU_TEST, fm = a->b, si = a->c2;
            if (fm == F_MI && ea_ok(u) && !fd) {   // memory operand: no stub (another thread may change it)
                if (simple_mem(u)) { x = put_uop(o, v, t_fmi[t][si][cc][u->base]); x->disp = u->disp; }
                else { put_uop(o, u, ea_h(u)); x = put_uop(o, v, t_fmit[t][si][cc]); }
                x->fimm = (int32_t)u->imm;
                return 2;
            }
            if (fm == F_MR && S >= 0 && ea_ok(u) && !fd) {   // cmp/test [mem], reg
                put_uop(o, u, ea_h(u));
                put_uop(o, v, t_fmr[t ? 2 : 0][si][cc][S]);
                return 2;
            }
            if (fm == F_RM && !t && D >= 0 && ea_ok(u) && !fd) {   // cmp reg, [mem]
                put_uop(o, u, ea_h(u));
                put_uop(o, v, t_fmr[1][si][cc][D]);
                return 2;
            }
            if (fm == F_RI && si < 2 && D >= 0) {   // 8/16-bit register
                x = put_uop(o, v, t_fjs_ri[t][si][cc][D]);
                x->disp = (int64_t)u->imm;
                fs = t ? t_fs_andi[si][D] : t_fs_subi[si][D];
            } else if (fm == F_RR && si < 2 && D >= 0 && S >= 0) {
                x = put_uop(o, v, t && D == S ? t_fjs_rr[0][si][cc][D] : t_fjs_rrx[t][si][D][S]);
                x->dst = u->dst; x->src = u->src;
                fs = !t ? t_fs_sub[si][D][S] : D == S ? t_fs_log[si][D] : t_fs_and[si][D][S];
            } else return 0;
            if (fd) edge_stubs(o, fd, fs);
            return 2;
        }
        if (a->fam == FAM_X && a->d) {   // (u)comiss / (u)comisd
            int dbl = !strcmp(fxr_xops[a->a].name, "comisd");
            if (!a->b) {
                fs = t_fs_comis[dbl][xcls(u->dst)][xcls(u->src)];
                x = put_uop(o, v, t_fcj[dbl][cc][xcls(u->dst)][xcls(u->src)]);
            } else if (ea_ok(u) && !fd) {
                put_uop(o, u, ea_h(u));
                x = put_uop(o, v, t_fcjt[dbl][cc][xcls(u->dst)]);
            } else return 0;
            x->dst = u->dst; x->src = u->src;
            if (fd) edge_stubs(o, fd, fs);
            return 2;
        }
        if (a->fam == FAM_ALU && a->b == F_RI && a->c2 >= 2 && D >= 0) {   // add/sub/and/or/xor reg, imm
            int k = a->a == ALU_ADD ? 0 : a->a == ALU_SUB ? 1 : a->a == ALU_AND ? 2 : a->a == ALU_OR ? 3 : a->a == ALU_XOR ? 4 : -1;
            if (k < 0) return 0;
            x = put_uop(o, v, t_fai[k][a->c2 - 2][cc][D]);
            x->disp = (int64_t)u->imm; x->dst = u->dst;
            if (fd) edge_stubs(o, fd, k == 0 ? t_fs_addr[a->c2][D] : k == 1 ? t_fs_subr[a->c2][D] : t_fs_log[a->c2][D]);
            return 2;
        }
        if (a->fam == FAM_ALU && a->b == F_RR && a->c2 >= 2 && D >= 0 && S >= 0 && cc8(cc) >= 0) {   // ... reg, reg
            int k = a->a == ALU_ADD ? 0 : a->a == ALU_SUB ? 1 : a->a == ALU_AND ? 2 : a->a == ALU_OR ? 3 : a->a == ALU_XOR ? 4 : -1;
            if (k < 0) return 0;
            int si = a->c2;   // add/sub of a register with itself: the operands cannot be recovered
            fs = k == 0 ? (S != D ? t_fs_addrr[si][D][S] : 0) : k == 1 ? (S != D ? t_fs_subrr[si][D][S] : 0) : t_fs_log[si][D];
            if (fd && !fs) return 0;
            put_uop(o, v, t_arj[k][si - 2][cc8(cc)][D][S]);
            if (fd) edge_stubs(o, fd, fs);
            return 2;
        }
        if (a->fam == FAM_UNARY && !a->b && a->c2 >= 2 && D >= 0 && (a->a == U_INC || a->a == U_DEC) && !fd) {
            int ci = cc == 4 ? 0 : cc == 5 ? 1 : cc == 8 ? 2 : cc == 9 ? 3 : -1;   // inc/dec keep CF: no stub
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
        if (fd) edge_stubs(o, fd, t_fs_log[a->b][D]);
        return 2;
    }
    // mov D, [base + disp] + FXI's fused cmp D, imm / jcc (the load's displacement in fimm)
    if (a->fam == FAM_MOV && a->a == F_RM && a->b >= 2 && D >= 0 && simple_mem(u) && (int64_t)(int32_t)u->disp == u->disp &&
        b->fam == FAM_FJCC && b->a == 0 && b->b && b->c2 == (a->b == 3) && greg(v->dst) == D) {
        x = put_uop(o, v, t_lcj[a->b == 3][b->d & 15][u->base][D]);
        x->fimm = (int32_t)u->disp;
        if (fd) edge_stubs(o, fd, t_fs_subi[a->b][D]);
        return 2;
    }
    // movaps/movups/movdqa D, S ; op D, X -> D = S op X (X == D reads the copy, S)
    if (a->fam == FAM_X && !a->b && b->fam == FAM_X && !b->b && v->dst == u->dst && !strcmp(fxr_xops[a->a].name, "movx")) {
        int k = x3_index(fxr_xops[b->a].name);
        if (k >= 0) {
            unsigned X = v->src == u->dst ? u->src : v->src;
            x = put_uop(o, v, t_x3[k][xcls(u->dst)][xcls(u->src)][xcls(X)]);
            x->dst = u->dst; x->src = u->src; x->base = (uint8_t)X;
            return 2;
        }
    }
    // loop step: add/sub D, imm|reg + FXI's fused cmp (D, S | S, D | D, imm) / jcc
    if (a->fam == FAM_ALU && (a->a == ALU_ADD || a->a == ALU_SUB) && a->c2 >= 2 && D >= 0 &&
        b->fam == FAM_FJCC && b->a == 0 && b->c2 == (a->c2 == 3) && cc10(b->d) >= 0) {
        int op = a->a == ALU_SUB, s64 = a->c2 == 3, si = a->c2, ci = cc10(b->d), cd = greg(v->dst), cs = greg(v->src);
        if (a->b == F_RI) {
            if (b->b && cd == D) { x = put_uop(o, v, t_sii[op][s64][ci][D]); fs = t_fs_subi[si][D]; }
            else if (!b->b && cd == D && cs >= 0) { x = put_uop(o, v, t_sic[op][s64][ci][D][cs]); fs = t_fs_sub[si][D][cs]; }
            else if (!b->b && cs == D && cd >= 0) { x = put_uop(o, v, t_sicr[op][s64][ci][D][cd]); fs = t_fs_sub[si][cd][D]; }
            if (x) x->fimm = (int32_t)u->imm;
        } else if (a->b == F_RR && S >= 0 && b->b && cd == D) {
            x = put_uop(o, v, t_sri[op][s64][ci][D][S]);
            fs = t_fs_subi[si][D];
        }
        if (x) { if (fd) edge_stubs(o, fd, fs); return 2; }
    }
    // mov D, S ; op D, imm -> D = S op imm (the op no wider than the move; its flags dead)
    if (a->fam == FAM_MOV && a->a == F_RR && a->b >= 2 && D >= 0 && S >= 0 && D != S && greg(v->dst) == D && !v->flive) {
        if (b->fam == FAM_ALU && b->b == F_RI && b->c2 >= 2 && b->c2 <= a->b) {
            int k = b->a == ALU_ADD ? 0 : b->a == ALU_SUB ? 1 : b->a == ALU_AND ? 2 : b->a == ALU_OR ? 3 : b->a == ALU_XOR ? 4 : -1;
            if (k >= 0) { put_uop(o, v, t_3op[k][b->c2 - 2][D][S]); return 2; }
        }
        if (b->fam == FAM_SHIFT && !b->b && b->c2 >= 2 && b->c2 <= a->b && v->src != 0xffff) {
            int op = b->a, k = -1;
            if (op == SH_SHL || op == SH_SAL) k = 5;
            else if (op == SH_SHR) k = 6;
            else if (op == SH_SAR) k = 7;
            else if (op == SH_ROL && !v->cc) k = 8;
            else if (op == SH_ROR && !v->cc) k = 9;
            unsigned n = (unsigned)v->imm & (b->c2 == 3 ? 63u : 31u);
            if (k >= 0 && n) { put_uop(o, v, t_3op[k][b->c2 - 2][D][S])->imm = n; return 2; }
        }
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

// The general registers a register-only uop reads and writes (bit masks); 0 when it is anything
// else: a memory operand, reads the flags, an 8-bit high register, a shift by CL.
static int reg_rw(const Uop *u, unsigned *rd, unsigned *wr) {
    const Desc *d = find(u->fn);
    int D = greg(u->dst), S = greg(u->src);
    if (!d || D < 0) return 0;
    unsigned dm = 1u << D, sm = S >= 0 ? 1u << S : 0;
    switch (d->fam) {
    case FAM_ALU:
        if (d->a == ALU_ADC || d->a == ALU_SBB) return 0;
        if (d->b == F_RR && S >= 0) { *rd = dm | sm; *wr = (d->a == ALU_CMP || d->a == ALU_TEST) ? 0 : dm; return 1; }
        if (d->b == F_RI) { *rd = dm; *wr = (d->a == ALU_CMP || d->a == ALU_TEST) ? 0 : dm; return 1; }
        return 0;
    case FAM_MOV:
        if (d->a == F_RR && S >= 0) { *rd = sm | (d->b < 2 ? dm : 0); *wr = dm; return 1; }
        if (d->a == F_RI) { *rd = d->b < 2 ? dm : 0; *wr = dm; return 1; }
        return 0;
    case FAM_SHIFT: if (d->b || u->src == 0xffff) return 0; *rd = dm; *wr = dm; return 1;
    case FAM_UNARY: if (d->b) return 0; *rd = dm; *wr = dm; return 1;
    case FAM_EXT: if (d->b || S < 0) return 0; *rd = sm; *wr = dm; return 1;
    case FAM_IMUL2: if (d->a || S < 0) return 0; *rd = dm | sm; *wr = dm; return 1;
    case FAM_IMUL3: if (d->a || S < 0) return 0; *rd = sm; *wr = dm; return 1;
    case FAM_LEA: if (!simple_mem(u) || u->base > 15) return 0; *rd = 1u << u->base; *wr = dm; return 1;
    }
    return 0;
}
// A loop step with something between it and its compare ([a][x][c], c FXI's fused cmp+jcc on the
// stepped register): x may run first when it is register-only, reads no flags, and neither reads
// nor writes anything a or c use, or reads what a writes. No memory access moves, so the state at
// a fault stays exact. Then a and c fuse.
static int can_hoist(const Uop *a, const Uop *x, const Uop *c) {
    const Desc *da = find(a->fn), *dc = find(c->fn);
    if (!da || !dc || da->fam != FAM_ALU || (da->a != ALU_ADD && da->a != ALU_SUB) || dc->fam != FAM_FJCC || dc->a)
        return 0;
    int D = greg(a->dst), S = da->b == F_RR ? greg(a->src) : -2, cd = greg(c->dst), cs = dc->b ? -2 : greg(c->src);
    if (D < 0 || S == -1 || cd < 0 || cs == -1 || (cd != D && cs != D)) return 0;
    unsigned rd, wr, used = 1u << D | (S >= 0 ? 1u << S : 0) | 1u << cd | (cs >= 0 ? 1u << cs : 0);
    return reg_rw(x, &rd, &wr) && !(wr & used) && !(rd & (1u << D));
}

// The 3-operand pair with an instruction between ([a][x][c]: mov D, S ; x ; op D, imm|shift): when
// x is register-only, reads no flags and neither reads nor writes D, a and c may fuse and run
// before x (x then sees what it saw before: D is not its business, S is read by the pair first
// either way, and the flags c leaves are dead, which try_fuse requires of the 3-operand forms).
static int can_sink(const Uop *a, const Uop *x, const Uop *c) {
    const Desc *da = find(a->fn), *dc = find(c->fn);
    if (!da || !dc || da->fam != FAM_MOV || da->a != F_RR || (dc->fam != FAM_ALU && dc->fam != FAM_SHIFT)) return 0;
    int D = greg(a->dst);
    unsigned rd, wr;
    return D >= 0 && greg(a->src) >= 0 && greg(c->dst) == D && reg_rw(x, &rd, &wr) && !((rd | wr) & (1u << D));
}

// Instruction boundaries, for an exact x64 state at a stop (fxr_win_host_state): out[start..n),
// just lowered, take rip (the first x64 instruction they cover; 0: keep theirs), and the flag
// stubs among them (patches from np on) are no boundary: their branch already went.
static void bound(Out *o, int start, int np, uint64_t rip, int nobound) {
    for (int k = start; k < o->n; k++) {
        int stub = 0;
        for (int j = np; j < o->np; j++) if (o->patch[j].stub == k) stub = 1;
        if (stub || nobound) o->out[k].fdir |= FXR_NOBOUND;
        else if (rip) o->out[k].rip = rip;
    }
}

static void lower_block(Out *o, const Block *b) {
    for (uint32_t i = 0; i < b->n; i++) {
        int start = o->n, np = o->np;
        if (i + 2 < b->n && can_hoist(&b->u[i], &b->u[i + 1], &b->u[i + 2])) {
            // [a][x][c] runs as x, then a + c: x starts where a did; between x and the pair is no
            // x64 instruction boundary
            Uop pair[2] = { b->u[i], b->u[i + 2] };
            lower_one(o, &b->u[i + 1]);
            bound(o, start, np, b->u[i].rip, 0);
            int mid = o->n;
            np = o->np;
            if (try_fuse(o, pair, 2)) { bound(o, mid, np, 0, 1); i += 2; continue; }
            lower_one(o, &b->u[i]);   // not fused after all: a, then c in its turn (order still valid)
            bound(o, mid, np, 0, 1);
            i++;
            continue;
        }
        if (i + 2 < b->n && can_sink(&b->u[i], &b->u[i + 1], &b->u[i + 2])) {
            // [a][x][c] runs as a + c, then x: no boundary before x
            Uop pair[2] = { b->u[i], b->u[i + 2] };
            if (try_fuse(o, pair, 2)) {
                bound(o, start, np, b->u[i].rip, 0);
                int mid = o->n;
                np = o->np;
                lower_one(o, &b->u[i + 1]);
                bound(o, mid, np, 0, 1);
                i += 2;
                continue;
            }
        }
        if (try_fuse(o, &b->u[i], b->n - i)) { bound(o, start, np, b->u[i].rip, 0); i++; continue; }
        lower_one(o, &b->u[i]);
        bound(o, start, np, 0, 0);
    }
}

// ---- Loop traces (superblocks) ----
// When a block starts a loop, its blocks along the path back to it are laid out one after another
// (a trace), in several copies while they fit in 48 uops (g_trace_uops), then the flag stubs. Each edge
// along the trace (to the next block, or from the last block back to the first) leads to the next
// uop, which the branch handlers reach with an add instead of loading the link (PCHAIN_T,
// fxr_pin.h); only the last copy's edge back to the start loads it. Every guest instruction and
// every branch of every iteration still runs as before: an exit leaves from whichever copy it is
// in, and the blocks also exist on their own for entries from elsewhere.
enum { TRACE_SEGS = 4, TRACE_COPIES = 8, TRACE_MAX = 160, LOOP_SCAN = 1024 };   // copies: g_trace_uops (48) uops

// Is rip a loop head: does a direct branch (jcc, jmp; rel8 or rel32) in the LOOP_SCAN bytes after
// it jump back to it? A scan of the raw bytes: a false match only costs a search that finds no
// loop. Returns the end of the furthest such branch (the loop's code is [rip, end)), or 0.
static uint64_t loop_end(struct Fxi *vm, uint64_t rip) {
    uint64_t lo = (uint64_t)(uintptr_t)vm->image, hi = lo + vm->image_size, found = 0;
    if (vm->windows || rip < lo || rip >= hi) return 0;
    uint64_t end = hi - rip > LOOP_SCAN ? rip + LOOP_SCAN : hi;
    for (uint64_t a = rip; a + 2 <= end; a++) {
        const uint8_t *q = (const uint8_t *)(uintptr_t)a;
        int32_t r;
        if (((q[0] & 0xf0) == 0x70 || q[0] == 0xeb) && a + 2 + (uint64_t)(int64_t)(int8_t)q[1] == rip) found = a + 2;
        else if (q[0] == 0xe9 && a + 5 <= end && (memcpy(&r, q + 1, 4), a + 5 + (uint64_t)(int64_t)r == rip)) found = a + 5;
        else if (q[0] == 0x0f && (q[1] & 0xf0) == 0x80 && a + 6 <= end && (memcpy(&r, q + 2, 4), a + 6 + (uint64_t)(int64_t)r == rip))
            found = a + 6;
    }
    return found;
}

// A decoded block's direct successors: e[0] the taken target (jcc, jmp) or the next block (a goto
// that splits a long block), e[1] a conditional branch's fallthrough. Returns how many (0: none
// the trace can follow: calls, returns, indirect jumps, syscalls).
static int trace_succ(const Block *b, uint64_t e[2]) {
    if (!b->n) return 0;
    const Uop *t = &b->u[b->n - 1];
    const Desc *d = find(t->fn);
    if (d && (d->fam == FAM_FJCC || is_named(d, N_JCC))) { e[0] = t->imm; e[1] = t->aux; return 2; }
    if (is_named(d, N_JMP)) { e[0] = t->imm; return 1; }
    if (is_named(d, N_GOTO)) { e[0] = t->aux; return 1; }
    return 0;
}

// The blocks of the loop starting at b: seg[0] = b, seg[i + 1] the successor of seg[i] along
// edge[i] (0 taken / jump / goto, 1 fallthrough), and the last block's edge[] leads back to b.
// Follows the successor inside the loop's code (the fallthrough when both are). Returns how many
// blocks, or 0 when there is no such loop (any blocks it decoded are freed).
static int find_trace(struct Fxi *vm, Block *b, Block **seg, int *edge) {
    uint64_t end = 0, e[2];
    int n = 1, scanned = 0;
    seg[0] = b;
    for (;;) {
        int k = trace_succ(seg[n - 1], e);
        if (!k) break;
        if (e[0] == b->rip) { edge[n - 1] = 0; return n; }
        if (k == 2 && e[1] == b->rip) { edge[n - 1] = 1; return n; }
        if (n == TRACE_SEGS) break;
        if (!scanned) { end = loop_end(vm, b->rip); scanned = 1; }
        if (!end) break;
        int j = k == 2 && e[1] > b->rip && e[1] < end ? 1 : e[0] > b->rip && e[0] < end ? 0 : -1;
        for (int i = 1; j >= 0 && i < n; i++) if (seg[i]->rip == e[j]) j = -1;   // an inner loop
        if (j < 0) break;
        edge[n - 1] = j;
        seg[n++] = fxr_decode(vm, e[j]);
    }
    for (int i = 1; i < n; i++) free(seg[i]);
    return 0;
}

Block *fxr_lower(struct Fxi *vm, Block *b) {
    pthread_once(&g_once, init_desc);
    g_win = (vm && vm->windows) || g_win_force;
    Block *seg[TRACE_SEGS];
    int edge[TRACE_SEGS], ns = g_traces ? find_trace(vm, b, seg, edge) : 0, nseg = ns ? ns : 1;
    if (!ns) seg[0] = b;
    Out o[TRACE_SEGS];
    int m[TRACE_SEGS], off[TRACE_SEGS], soff[TRACE_SEGS], M = 0, S = 0;
    for (int i = 0; i < nseg; i++) {
        o[i] = (Out){ malloc(sizeof(Uop) * (2 * (size_t)seg[i]->n + 3)), 0, 0, { { 0, 0, 0 } } };   // + 2 flag stubs
        lower_block(&o[i], seg[i]);
        m[i] = o[i].n - o[i].np;
        off[i] = M; soff[i] = S;
        M += m[i]; S += o[i].np;
    }
    // A trace edge whose successor reads the flags needs its stub: no trace then.
    for (int i = 0; ns && i < nseg; i++) {
        if (m[i] < 1) ns = 0;
        for (int k = 0; ns && k < o[i].np; k++) if (o[i].patch[k].edge == edge[i]) ns = 0;
    }
    if (M > TRACE_MAX) ns = 0;
    if (!ns && nseg > 1) {   // the first block alone
        for (int i = 1; i < nseg; i++) { free(o[i].out); free(seg[i]); }
        nseg = 1; M = m[0]; S = o[0].np;
    }
    int copies = 1;
    if (ns) { copies = g_trace_uops / M; if (copies > TRACE_COPIES) copies = TRACE_COPIES; if (copies < 1) copies = 1; }
    if (copies > 1 && (copies & 1)) copies--;   // even: the replicas alternate between copies (replicate)
    uint32_t n = (uint32_t)(copies * M + S);
    Block *nb = malloc(sizeof(Block) + sizeof(Uop) * n);
    nb->rip = b->rip;
    nb->n = n;
    for (int c = 0; c < copies; c++)
        for (int i = 0; i < nseg; i++) memcpy(&nb->u[c * M + off[i]], o[i].out, sizeof(Uop) * (size_t)m[i]);
    for (int i = 0; i < nseg; i++) memcpy(&nb->u[copies * M + soff[i]], o[i].out + m[i], sizeof(Uop) * (size_t)o[i].np);
#ifdef FXR_PROFILE
    for (uint32_t k = 0; k < n; k++) nb->u[k].prof = 0;
#endif
    for (int c = 0; c < copies; c++)
        for (int i = 0; i < nseg; i++) {
            int base = c * M + off[i];
            for (int k = 0; k < o[i].np; k++) {   // point the branch's edges at their flag stubs
                Uop *br = &nb->u[base + o[i].patch[k].at], *st = &nb->u[copies * M + soff[i] + (o[i].patch[k].stub - m[i])];
                if (o[i].patch[k].edge) br->ulink2 = st; else br->ulink = st;
            }
            if (ns) {   // the trace edge: the next block, or back to the start (the next copy's)
                Uop *t = &nb->u[base + m[i] - 1], *to = i + 1 < nseg ? t + 1 : &nb->u[c + 1 < copies ? (c + 1) * M : 0];
                if (edge[i]) t->ulink2 = to; else t->ulink = to;
            }
        }
    if (g_replicas) replicate(nb->u, M, copies);
    for (int i = 0; i < nseg; i++) { free(o[i].out); free(seg[i]); }
    return nb;
}

void fxr_init_stop(Block *b) { b->u[0].p = p_stop; }

#ifdef FXR_PROFILE
// Diagnostic build (-DFXR_PROFILE): every dispatch counts its uop. At exit, the total and the
// hottest blocks, uop by uop: count, handler (as an offset from fxr_lower; CI names it with nm),
// guest rip (as an offset into the loaded image). Shows what one iteration of a hot loop costs.
void fxr_profile_dump(struct Fxi *vm) {
    enum { TOP = 8 };
    BlockTable *t = vm->table;
    Block *top[TOP] = { 0 };
    uint64_t topn[TOP] = { 0 }, total = 0, nblocks = 0;
    for (uint64_t i = 0; i <= t->mask; i++) {
        Block *b = t->slot[i];
        if (!b || !b->n || b->u[0].p == p_stop) continue;
        uint64_t s = 0;
        for (uint32_t k = 0; k < b->n; k++) s += b->u[k].prof;
        total += s;
        nblocks++;
        for (int j = 0; j < TOP; j++)
            if (s > topn[j]) {
                for (int m = TOP - 1; m > j; m--) { top[m] = top[m - 1]; topn[m] = topn[m - 1]; }
                top[j] = b; topn[j] = s;
                break;
            }
    }
    fprintf(stderr, "[fxr-prof] %llu dispatches in %llu blocks\n", (unsigned long long)total, (unsigned long long)nblocks);
    for (int j = 0; j < TOP && top[j]; j++) {
        Block *b = top[j];
        fprintf(stderr, "[fxr-prof] block img+%#llx: %u uops, %llu dispatches (%.1f%%)\n",
                (unsigned long long)(b->rip - (uintptr_t)vm->image), b->n,
                (unsigned long long)topn[j], 100.0 * (double)topn[j] / (double)(total ? total : 1));
        for (uint32_t k = 0; k < b->n; k++)
            fprintf(stderr, "[fxr-prof]   %12llu  @%ld  img+%#llx\n", (unsigned long long)b->u[k].prof,
                    (long)((intptr_t)b->u[k].p - (intptr_t)fxr_lower), (unsigned long long)(b->u[k].rip - (uintptr_t)vm->image));
    }
}
#endif

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

// ---- Windows mode: the x64 state at a host fault or at a stop of the thread ----
// FXR keeps the guest registers in host registers (FXR_PARAMS, preserve_none on ARM64): RAX x22,
// RCX x23, RDX x24, RBX x28, RSP x27, RBP x0, RSI x25, RDI x26, R8-R14 x1-x7, R15 x10, T x11,
// flags x12, x13, x14, x9 (clang gives x9 last: frame lowering's scratch), XMM0-7 v0-v7; x21 is
// the uop. At a fault inside a pinned handler (section
// fxr_h) those hold the x64 state as before the instruction: no handler writes guest-visible
// state before its last guest memory access (engine/fxr-probe/precise.py checks it in CI, and
// Windows mode lowers the forms that would to FXI's handlers). Outside the pinned handlers the
// state is in the CPU structure: FXI's handlers under p_slow, syscalls and block lookups run
// with the registers spilled.
#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none)
#define FXR_HOST_STATE 1
#if defined(__APPLE__)
extern const char fxr_h_start[] __asm("section$start$__TEXT$__fxr_h");
extern const char fxr_h_end[] __asm("section$end$__TEXT$__fxr_h");
#else
extern const char __start_fxr_h[], __stop_fxr_h[];
#define fxr_h_start __start_fxr_h
#define fxr_h_end __stop_fxr_h
#endif
static const uint8_t k_host_gpr[16] = { 22, 23, 24, 28, 27, 0, 25, 26, 1, 2, 3, 4, 5, 6, 7, 10 };   // RAX..R15
#endif

static uintptr_t *g_hidx;   // every pinned handler's start, sorted
static size_t g_hn, g_hcap;
static pthread_once_t g_hidx_once = PTHREAD_ONCE_INIT;
static void hidx_add(PFn p) {
    if (!p) return;
    if (g_hn == g_hcap) { g_hcap = g_hcap ? 2 * g_hcap : 1 << 16; g_hidx = realloc(g_hidx, g_hcap * sizeof *g_hidx); }
    g_hidx[g_hn++] = (uintptr_t)p;
}
static void hidx_tab(const PFn *a, size_t n) { for (size_t i = 0; i < n; i++) hidx_add(a[i]); }
static int cmp_uptr(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}
static void hidx_build(void) {
    pthread_once(&g_once, init_desc);   // the replica map
#define HIDX_TAB(NAME, DIMS) hidx_tab((const PFn *)fxr_##NAME, sizeof fxr_##NAME / sizeof(PFn));
    FXR_TABLES(HIDX_TAB)
    for (size_t i = 0; i < RMAP_SLOTS; i++) hidx_add(g_rmap[i].r);
    for (size_t i = 0; i < fxr_n_xops; i++) { hidx_tab(&fxr_xops[i].rr[0][0], 81); hidx_tab(fxr_xops[i].rt, 9); }
    for (size_t i = 0; i < fxr_n_xshift; i++) hidx_tab(fxr_xshift[i].h, 9);
    hidx_tab(t_push, 16); hidx_tab(t_pop, 16); hidx_tab(t_call_R, 16); hidx_tab(t_jmp_R, 16); hidx_tab(t_jcc, 16);
    static const PFn one[] = { p_stop, p_nop, p_jmp, p_goto, p_call, p_ret, p_call_T, p_jmp_T, p_call_M, p_jmp_M, p_syscall };
    hidx_tab(one, sizeof one / sizeof one[0]);
    qsort(g_hidx, g_hn, sizeof *g_hidx, cmp_uptr);
    size_t n = 0;
    for (size_t i = 0; i < g_hn; i++) if (!n || g_hidx[i] != g_hidx[n - 1]) g_hidx[n++] = g_hidx[i];
    g_hn = n;
}
void fxr_win_index(void) { pthread_once(&g_hidx_once, hidx_build); }

#ifdef FXR_HOST_STATE
// The start of the pinned handler containing pc (0: none).
static uintptr_t handler_at(uintptr_t pc) {
    size_t lo = 0, hi = g_hn;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_hidx[mid] <= pc) lo = mid + 1; else hi = mid; }
    return lo ? g_hidx[lo - 1] : 0;
}
// How far the handler at h moved x21 (the uop) before pc, in address order: the dispatch's load
// with writeback (ldr xT, [x21, #imm]!) or an add x21, x21, #imm. INT64_MIN after a mov x21, xN
// (the chain to another block). The caller checks the result against the handler's identity.
static int64_t x21_moved(uintptr_t h, uintptr_t pc) {
    int64_t d = 0;
    for (const uint32_t *p = (const uint32_t *)h; (uintptr_t)p < pc; p++) {
        uint32_t i = *p;
        if ((i & 0xFFE00C00u) == 0xF8400C00u && ((i >> 5) & 31) == 21) {   // ldr Xt, [x21, #simm9]!
            int32_t imm = (int32_t)((i >> 12) & 0x1FF);
            d += imm >= 256 ? imm - 512 : imm;
        } else if ((i & 0xFF8003FFu) == 0x910002B5u) {                    // add x21, x21, #imm{, lsl 12}
            d += (int64_t)((i >> 10) & 0xFFF) << ((i >> 22) & 1 ? 12 : 0);
        } else if ((i & 0xFFE0FFFFu) == 0xAA0003F5u) {                    // mov x21, xN
            return INT64_MIN;
        }
    }
    return d;
}
static void load_host(FxiCpu *c, const fxr_host_state *h) {
    for (int i = 0; i < 16; i++) c->r[i] = h->x[k_host_gpr[i]];
    uint64_t F0 = h->x[12], F1 = h->x[13], F2 = h->x[14], F3 = h->x[9];
    SPILL_F();
    for (int i = 0; i < 8; i++) memcpy(&c->xmm[i], h->q[i], 16);
}
#endif

int fxr_win_host_state(FxiCpu *c, fxr_host_state *h, int fault, uint64_t *rip) {
#ifdef FXR_HOST_STATE
    uintptr_t pc = (uintptr_t)h->pc;
    if (pc < (uintptr_t)fxr_h_start || pc >= (uintptr_t)fxr_h_end) {
        // FXI's code under p_slow, a block lookup or translation (registers in the CPU
        // structure): exact for a fault (FXI's rule: the instruction whose access faulted, or
        // the block being translated). A stop there may be mid-way through FXI's handler.
        if (!fault) return 0;
        *rip = c->cur ? c->cur->rip : c->rip;
        return 1;
    }
    if (!g_hn) return 0;
    uintptr_t hs = handler_at(pc);
    if (!hs) return 0;
    Uop *u = (Uop *)(uintptr_t)h->x[21], *cur = NULL;
    if (fault) {
        int64_t d = x21_moved(hs, pc);
        if (d != INT64_MIN && d % (int64_t)sizeof(Uop) == 0) cur = (Uop *)((uintptr_t)u - (uintptr_t)d);
        if (!cur || (uintptr_t)cur->p != hs) {
            if ((uintptr_t)u->p == hs) cur = u;
            else if ((uintptr_t)(u - 1)->p == hs) cur = u - 1;
            else return 0;
        }
    } else {
        uint32_t ins = *(const uint32_t *)pc;
        cur = u;
        if (pc == hs) {                                         // a handler's start: x21 is its uop
            if ((uintptr_t)cur->p != hs || (cur->fdir & FXR_NOBOUND)) return 0;
        } else if ((ins & 0xFFFFFC1Fu) == 0xD61F0000u) {        // br Xn: the dispatch, x21 the next uop
            uint64_t target = h->x[(ins >> 5) & 31];
            if ((uintptr_t)cur->p != target || (cur->fdir & FXR_NOBOUND)) return 0;
            h->pc = target;
        } else {
            return 0;
        }
    }
    load_host(c, h);
    c->rip = cur->rip;
    c->cur = cur;
    *rip = cur->rip;
    return 1;
#else
    (void)c; (void)h; (void)fault; (void)rip;
    return 0;
#endif
}
