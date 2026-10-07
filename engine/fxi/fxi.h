// SPDX-License-Identifier: GPL-3.0-or-later
// FXI: fast x86-64 interpreter (docs/FAST_INTERPRETER.md). Runs x86-64 code
// with no executable memory: pre-decoded micro-op blocks, tail-call threaded
// dispatch, lazy flags. Portable C (clang); guest addresses are host addresses.

#ifndef FXI_H
#define FXI_H

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

#ifdef __cplusplus
}
#endif

#endif
