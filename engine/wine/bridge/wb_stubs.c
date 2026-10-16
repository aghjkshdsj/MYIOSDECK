// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part F: the contract symbols that only need to exist (version string, power
// sources, cursors), and the four thread-local process-exit variables.

#include <CoreFoundation/CoreFoundation.h>

#include "wb_internal.h"

/* Defined with exactly the contract's types; the contract header's extern declarations,
 * included above, make the compiler check that. */
_Thread_local jmp_buf wine_ios_exit_jmpbuf;
_Thread_local volatile int wine_ios_exit_code;
_Thread_local pthread_t wine_ios_main_thread;
_Thread_local int wine_ios_exit_initialized;

const char wine_build[] = "wine-11-ios";

/* iOS has no IOKit power-source API. */
CFTypeRef IOPSCopyPowerSourcesInfo(void)
{
    return NULL;
}

CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob)
{
    (void)blob;
    return NULL;
}

CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps)
{
    (void)blob;
    (void)ps;
    return NULL;
}

/* Cursors belong to desktop sessions, which are out of scope. */
void winios_cursor_set(unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra)
{
    (void)id; (void)w; (void)h; (void)hot_x; (void)hot_y; (void)bgra;
}

void winios_cursor_show(int show)
{
    (void)show;
}

int winios_direct_cursor_wanted(void)
{
    return 0;
}

void winios_direct_cursor_set(unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra)
{
    (void)id; (void)w; (void)h; (void)hot_x; (void)hot_y; (void)bgra;
}

void winios_direct_cursor_show(int show)
{
    (void)show;
}

void winios_direct_cursor_pos(int x, int y)
{
    (void)x;
    (void)y;
}
