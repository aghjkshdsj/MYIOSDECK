// SPDX-License-Identifier: GPL-3.0-or-later
// GPTA: loop-step superinstructions, kind sicr (see gpta_step.inc).

#include "gpta_pin.h"
#include "gpta_step.inc"

STEP_DEFS(SICR)
const PFn t_sicr[2][2][10][16][16] = STEP_TABLE(sicr);
