// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: loop-step superinstructions, kind sicr (see fxr_step.inc).

#include "fxr_pin.h"
#include "fxr_step.inc"

STEP_DEFS(SICR)
const PFn t_sicr[2][2][10][16][16] = STEP_TABLE(sicr);
