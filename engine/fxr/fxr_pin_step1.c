// SPDX-License-Identifier: GPL-3.0-or-later
// FXR: loop-step superinstructions, kind sic (see fxr_step.inc).

#include "fxr_pin.h"
#include "fxr_step.inc"

STEP_DEFS(SIC)
const PFn t_sic[2][2][10][16][16] = STEP_TABLE(sic);
