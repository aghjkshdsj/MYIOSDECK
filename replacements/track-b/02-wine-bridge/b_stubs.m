// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_internal.h"
#import <CoreFoundation/CoreFoundation.h>

const char wine_build[] = "wine-11-ios";
CFTypeRef IOPSCopyPowerSourcesInfo(void) { return NULL; }
CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob) { (void)blob; return NULL; }
CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps) { (void)blob; (void)ps; return NULL; }
int winios_direct_cursor_wanted(void) { return 0; }
void winios_cursor_set(unsigned int id, int w, int h, int x, int y, const void *p) {
    (void)id; (void)w; (void)h; (void)x; (void)y; (void)p;
}
void winios_cursor_show(int show) { (void)show; }
void winios_direct_cursor_set(unsigned int id, int w, int h, int x, int y, const void *p) {
    (void)id; (void)w; (void)h; (void)x; (void)y; (void)p;
}
void winios_direct_cursor_show(int show) { (void)show; }
void winios_direct_cursor_pos(int x, int y) { (void)x; (void)y; }
