// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Host-test stand-ins for the parts of spec 02 that need the app: the log (to stderr when
// WB_TEST_VERBOSE is set), the app's folders (WB_TEST_DOCS, WB_TEST_BUNDLE), and the unix
// side's input functions (recorded so tests can read them back).

#include "wb_internal.h"
#include "wb_log.h"
#include "host_stubs.h"

#include <stdio.h>
#include <stdlib.h>

void wb_logv(const char *tag, const char *fmt, va_list ap)
{
    if (!getenv("WB_TEST_VERBOSE")) return;
    fprintf(stderr, "[%s] ", tag ? tag : "wb");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void wb_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wb_logv(tag, fmt, ap);
    va_end(ap);
}

const char *wb_documents_dir(void)
{
    const char *d = getenv("WB_TEST_DOCS");
    return d ? d : "/tmp";
}

const char *wb_bundle_dir(void)
{
    const char *d = getenv("WB_TEST_BUNDLE");
    return d ? d : "/tmp";
}

const char *wb_app_support_dir(void)
{
    return NULL;
}

struct stub_input_call stub_calls[STUB_MAX_CALLS];
int stub_call_count;

void winios_drv_post_key(unsigned short vk, unsigned int flags)
{
    if (stub_call_count < STUB_MAX_CALLS)
        stub_calls[stub_call_count++] = (struct stub_input_call){ 'K', vk, 0, flags, 0 };
}

void winios_drv_post_mouse(int x, int y, unsigned int flags, unsigned int mouse_data, HWND hwnd)
{
    (void)hwnd;
    if (stub_call_count < STUB_MAX_CALLS)
        stub_calls[stub_call_count++] = (struct stub_input_call){ 'M', x, y, flags, mouse_data };
}

void wb_windows_forget(HWND hwnd)
{
    (void)hwnd;
}
