// SPDX-License-Identifier: GPL-3.0-or-later
// Fallback when the Blink interpreter is not in this build. The real
// implementation lives in engine/blink/myiosdeck_driver.c (libblink_ios.a).

#include "interp_engine.h"

#include <stdio.h>
#include <string.h>

#if __has_include("blink_version.h")
#include "blink_version.h"
#endif

#if !MYIOSDECK_WITH_BLINK
bool mid_interp_linked(void) { return false; }
const char *mid_interp_version(void) { return "none"; }
bool mid_interp_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out) {
    (void)elf; (void)len; (void)argc; (void)argv;
    memset(out, 0, sizeof *out);
    snprintf(out->error, sizeof out->error, "This build was made without the interpreter");
    return false;
}
#endif
