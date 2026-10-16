// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part A: the one log file of an app run. Every line is
// "[HH:MM:SS.mmm] [tag] message", written whole under a mutex, and also sent to os_log and to
// the unix side's wine_ui_log. Tags name the part: wb-server, wb-launch, wb-prefix,
// wb-display, wb-windows, wb-input, wb-config, phase.

#ifndef WB_LOG_H
#define WB_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void wb_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void wb_logv(const char *tag, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));

/// Point `fd` (STDOUT_FILENO / STDERR_FILENO) at the log file, opened for append, with dup2.
/// 0 on success, -1 on failure.
int wb_log_redirect_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif
