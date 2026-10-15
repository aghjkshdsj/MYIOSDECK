// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// Integration glue; not part of the independently authored package.
#include "../Native/wine_host.h"
#include "../Native/jit_core.h"
#include "b_internal.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wine_version.h"

static _Atomic int integration_booted;
extern void wineserver_kill_programs(int status);
extern void mid_track_b_start_log_tail(const char *path);
int64_t fex_get_jit_write_offset(void) { return mid_jit_pool_write_offset(); }
// The replacement writes the shared log directly, so no stdout pipe is needed.
void mid_capture_stdio(void) {}
bool mid_wine_linked(void) { return true; }
int mid_wine_running(void) { return wb_process_is_running(); }
int mid_wine_session_reusable(void) { return 0; }
int mid_hidpad_gate(void) {
#ifdef MYIOSDECK_HIDPAD_GATE
    return 1;
#else
    return 0;
#endif
}
int mid_wine_force_quit(void) {
    if (!wb_process_is_running()) return 0;
    wineserver_kill_programs(1);
    return 1;
}
void mid_wine_seed_prefix(const char *prefix) { wb_seed_prefix(prefix); }
bool mid_wine_boot(const char *prefix, const char *exe, const char *args, char *error, size_t capacity) {
    if (atomic_exchange(&integration_booted, 1)) {
        snprintf(error, capacity, "Restart MYIOSDECK before starting another Windows session.");
        return false;
    }
    const char *noJIT = getenv("WINE_IOS_NOJIT");
    if (!noJIT || strcmp(noJIT, "1")) {
        if (!mid_jit_pool_ready()) { snprintf(error, capacity, "Enable JIT before starting this program."); return false; }
        size_t left = mid_jit_pool_size() - mid_jit_pool_used();
        void *rx = mid_jit_pool_alloc(left);
        if (!rx || left < (256u << 20)) { snprintf(error, capacity, "The JIT pool has insufficient space."); return false; }
        char value[64];
        snprintf(value, sizeof value, "%llx", (unsigned long long)(uintptr_t)rx); setenv("WINE_IOS_JIT_RX", value, 1);
        snprintf(value, sizeof value, "%llx", (unsigned long long)((uintptr_t)rx + mid_jit_pool_write_offset())); setenv("WINE_IOS_JIT_RW", value, 1);
        snprintf(value, sizeof value, "%llx", (unsigned long long)left); setenv("WINE_IOS_JIT_SIZE", value, 1);
    } else {
        unsetenv("WINE_IOS_JIT_RX"); unsetenv("WINE_IOS_JIT_RW"); unsetenv("WINE_IOS_JIT_SIZE");
    }
    uint64_t base, size;
    if (mid_exe_window(&base, &size)) {
        char value[64]; snprintf(value, sizeof value, "%llx:%llx", (unsigned long long)base, (unsigned long long)size);
        setenv("WINE_IOS_EXE_WINDOW", value, 1);
    }
    setenv("MADEIRA_EXE", exe, 1);
    if (args && *args) setenv("MADEIRA_ARGS", args, 1); else unsetenv("MADEIRA_ARGS");
    wb_exit_status_reset();
    if (wb_server_start(prefix)) { snprintf(error, capacity, "The Windows server could not start."); return false; }
    for (int attempt = 0; attempt < 300 && !wb_server_is_ready(); ++attempt) usleep(20000);
    if (!wb_server_is_ready() || wb_process_start(prefix)) {
        wb_server_stop(); snprintf(error, capacity, "The Windows program could not start."); return false;
    }
    mid_track_b_start_log_tail(wb_log_path());
    return true;
}
void mid_display_set_layer(void *layer) { wb_display_set_layer(layer); }
void mid_display_screen_size(int *w, int *h) { wb_screen_size(w, h); }
void mid_display_layout(double x,double y,double w,double h,double rx,double ry,double rw,double rh) {
    wb_overlay_set_frame(x,y,w,h); wb_overlay_set_desktop_rect(rx,ry,rw,rh,1);
}
void mid_game_overlay_show(int shown, void *window) { wb_overlay_show(shown,window); }
int mid_game_picture_rect(int dw,int dh,int *x,int *y,int *w,int *h) { return wb_game_picture_rect(dw,dh,x,y,w,h); }
void mid_post_key(int vk,int down) { wb_input_key(vk,down); }
void mid_post_mouse_button(unsigned flags) { wb_input_pointer(0,0,flags,0); }
void mid_post_pointer(int x,int y,unsigned flags,int data) { wb_input_pointer(x,y,flags,(unsigned)data); }
void mid_post_touch(int phase,int x,int y) { wb_input_touch(phase,x,y); }
void mid_pad_set(int slot,int connected,uint16_t buttons,uint8_t lt,uint8_t rt,int16_t lx,int16_t ly,int16_t rx,int16_t ry) {
    struct winios_gamepad state = {0}; state.connected=connected != 0; state.buttons=buttons;
    state.left_trigger=lt; state.right_trigger=rt; state.lx=lx; state.ly=ly; state.rx=rx; state.ry=ry;
    winios_gamepad_set_state(slot, connected ? &state : NULL);
}
void mid_hidpad_set(int connected,uint16_t buttons,uint8_t lt,uint8_t rt,int16_t lx,int16_t ly,int16_t rx,int16_t ry) {
    struct winios_hidpad state = {0}; state.connected=connected != 0; state.buttons=buttons;
    if (lt>25) state.buttons |= WINIOS_HIDPAD_L2;
    if (rt>25) state.buttons |= WINIOS_HIDPAD_R2;
    state.battery=WINIOS_HIDPAD_BATTERY_UNKNOWN;
    state.left_trigger=lt; state.right_trigger=rt; state.lx=lx; state.ly=ly; state.rx=rx; state.ry=ry;
    winios_hidpad_set_state(connected ? &state : NULL);
}
