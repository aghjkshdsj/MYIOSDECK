// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT Windows code, milestone A spike (docs/NO_JIT_WINDOWS.md): load a Windows PE
// DLL (ARM64EC or ARM64) that CI wrapped in a signed dylib (engine/pedylib) and run
// it in place, with no executable memory of our own.

#ifndef MYIOSDECK_PE_DYLIB_H
#define MYIOSDECK_PE_DYLIB_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Runs the spike on every PE dylib in `dir` (the app's PE/ folder): load, fix up,
/// call the exports, then the benchmark kernels inside the DLL vs. the app's own
/// native build. Every step logs a `[pe-dylib]` line. `summary` gets one line per DLL.
/// Returns true when every DLL passed.
bool mid_pe_dylib_spike(const char *dir, char *summary, size_t summary_len);

#ifdef __cplusplus
}
#endif

#endif
