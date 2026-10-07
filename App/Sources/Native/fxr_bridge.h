// SPDX-License-Identifier: GPL-3.0-or-later
// FXR (engine/fxr): FXI with the guest registers pinned in host registers. Experimental: x86-64
// Linux programs only (Windows programs stay on FXI). No executable memory, like FXI.

#ifndef MYIOSDECK_FXR_BRIDGE_H
#define MYIOSDECK_FXR_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fex_engine.h" // mid_run_result

#ifdef __cplusplus
extern "C" {
#endif

/// Same contract as mid_fxi_run_elf, interpreted by FXR. Serialised.
bool mid_fxr_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out);
const char *mid_fxr_version(void);
/// True when this build pins the guest registers (clang's preserve_none on ARM64).
bool mid_fxr_pinned(void);

#ifdef __cplusplus
}
#endif

#endif
