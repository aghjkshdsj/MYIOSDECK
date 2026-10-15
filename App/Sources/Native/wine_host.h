// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The C functions the Swift app calls to reach the Windows side (Wine running in-process).
// Swift sees these through the app's bridging header. When the build has no Wine, every
// function is a harmless no-op (getters return 0 / false, screen size returns 1024x768).
// Specs 01 and 02 refer to this file. Its owner keeps the names and signatures fixed.

#ifndef MYIOSDECK_WINE_HOST_H
#define MYIOSDECK_WINE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- session (spec 02 implements what these call) ---- */
bool mid_wine_linked(void);
bool mid_wine_boot(const char *prefix, const char *exe, const char *args, char *err, size_t errlen);
void mid_wine_seed_prefix(const char *prefix);
int  mid_wine_running(void);
int  wine_crash_exit_status(uint32_t *status);
void mid_capture_stdio(void);

/* ---- display (spec 02) ---- */
void mid_display_set_layer(void *metal_layer);
void mid_display_layout(double fx, double fy, double fw, double fh,
                        double rx, double ry, double rw, double rh);
/// The guest's virtual monitor in pixels (1024x768 unless a program changes it).
void mid_display_screen_size(int *w, int *h);
/// The desktop rectangle the game view shows, in guest desktop pixels: the client area of the
/// window the game presents in. dw, dh: the game view's drawable size. Returns 0 when there is
/// none (the view then shows the whole desktop). Main thread.
int  mid_game_picture_rect(int dw, int dh, int *x, int *y, int *w, int *h);
void mid_game_overlay_show(int shown, void *window);

/* ---- input (spec 01 calls these; spec 02 implements them) ---- */
/// One XInput controller slot (0-3). buttons: XINPUT_GAMEPAD_* bits. Triggers 0-255.
/// Sticks: full signed 16-bit range, +y is up. connected=0 unplugs the slot.
void mid_pad_set(int slot, int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                 int16_t lx, int16_t ly, int16_t rx, int16_t ry);
/// Player 1 as a virtual HID gamepad instead of XInput. Same units as mid_pad_set.
void mid_hidpad_set(int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                    int16_t lx, int16_t ly, int16_t rx, int16_t ry);
/// A Windows virtual-key press (down=1) or release (down=0).
void mid_post_key(int vk, int down);
/// A mouse button event: MOUSEEVENTF_* button flags only, no movement.
void mid_post_mouse_button(unsigned int flags);
/// Mouse input. flags: MOUSEEVENTF_* bits. With ABSOLUTE (0x8000), x and y are guest desktop
/// pixels; with MOVE (0x0001) alone they are a relative delta. data: wheel delta (+-120 per
/// notch) or XBUTTON number.
void mid_post_pointer(int x, int y, unsigned int flags, int data);
/// A finger acting as the left mouse button at guest desktop pixels. phase: 0 down, 1 move, 2 up.
void mid_post_touch(int phase, int x, int y);

/* ---- diagnostics ---- */
/// Ask the 32-bit CPU layer to log the program's system calls for this many seconds (called
/// on each new click, key or button press so a log shows what the input did).
void mid_wow_trace_calls(double seconds);

#ifdef __cplusplus
}
#endif

#endif
