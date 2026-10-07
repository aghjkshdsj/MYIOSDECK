// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ASTRA_H
#define ASTRA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int ok;
    long long exit_code;
    double seconds;
    unsigned long long syscalls, blocks;
    char output[8192];
    size_t output_len;
    char error[256];
} astra_result;
int astra_run_elf(const uint8_t *elf, size_t len, int argc,
                  const char *const *argv, astra_result *out);
const char *astra_version(void);
#ifdef __cplusplus
}
#endif
#endif
