// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The API spec 02 provides to the rest of the app: the app's own C glue (which implements
// interfaces/wine_host.h on top of these) and nothing else calls it. Plain C, callable from
// C, Objective-C and (through the bridging header) Swift. Spec 02 owns this file's
// implementation; the names and signatures are fixed.

#ifndef MYIOSDECK_WINE_BRIDGE_H
#define MYIOSDECK_WINE_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct winios_gamepad;   /* interfaces/wine_unix_contract.h, Section C */
struct winios_hidpad;

/* ---- session ---- */

/// Prepare the prefix (spec 02 "Prefix"), then start the wineserver thread. 0 on success,
/// -1 on failure, 0 at once if it already runs.
int  wb_server_start(const char *prefix);
int  wb_server_is_running(void);
/// 1 once the server has loaded the registry and entered its loop.
int  wb_server_is_ready(void);
/// Ask the server loop to end and wait for its thread.
void wb_server_stop(void);

/// Start the Windows program on its own thread (spec 02 "Launch"). The program and its
/// arguments come from the environment (MADEIRA_EXE, MADEIRA_ARGS, ...). The server must
/// be ready. 0 on success, -1 on failure, 0 at once if a program already runs.
int  wb_process_start(const char *prefix);
/// 1 from wb_process_start until the launch thread has finished (server stopped).
int  wb_process_is_running(void);
/// 1 and the NTSTATUS when the session's program ended with an error status (0xC0000000 or
/// above) since the last reset; else 0. (wine_host.h declares the same function as
/// wine_crash_exit_status; spec 02 provides both names.)
int  wb_crash_exit_status(uint32_t *status);
void wb_exit_status_reset(void);

/// Create the prefix from the bundled template if it is new, and repair it. Idempotent, cheap
/// after the first call. Safe before the server starts (and must be done before it starts).
void wb_seed_prefix(const char *prefix);

/// The log file every part of Wine writes to for this app run: <Documents>/wine-log.txt.
/// The app follows it and copies its lines into its own log.
const char *wb_log_path(void);

/* ---- display ---- */

/// The CAMetalLayer (passed as void *) that every Direct3D swapchain of a game session
/// presents into. Set once before the first swapchain; the layer outlives Wine.
void wb_display_set_layer(void *metal_layer);
/// The guest's virtual monitor size in pixels.
void wb_screen_size(int *w, int *h);

/// Where the game picture is: `frame` is the area the overlay view covers, in the coordinates
/// of the window it lives in; the desktop rect (x, y, w, h) is where the guest desktop is
/// drawn inside that area, in the overlay view's own points (set = 0 clears it: the desktop is
/// then aspect-fitted and centred in the frame). Any thread; applied on the main thread;
/// identical repeats do nothing.
void wb_overlay_set_frame(double x, double y, double w, double h);
void wb_overlay_set_desktop_rect(double x, double y, double w, double h, int set);
/// Show the overlay of a game session's GDI windows above the game view (shown = 1), moving
/// it into `window` (a UIWindow *) when given, or hide it (shown = 0). Main thread. No-op
/// before the overlay exists.
void wb_overlay_show(int shown, void *window);
/// The client rectangle, in desktop pixels, of the window the game presents in (spec 02
/// "Game picture"). dw x dh: the game view's drawable size. Returns 0 when there is none.
/// Main thread.
int  wb_game_picture_rect(int dw, int dh, int *x, int *y, int *w, int *h);

/* ---- input (any thread; delivered on a Wine thread, in order) ---- */

/// A key: virtual-key code, down = 1 press, 0 release.
void wb_input_key(int vk, int down);
/// Mouse: MOUSEEVENTF_* flags; with ABSOLUTE, x and y are desktop pixels; data is the wheel
/// delta or XBUTTON.
void wb_input_pointer(int x, int y, unsigned int flags, unsigned int data);
/// A finger as the left button at desktop pixels: phase 0 down, 1 move, 2 up.
void wb_input_touch(int phase, int x, int y);

/* ---- controllers (the stores the unix side reads; contract Section B and C) ---- */

/// Store XInput slot `index` (0-3). NULL or connected == 0: unplugged.
void winios_gamepad_set_state(int index, const struct winios_gamepad *state);
/// Store the HID gamepad. NULL or connected == 0: unplugged.
void winios_hidpad_set_state(const struct winios_hidpad *state);

#ifdef __cplusplus
}
#endif

#endif
