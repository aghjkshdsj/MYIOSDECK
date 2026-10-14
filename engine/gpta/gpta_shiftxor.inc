// SPDX-License-Identifier: GPL-3.0-or-later
// Corpus-selected immediate shift/rotate and XOR using the result. Both guest
// surviving results and XOR flags are preserved; no guest loop or immediate
// value is recognized.
#include "gpta_pin.h"
#include "gpta_pin_shiftxor.h"
#define SX_shl(A,N,SZ) ((A) << (N))
#define SX_shr(A,N,SZ) ((A) >> (N))
#define SX_sar(A,N,SZ) ((uint64_t)((ST##SZ)(A) >> (N)))
#define SX_rol(A,N,SZ) (((A) << (N)) | ((A) >> ((SZ - (N)) & (SZ - 1))))
#define SX_ror(A,N,SZ) (((A) >> (N)) | ((A) << ((SZ - (N)) & (SZ - 1))))
#define SX_DEF(E,D,SZ,F,ROLE) SX_DEF_EXPAND(E,D,SZ,F,ROLE,SX_OP)
#define SX_DEF_EXPAND(E,D,SZ,F,ROLE,OP) SX_DEF2(E,D,SZ,F,ROLE,OP)
#define SX_DEF2(E,D,SZ,F,ROLE,OP) \
    PH p_sx_##OP##_##SZ##_##F##_##ROLE##_##D##_##E(GPTA_PARAMS) { \
        unsigned n = (unsigned)u->imm & (SZ - 1); \
        uint64_t a = g##D & M##SZ, t = SX_##OP(a,n,SZ) & M##SZ, r; \
        if ((ROLE) == 0) { r = (t ^ ((D) == (E) ? t : g##E)) & M##SZ; g##D = r; } \
        else { g##D = t; r = (g##E ^ t) & M##SZ; g##E = r; } \
        if (F) { F0 = LF(LF_LOGIC,SI##SZ); F3 = r; } PNEXT(); }
#define SX_DST(D,SZ,F,ROLE) R16B(SX_DEF,D,SZ,F,ROLE)
#define SX_WIDTH(SZ,F,ROLE) R16(SX_DST,SZ,F,ROLE)
#define SX_FLAGS(F,ROLE) SX_WIDTH(32,F,ROLE) SX_WIDTH(64,F,ROLE)
SX_FLAGS(0,0) SX_FLAGS(1,0) SX_FLAGS(0,1) SX_FLAGS(1,1)
#define SX_E(E,D,SZ,F,ROLE) SX_E_EXPAND(E,D,SZ,F,ROLE,SX_OP)
#define SX_E_EXPAND(E,D,SZ,F,ROLE,OP) SX_E2(E,D,SZ,F,ROLE,OP)
#define SX_E2(E,D,SZ,F,ROLE,OP) p_sx_##OP##_##SZ##_##F##_##ROLE##_##D##_##E,
#define SX_ROW(D,SZ,F,ROLE) { R16B(SX_E,D,SZ,F,ROLE) },
#define SX_TAB(SZ,F,ROLE) { R16(SX_ROW,SZ,F,ROLE) }
#define SX_F(F,ROLE) { SX_TAB(32,F,ROLE),SX_TAB(64,F,ROLE) }
#define SX_ROLE(ROLE) { SX_F(0,ROLE),SX_F(1,ROLE) }
const PFn SX_TABLE[2][2][2][16][16] = { SX_ROLE(0),SX_ROLE(1) };
