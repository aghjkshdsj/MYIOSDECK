// SPDX-License-Identifier: GPL-3.0-or-later
// GOG offline installers (Inno Setup: setup_<game>.exe + setup_<game>-N.bin) unpacked on the
// phone. engine/innoextract/myiosdeck_inno.cpp implements this with innoextract (zlib license);
// inno_stub.c answers when this build has no unpacker.
#ifndef MYIOSDECK_INNO_UNPACK_H
#define MYIOSDECK_INNO_UNPACK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Shared with the unpacker while it runs: it writes done/total (bytes of game files), the
/// caller may read them at any time and set cancel to stop it.
typedef struct {
    volatile uint64_t done;
    volatile uint64_t total;
    volatile int32_t cancel;
} mid_inno_progress;

bool mid_inno_linked(void);
/// "innoextract <commit>" or "none".
const char *mid_inno_version(void);

/// Reads the installer's headers. Returns JSON (free with mid_inno_free):
/// {"ok", "error", "app_name", "app_versioned_name", "default_dir_name", "publisher",
///  "inno_version", "gog_id", "embedded_data", "rar_data", "encrypted", "app_files", "app_size",
///  "total_size", "languages": [...], "parts": [{"name", "present"}]}
/// parts lists every setup-N.bin the data is in, in order, and whether it is next to the .exe.
char *mid_inno_inspect(const char *setup_exe);

/// Unpacks the files the installer puts into the game folder ({app}) into out_dir/app, like
/// `innoextract -e -m -I /app --default-language <language> -d out_dir`. Blocks until done.
/// Returns JSON (free with mid_inno_free): {"ok", "error", "cancelled", "warnings", "errors",
/// "done", "total", "log": [...]}.
char *mid_inno_extract(const char *setup_exe, const char *out_dir, const char *language,
                       mid_inno_progress *progress);

void mid_inno_free(char *json);

#ifdef __cplusplus
}
#endif

#endif
