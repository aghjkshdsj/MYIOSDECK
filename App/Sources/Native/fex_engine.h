// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK engine bridge: FEXCore (x86/x86-64 -> ARM64 JIT) embedded in the app.
//
// Stage 1 runs x86-64 Linux programs on a small built-in syscall layer (enough
// for the self-test and the benchmarks). Stage 2 hands FEXCore to Wine ARM64EC
// as xtajit64.dll, which is how Windows games and Valve's Steam client run.

#ifndef MYIOSDECK_FEX_ENGINE_H
#define MYIOSDECK_FEX_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Speed/accuracy presets, named like the SteamOS ARM Port's `konkr-game`
/// presets so they mean the same thing everywhere.
typedef enum {
    MID_PRESET_COMPAT = 0,  ///< strict TSO incl. vector + memcpy, split locks: for games that crash
    MID_PRESET_FAST = 1,    ///< TSO on, x87 reduced precision: safe for most games (default)
    MID_PRESET_FASTEST = 2, ///< TSO off: biggest CPU win, may break multithreaded games
} mid_preset;

typedef struct {
    mid_preset preset;
    bool multiblock;      ///< compile multiple x86 blocks per JIT unit (faster, default on)
    int max_inst;         ///< max x86 instructions per block (FEX default 5000)
} mid_engine_config;

typedef struct {
    bool ok;               ///< ran to exit(); false on load/translate failure
    int64_t exit_code;
    double seconds;        ///< host wall time including translation
    uint64_t syscalls;
    size_t pool_used_before, pool_used_after;
    char output[8192];     ///< what the guest wrote to stdout/stderr
    size_t output_len;
    char error[256];
} mid_run_result;

/// True when this build links FEXCore (false in the no-engine fallback build).
bool mid_engine_linked(void);
/// FEX fork commit this build was made from (from the CI), or "none".
const char *mid_engine_version(void);

/// Create the FEXCore context. Requires the JIT pool (mid_jit_pool_create).
/// Config changes need a re-init, which costs a little pool space.
bool mid_engine_init(const mid_engine_config *cfg, char *err, size_t errlen);
bool mid_engine_ready(void);
void mid_engine_shutdown(void);

/// Load a static-PIE x86-64 Linux ELF from memory and run it to exit.
/// argv[0] should be the program name. Blocks the caller; runs on its own
/// 16 MB-stack thread. Serialised: one guest at a time in stage 1.
bool mid_engine_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv,
                        mid_run_result *out);

#ifdef __cplusplus
}
#endif

#endif
