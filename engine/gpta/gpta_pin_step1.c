// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA: loop-step superinstructions, kind sic (see gpta_step.inc).

#include "gpta_pin.h"
#include "gpta_step.inc"

STEP_DEFS(SIC)
const PFn t_sic[2][2][10][16][16] = STEP_TABLE(sic);
