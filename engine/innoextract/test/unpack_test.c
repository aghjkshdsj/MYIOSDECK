// SPDX-License-Identifier: GPL-3.0-or-later
// CI driver for the app's unpacker (inno_unpack.h), built for the Linux runner:
//   unpack_test inspect <setup.exe>
//   unpack_test extract <setup.exe> <out_dir> [language]
// Prints the JSON the app gets; exits 0 when it says "ok":true.

#include "inno_unpack.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char *json = NULL;
    mid_inno_progress progress = { 0, 0, 0 };
    if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        json = mid_inno_inspect(argv[2]);
    } else if ((argc == 4 || argc == 5) && strcmp(argv[1], "extract") == 0) {
        json = mid_inno_extract(argv[2], argv[3], argc == 5 ? argv[4] : "", &progress);
        fprintf(stderr, "progress: %llu / %llu bytes\n", (unsigned long long)progress.done,
                (unsigned long long)progress.total);
    } else {
        fprintf(stderr, "usage: %s inspect <setup.exe> | extract <setup.exe> <out_dir> [language]\n", argv[0]);
        return 2;
    }
    if (!json) return 1;
    puts(json);
    int ok = strncmp(json, "{\"ok\":true", 10) == 0;
    mid_inno_free(json);
    return ok ? 0 : 1;
}
