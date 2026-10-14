// SPDX-License-Identifier: GPL-3.0-or-later
// Two distinct guest loads and two instruction boundaries; the EA temporary
// carries the first address to the second handler. Flags remain unchanged.
#include "gpta_pin.h"
#include "gpta_pin_eareuse.h"
#define ER_DEF(D,I,B,SZ) \
    PH p_rf_##SZ##_##B##_##I##_##D(GPTA_PARAMS) { \
        T = g##B + (g##I << u->scale) + (uint64_t)u->disp; \
        uint64_t v = ld##SZ(T); KEEP_R(D); g##D = v; PNEXT(); }
#define ER_INDEX(I,B,SZ) R16C(ER_DEF,I,B,SZ)
#define ER_BASE(B,SZ) R17B(ER_INDEX,B,SZ)
R17(ER_BASE,32) R17(ER_BASE,64) R17(ER_BASE,8)
#define ER_E(D,I,B,SZ) p_rf_##SZ##_##B##_##I##_##D,
#define ER_ROW(I,B,SZ) { R16C(ER_E,I,B,SZ) },
#define ER_PLANE(B,SZ) { R17B(ER_ROW,B,SZ) },
#define ER_TABLE(SZ) { R17(ER_PLANE,SZ) }
const PFn t_rf[3][17][17][16] = { ER_TABLE(32),ER_TABLE(64),ER_TABLE(8) };
#define ER_SECOND(D,SZ,Z) \
    PH p_rs_##SZ##_##Z##_##D(GPTA_PARAMS) { \
        uint64_t v = ld##SZ(T + ((Z) ? 0 : u->imm)); \
        KEEP_R(D); g##D = v; PNEXT(); }
#define ER_Z(Z,SZ) R16(ER_SECOND,SZ,Z)
ER_Z(0,32) ER_Z(1,32) ER_Z(0,64) ER_Z(1,64) ER_Z(0,8) ER_Z(1,8)
#define ER_SE(D,SZ,Z) p_rs_##SZ##_##Z##_##D,
#define ER_SZ(SZ) { { R16(ER_SE,SZ,0) }, { R16(ER_SE,SZ,1) } }
const PFn t_rs[3][2][16] = { ER_SZ(32),ER_SZ(64),ER_SZ(8) };
