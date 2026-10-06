// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT path: x86-64 programs run in Blink's interpreter (jart/blink, ISC).
// Needs no executable memory, so it works without StikDebug (and is the only
// way x86 code could ever run in an App Store build), at a large speed cost.

#ifndef MYIOSDECK_INTERP_ENGINE_H
#define MYIOSDECK_INTERP_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fex_engine.h" // mid_run_result

#ifdef __cplusplus
extern "C" {
#endif

/// True when this build links the Blink interpreter.
bool mid_interp_linked(void);
/// Blink commit this build was made from, or "none".
const char *mid_interp_version(void);
/// Same contract as mid_engine_run_elf, but interpreted. Serialised.
bool mid_interp_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out);

#ifdef __cplusplus
}
#endif

#endif
