// SPDX-License-Identifier: GPL-3.0-or-later
// Fallback when the GOG installer unpacker is not in this build. The real implementation lives
// in engine/innoextract/myiosdeck_inno.cpp (libinno_ios.a).

#include "inno_unpack.h"

#include <stdlib.h>
#include <string.h>

#if __has_include("inno_version.h")
#include "inno_version.h"
#endif

#if !MYIOSDECK_WITH_INNO
static char *mid_inno_unavailable(void) {
    return strdup("{\"ok\":false,\"error\":\"This build was made without the installer unpacker.\"}");
}
bool mid_inno_linked(void) { return false; }
const char *mid_inno_version(void) { return "none"; }
char *mid_inno_inspect(const char *setup_exe) { (void)setup_exe; return mid_inno_unavailable(); }
char *mid_inno_extract(const char *setup_exe, const char *out_dir, const char *language, mid_inno_progress *progress) {
    (void)setup_exe; (void)out_dir; (void)language; (void)progress;
    return mid_inno_unavailable();
}
void mid_inno_free(char *json) { free(json); }
#endif
