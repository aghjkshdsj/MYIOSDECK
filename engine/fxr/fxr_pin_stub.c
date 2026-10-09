// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: flag stubs. A fused compare-and-branch never records the flags. When the block on one of
// its edges reads them (the decoder's per-edge liveness, u->fdir), the lowering points that edge
// at one of these stubs, appended to the block: it recomputes the lazy flags exactly as the fused
// instruction would have left them, from the registers (nothing ran since the branch, so they
// still hold its operands or result), then chains to the real successor (imm). A loop's back edge
// stays free of flag work; only the exit that needs them pays. See fxr_pin.c.

#include "fxr_pin.h"

#define FS_TAIL() PCHAIN(u->ulink, p_miss_t)
#define DEF_FS_RR(S, D, SZ)                                                                \
    PH p_fs_sub_##SZ##_##D##_##S(FXR_PARAMS) {   /* cmp D, S */                            \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ; SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0); FS_TAIL(); } \
    PH p_fs_and_##SZ##_##D##_##S(FXR_PARAMS) {   /* test D, S */                           \
        uint64_t a = g##D & M##SZ, b = g##S & M##SZ; SETF(LF_LOGIC, SI##SZ, a, b, a & b, 0); FS_TAIL(); } \
    PH p_fs_addrr_##SZ##_##D##_##S(FXR_PARAMS) {   /* D = a + S happened (S != D): a = D - S */ \
        uint64_t r = g##D & M##SZ, b = g##S & M##SZ, a = (r - b) & M##SZ; SETF(LF_ADD, SI##SZ, a, b, r, 0); FS_TAIL(); } \
    PH p_fs_subrr_##SZ##_##D##_##S(FXR_PARAMS) {   /* D = a - S happened (S != D): a = D + S */ \
        uint64_t r = g##D & M##SZ, b = g##S & M##SZ, a = (r + b) & M##SZ; SETF(LF_SUB, SI##SZ, a, b, r, 0); FS_TAIL(); }
#define DEF_FS_RR_ROW(D, SZ) R16B(DEF_FS_RR, D, SZ)                                        \
    PH p_fs_subi_##SZ##_##D(FXR_PARAMS) {   /* cmp D, imm (disp) */                        \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ; SETF(LF_SUB, SI##SZ, a, b, (a - b) & M##SZ, 0); FS_TAIL(); } \
    PH p_fs_andi_##SZ##_##D(FXR_PARAMS) {   /* test D, imm (disp) */                       \
        uint64_t a = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ; SETF(LF_LOGIC, SI##SZ, a, b, a & b, 0); FS_TAIL(); } \
    PH p_fs_log_##SZ##_##D(FXR_PARAMS) {    /* test D, D; and/or/xor result in D */        \
        uint64_t r = g##D & M##SZ; SETF(LF_LOGIC, SI##SZ, r, r, r, 0); FS_TAIL(); }        \
    PH p_fs_addr_##SZ##_##D(FXR_PARAMS) {   /* D = a + imm (disp) happened: a = D - imm */ \
        uint64_t r = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, a = (r - b) & M##SZ;    \
        SETF(LF_ADD, SI##SZ, a, b, r, 0); FS_TAIL(); }                                     \
    PH p_fs_subr_##SZ##_##D(FXR_PARAMS) {   /* D = a - imm (disp) happened: a = D + imm */ \
        uint64_t r = g##D & M##SZ, b = (uint64_t)u->disp & M##SZ, a = (r + b) & M##SZ;    \
        SETF(LF_SUB, SI##SZ, a, b, r, 0); FS_TAIL(); }
R16(DEF_FS_RR_ROW, 8) R16(DEF_FS_RR_ROW, 16) R16(DEF_FS_RR_ROW, 32) R16(DEF_FS_RR_ROW, 64)
#define T_FS_RR(NAME) { { R16(ROW_RR, NAME##_8) }, { R16(ROW_RR, NAME##_16) }, { R16(ROW_RR, NAME##_32) }, { R16(ROW_RR, NAME##_64) } }
const PFn t_fs_sub[4][16][16] = T_FS_RR(fs_sub);   // [size][D][S]
const PFn t_fs_and[4][16][16] = T_FS_RR(fs_and);
const PFn t_fs_addrr[4][16][16] = T_FS_RR(fs_addrr);
const PFn t_fs_subrr[4][16][16] = T_FS_RR(fs_subrr);
#define T_FS_R(NAME) { { R16(E1, NAME##_8) }, { R16(E1, NAME##_16) }, { R16(E1, NAME##_32) }, { R16(E1, NAME##_64) } }
const PFn t_fs_subi[4][16] = T_FS_R(fs_subi);   // [size][D]
const PFn t_fs_andi[4][16] = T_FS_R(fs_andi);
const PFn t_fs_log[4][16] = T_FS_R(fs_log);
const PFn t_fs_addr[4][16] = T_FS_R(fs_addr);
const PFn t_fs_subr[4][16] = T_FS_R(fs_subr);

// (u)comiss / (u)comisd of two XMM registers
#define DEF_FS_COM(S, D)                                                                   \
    PH p_fs_comisd_##D##_##S(FXR_PARAMS) { XCOMIS_SET(LD0(XD_##D), LD0(XS_##S)); FS_TAIL(); } \
    PH p_fs_comiss_##D##_##S(FXR_PARAMS) { XCOMIS_SET(LF0(XD_##D), LF0(XS_##S)); FS_TAIL(); }
#define DEF_FS_COM_ROW(D, _) X9B(DEF_FS_COM, D)
X9(DEF_FS_COM_ROW, _)
#define E_FSC(S, D, NAME) p_##NAME##_##D##_##S,
#define ROW_FSC(D, NAME) { X9B(E_FSC, D, NAME) },
const PFn t_fs_comis[2][9][9] = { { X9(ROW_FSC, fs_comiss) }, { X9(ROW_FSC, fs_comisd) } };   // [double?][D][S]
