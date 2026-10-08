// SPDX-License-Identifier: GPL-3.0-or-later
// Until stage 3 (DXMT: Direct3D on Metal) is linked, Wine's iOS loader still
// references DXMT's unix call tables. Answer every call with
// STATUS_NOT_IMPLEMENTED so Direct3D fails cleanly instead of jumping to null.

#include <stdint.h>

#if __has_include("wine_version.h")
#include "wine_version.h"
#endif
#if __has_include("dxmt_version.h")
#include "dxmt_version.h"
#endif

#if MYIOSDECK_WITH_WINE && !MYIOSDECK_WITH_DXMT

#define STUB_ENTRIES 256

static long dxmt_not_linked(void *args) {
    (void)args;
    return (long)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
}

#define FILL [0 ... STUB_ENTRIES - 1] = (const void *)dxmt_not_linked
const void *dxmt_winemetal_unix_call_funcs[STUB_ENTRIES] = { FILL };
const void *dxmt_winemetal_unix_call_wow64_funcs[STUB_ENTRIES] = { FILL };
const void *const dxmt_d3d9_unix_call_wow64_funcs[STUB_ENTRIES] = { FILL };

/* Diagnostics hook DXMT provides (weak-imported by ntdll's exception thread). */
const char *madeira_wmt_released_class(uintptr_t addr, uint64_t *ago) {
    (void)addr;
    (void)ago;
    return 0;
}

/* DXMT's present counter (winemetal_unix.c); Winios/WiniosGL.m adds its GL presents to it. */
uint64_t madeira_get_present_count(void) {
    return 0;
}

#endif
