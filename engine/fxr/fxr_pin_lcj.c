// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: "mov D, [B + disp] ; cmp D, imm ; jcc" as one uop (the corpus, engine/fxr/corpus.sh:
// "load r ; test/cmp r"; the test form is in fxr_pin_fuse.c), specialised on base, destination and
// condition. The load's displacement rides in fimm, the compare's immediate in disp. See fxr_pin.c.

#include "fxr_pin.h"

#define DEF_LCJ(D, B, SZ, CC)                                                              \
    PH p_lcj_##SZ##_##CC##_##B##_##D(FXR_PARAMS) {                                         \
        uint64_t r = ld##SZ(g##B + (uint64_t)(int64_t)u->fimm); g##D = r;                  \
        FJ_BR(cc_sub(CC, r, (uint64_t)u->disp, SI##SZ)); }
#define DEF_LCJ_B(B, SZ, CC) R16B(DEF_LCJ, B, SZ, CC)
#define DEF_LCJ_CC(CC, SZ) R17(DEF_LCJ_B, SZ, CC)
C16(DEF_LCJ_CC, 32) C16(DEF_LCJ_CC, 64)
#define E_LCJ(D, B, SZ, CC) p_lcj_##SZ##_##CC##_##B##_##D,
#define ROW_LCJ(B, SZ, CC) { R16B(E_LCJ, B, SZ, CC) },
#define CC_LCJ(CC, SZ) { R17(ROW_LCJ, SZ, CC) },
const PFn t_lcj[2][16][17][16] = { { C16(CC_LCJ, 32) }, { C16(CC_LCJ, 64) } };   // [64?][cc][base][D]
