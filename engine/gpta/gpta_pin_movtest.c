// SPDX-License-Identifier: GPL-3.0-or-later
// Corpus family: register copy followed by test of the copy and a branch.
// Keep the original chaining behavior. Other edges reconstruct TEST flags
// from the copied destination using the existing flag stubs.
#include "gpta_pin.h"
#include "gpta_pin_movtest.h"
#define MT_DEF(S,D,CC,MSZ,TSZ) \
    PH p_mtj_##MSZ##_##TSZ##_##CC##_##D##_##S(GPTA_PARAMS) { \
        g##D = g##S & M##MSZ; FJ_BR(cc_logic(CC,g##D,SI##TSZ)); }
#define MT_DST(D,CC,MSZ,TSZ) R16B(MT_DEF,D,CC,MSZ,TSZ)
#define MT_CC(CC,MSZ,TSZ) R16(MT_DST,CC,MSZ,TSZ)
C16(MT_CC,32,32) C16(MT_CC,32,64) C16(MT_CC,64,32) C16(MT_CC,64,64)
#define MT_E(S,D,CC,MSZ,TSZ) p_mtj_##MSZ##_##TSZ##_##CC##_##D##_##S,
#define MT_ROW(D,CC,MSZ,TSZ) { R16B(MT_E,D,CC,MSZ,TSZ) },
#define MT_PLANE(CC,MSZ,TSZ) { R16(MT_ROW,CC,MSZ,TSZ) },
#define MT_TEST(MSZ,TSZ) { C16(MT_PLANE,MSZ,TSZ) }
#define MT_MOVE(MSZ) { MT_TEST(MSZ,32), MT_TEST(MSZ,64) }
const PFn t_mtj[2][2][16][16][16] = { MT_MOVE(32),MT_MOVE(64) };
