// SPDX-License-Identifier: GPL-3.0-or-later
// fxi CLI (CI and desktop testing): fxi <guest.elf> [args...]
// Runs the guest, echoes its output, prints a stats line, exits with its code.

#include <stdio.h>
#include <stdlib.h>

#include "fxi.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fxi <guest.elf> [args...]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 2; }
    fclose(f);

    static fxi_result r;
    fxi_set_echo_fd(1);
    int ok = fxi_run_elf(buf, (size_t)len, argc - 1, (const char *const *)(argv + 1), &r);
    fflush(stdout);
    fprintf(stderr, "[fxi] %s ok=%d exit=%lld %.3f s, %llu blocks, %llu syscalls%s%s\n", fxi_version(), ok,
            r.exit_code, r.seconds, r.blocks, r.syscalls, r.error[0] ? " error: " : "", r.error);
    return ok ? (int)r.exit_code : 99;
}
