// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#ifndef B_INTERNAL_H
#define B_INTERNAL_H
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#else
typedef int BOOL;
#endif
typedef void *HWND;
typedef void *HCURSOR;
typedef unsigned int UINT;
typedef int INT;
typedef unsigned long DWORD;
typedef long WINELONG;
typedef struct { WINELONG left, top, right, bottom; } RECT;
struct window_rects;
struct window_surface;
struct winios_gamepad;
struct winios_hidpad;
struct winios_hidpad_output;
struct winios_gamepad_vibration;
#include "wine_bridge.h"
#include "wine_unix_contract.h"
#include "b_logic.h"

void wb_b_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void wb_b_redirect_log(int include_stdout);
void wb_b_link_farms(const char *prefix, const char *set);
void wb_b_mark_metal(HWND hwnd);
void winios_session_reset(void);
// Getters requested by spec F; their names are not fixed in the contract.
void wb_b_get_vibration(int index, struct winios_gamepad_vibration *out);
void wb_b_get_hid_output(struct winios_hidpad_output *out);
#endif
