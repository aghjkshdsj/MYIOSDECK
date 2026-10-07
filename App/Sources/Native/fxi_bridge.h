// SPDX-License-Identifier: GPL-3.0-or-later
// FXI (engine/fxi, docs/FAST_INTERPRETER.md): MYIOSDECK's own fast x86-64
// interpreter. No executable memory, so it always works, with or without JIT.

#ifndef MYIOSDECK_FXI_BRIDGE_H
#define MYIOSDECK_FXI_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fex_engine.h" // mid_run_result

#ifdef __cplusplus
extern "C" {
#endif

/// Same contract as mid_engine_run_elf, interpreted by FXI. Serialised.
bool mid_fxi_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out);
const char *mid_fxi_version(void);

#ifdef __cplusplus
}
#endif

#endif
