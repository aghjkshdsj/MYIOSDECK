// SPDX-License-Identifier: GPL-3.0-or-later
#include "fxr_bridge.h"

#include <stdlib.h>
#include <string.h>

#include "../../../engine/fxr/fxi.h"   // FXR's API, renamed to fxr_* (fxr_rename.h)

const char *mid_fxr_version(void) { return fxr_version(); }

bool mid_fxr_pinned(void) {
#if defined(__aarch64__) && defined(__has_attribute) && __has_attribute(preserve_none)
    return true;
#else
    return false;
#endif
}

bool mid_fxr_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out) {
    fxi_result *r = calloc(1, sizeof *r);
    int ok = fxr_run_elf(elf, len, argc, argv, r);
    memset(out, 0, sizeof *out);
    out->ok = ok != 0;
    out->exit_code = r->exit_code;
    out->seconds = r->seconds;
    out->syscalls = r->syscalls;
    size_t n = r->output_len < sizeof out->output - 1 ? r->output_len : sizeof out->output - 1;
    memcpy(out->output, r->output, n);
    out->output_len = n;
    strncpy(out->error, r->error, sizeof out->error - 1);
    free(r);
    return out->ok;
}
