// SPDX-License-Identifier: GPL-3.0-or-later
// FXI: fast x86-64 interpreter (docs/FAST_INTERPRETER.md). Runs x86-64 code
// with no executable memory: pre-decoded micro-op blocks, tail-call threaded
// dispatch, lazy flags. Portable C (clang); guest addresses are host addresses.

#ifndef FXI_H
#define FXI_H

#if FXI_I386
#include "fxi_i386.h"   // FXI32: fxi_* -> fx32_* (links next to FXI)
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ok;                     // ran to exit_group
    long long exit_code;
    double seconds;             // wall time, load to exit
    unsigned long long syscalls;
    unsigned long long blocks;  // basic blocks translated
    char output[8192];          // what the guest wrote to fd 1/2 (truncated)
    size_t output_len;
    char error[256];            // set when ok == 0 (e.g. an unimplemented opcode)
} fxi_result;

/// Load a static-PIE x86-64 Linux ELF and run it to exit. Serialised (one guest at a time).
int fxi_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, fxi_result *out);

/// "fxi <version>"; bumped with the instruction set.
const char *fxi_version(void);

/// When set, guest writes to fd 1/2 also go to this host fd (the CLI uses 1).
void fxi_set_echo_fd(int fd);

// ---- Windows mode (fxi_win.c): FXI as the x64 CPU of Wine's ARM64EC processes ----
typedef struct FxiCpu FxiCpu;
/// One per thread; all share the process's block cache. Layout: GPRs at 0x00 (x86 order,
/// slot 18 = GS base = TEB), rip at 0x98, xmm at 0xa0 (the transition glue relies on it).
FxiCpu *fxi_win_cpu_new(void);
/// Interpret from the CPU's rip until control reaches native ARM64EC code; returns that
/// address, or 0 on an error (fxi_win_error).
uint64_t fxi_win_run(FxiCpu *c);
const char *fxi_win_error(FxiCpu *c);
void fxi_win_set_teb(FxiCpu *c, uint64_t teb);   // GS base
uint64_t fxi_win_rip(FxiCpu *c);
/// Load an AMD64 CONTEXT into the CPU.
void fxi_win_load_context(FxiCpu *c, const void *amd64_context);
/// The CPU state as an AMD64 CONTEXT (0x4d0 bytes written) with the given rip.
void fxi_win_save_context(FxiCpu *c, void *amd64_context, uint64_t rip);

/// A CPU exception the x64 code raised (int3, ud2, divide error, ...): fxi_win_run returned 0
/// and this returns 1 with the exception (the CPU state is as before the instruction).
typedef struct { uint32_t code, flags, nparams; uint64_t info[2]; uint64_t rip; } fxi_win_exc;
int fxi_win_exception(FxiCpu *c, fxi_win_exc *e);
/// A host fault (SIGSEGV/SIGBUS) during fxi_win_run: the x64 instruction that faulted.
/// *is_fetch = 1 when it happened fetching code (an execute fault at that address).
uint64_t fxi_win_fault_rip(FxiCpu *c, int *is_fetch);
/// Diagnostics: the rips of the last block lookups (first-time control-flow edges), oldest first.
int fxi_win_trail(FxiCpu *c, uint64_t *rips, int max);
/// Diagnostics: how DF was last set (1 std, 2 popf, 3 context load, 0 never), *rip where.
int fxi_win_df_source(FxiCpu *c, uint64_t *rip);
/// Profiler sample, safe to call from another thread (racy by design): the rip of the last
/// memory-touching x64 instruction, and the lookup / native-exit / translated-block counters.
uint64_t fxi_win_profile(FxiCpu *c, uint64_t *lookups, uint64_t *exits, uint64_t *blocks);
/// Profiler: calls into native code per target since the previous call (racy by design).
int fxi_win_exit_counts(FxiCpu *c, uint64_t *targets, uint64_t *counts, int max);

#ifdef __cplusplus
}
#endif

#endif
