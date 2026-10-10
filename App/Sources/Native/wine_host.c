// SPDX-License-Identifier: GPL-3.0-or-later
#include "wine_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jit_core.h"

#include <fcntl.h>
#include <pthread.h>

#if __has_include("wine_version.h")
#include "wine_version.h"
#endif

static void *stdio_reader(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096], line[1024];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || used == sizeof line - 1) {
                line[used] = 0;
                if (used) mid_log("[stdio] %s", line);
                used = 0;
            } else {
                line[used++] = buf[i];
            }
        }
    }
    return NULL;
}

void mid_capture_stdio(void) {
    static int done;
    if (done) return;
    int p[2];
    if (pipe(p) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(p[1], 1);
    dup2(p[1], 2);
    close(p[1]);
    pthread_t t;
    if (pthread_create(&t, NULL, stdio_reader, (void *)(intptr_t)p[0]) == 0) pthread_detach(t);
    done = 1;
}

/* Madeira's Wine bridge publishes this to xtajit64.dll (FEX inside Wine) so its
 * own FEXCore copy writes code through the pool's RW alias. */
int64_t fex_get_jit_write_offset(void) { return mid_jit_pool_write_offset(); }

/* Madeira's bridge points Wine's stdout/stderr (and wineserver's log) at
 * Documents/madeira-log.txt. Follow that file and copy its lines into the
 * MYIOSDECK log so one log has everything. */
static void *wine_log_follower(void *arg) {
    char *path = arg;
    int fd = -1;
    for (int i = 0; i < 200 && fd < 0; i++) {
        fd = open(path, O_RDONLY);
        if (fd < 0) usleep(50000);
    }
    if (fd < 0) {
        mid_log("[wine] no Wine log at %s", path);
        free(path);
        return NULL;
    }
    char buf[8192], line[1024];
    size_t used = 0;
    unsigned idle = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            usleep(100000);
            if (++idle > 600 && !mid_wine_running()) break; /* 60 s quiet after exit */
            continue;
        }
        idle = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || used == sizeof line - 1) {
                line[used] = 0;
                if (used) mid_log("[winelog] %s", line);
                used = 0;
            } else {
                line[used++] = buf[i];
            }
        }
    }
    close(fd);
    free(path);
    return NULL;
}

static void follow_wine_log(const char *prefix) {
    /* The prefix lives in Documents; the log sits next to it. */
    char *path = malloc(1024);
    snprintf(path, 1024, "%s", prefix);
    char *slash = strrchr(path, '/');
    if (!slash) { free(path); return; }
    snprintf(slash, 1024 - (size_t)(slash - path), "/madeira-log.txt");
    unlink(path); /* start fresh for this session */
    pthread_t t;
    if (pthread_create(&t, NULL, wine_log_follower, path) == 0) pthread_detach(t);
    else free(path);
}

#if MYIOSDECK_WITH_WINE

#include "WineProcessBridge.h"
#include "WineServerBridge.h"

static int g_booted;

/* Madeira's IOSDisplayShim.m and Winios.m (staged app sources). */
extern void madeira_display_set_layer(void *layer);
extern void winios_screen_size(int *w, int *h);
extern void winios_set_compositor_frame(double x, double y, double w, double h);
extern void winios_set_desktop_rect(double x, double y, double w, double h, int set);

#include "Winios/WiniosGamepad.h"

void mid_pad_set(int slot, int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                 int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    if (slot < 0 || slot >= WINIOS_GAMEPAD_MAX) return;
    if (!connected) { winios_gamepad_set_state(slot, NULL); return; }
    struct winios_gamepad s = {0};
    s.connected = 1;
    s.buttons = buttons;
    s.left_trigger = lt; s.right_trigger = rt;
    s.lx = lx; s.ly = ly; s.rx = rx; s.ry = ry;
    winios_gamepad_set_state(slot, &s);
}

void mid_hidpad_set(int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                    int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    if (!connected) { winios_hidpad_set_state(NULL); return; }
    struct winios_hidpad h = {0};
    h.connected = 1;
    h.buttons = buttons;
    if (lt > 25) h.buttons |= WINIOS_HIDPAD_L2;
    if (rt > 25) h.buttons |= WINIOS_HIDPAD_R2;
    h.left_trigger = lt; h.right_trigger = rt;
    h.lx = lx; h.ly = ly; h.rx = rx; h.ry = ry;
    h.battery = WINIOS_HIDPAD_BATTERY_UNKNOWN;
    winios_hidpad_set_state(&h);
}

extern void winios_post_key(int vk, int down);
extern void winios_pointer(int x, int y, unsigned int flags, unsigned int data);
void mid_post_key(int vk, int down) { winios_post_key(vk, down); }
void mid_post_mouse_button(unsigned int flags) { winios_pointer(0, 0, flags, 0); }
void mid_post_pointer(int x, int y, unsigned int flags, int data) { winios_pointer(x, y, flags, (unsigned int)data); }
extern void winios_post_touch_down(int x, int y);
extern void winios_post_touch_move(int x, int y);
extern void winios_post_touch_up(int x, int y);
void mid_post_touch(int phase, int x, int y) {
    if (phase == 0) winios_post_touch_down(x, y);
    else if (phase == 1) winios_post_touch_move(x, y);
    else winios_post_touch_up(x, y);
}
extern void winios_game_overlay_show(int shown, void *window);   /* Winios.m, engine/wine/stage-app.sh */
void mid_game_overlay_show(int shown, void *window) { winios_game_overlay_show(shown, window); }

extern void madeira_seed_prefix_if_needed(const char *prefix_path);
void mid_wine_seed_prefix(const char *prefix) { madeira_seed_prefix_if_needed(prefix); }

void mid_display_set_layer(void *metal_layer) { madeira_display_set_layer(metal_layer); }
void mid_display_screen_size(int *w, int *h) { winios_screen_size(w, h); }
void mid_display_layout(double fx, double fy, double fw, double fh,
                        double rx, double ry, double rw, double rh) {
    winios_set_compositor_frame(fx, fy, fw, fh);
    winios_set_desktop_rect(rx, ry, rw, rh, 1);
}

bool mid_wine_linked(void) { return true; }
int mid_wine_running(void) { return wine_process_is_running(); }

bool mid_wine_boot(const char *prefix, const char *exe, const char *args, char *err, size_t errlen) {
    if (g_booted) {
        snprintf(err, errlen, "Wine already ran in this app session; restart MYIOSDECK to start another program");
        return false;
    }
    /* No-JIT (docs/NO_JIT_WINDOWS.md step B): the caller set WINE_IOS_NOJIT=1 and
     * MYIOSDECK_PE_DIR; Wine maps its DLLs from the signed dylibs there and gets no pool. */
    const char *nojit = getenv("WINE_IOS_NOJIT");
    bool no_jit = nojit && *nojit == '1';
    char buf[32];
    void *rx = NULL;
    size_t left = 0;
    if (no_jit) {
        unsetenv("WINE_IOS_JIT_RX");
        unsetenv("WINE_IOS_JIT_RW");
        unsetenv("WINE_IOS_JIT_SIZE");
    } else {
        if (!mid_jit_pool_ready()) {
            snprintf(err, errlen, "Wine needs JIT: enable it first");
            return false;
        }
        /* Give Wine everything left in the pool after the stage-1 engine's code.
         * Its loader copies PE code sections here and runs them from the RX view. */
        size_t used = (mid_jit_pool_used() + 0x3FFFu) & ~(size_t)0x3FFF;
        left = mid_jit_pool_size() > used ? mid_jit_pool_size() - used : 0;
        if (left < (256u << 20)) {
            snprintf(err, errlen, "Only %zu MB of JIT pool left; set the pool to 1 GB and enable JIT again", left >> 20);
            return false;
        }
        rx = mid_jit_pool_alloc(left);
        if (!rx) {
            snprintf(err, errlen, "JIT pool allocation for Wine failed");
            return false;
        }
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)(uintptr_t)rx);
        setenv("WINE_IOS_JIT_RX", buf, 1);
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)((uintptr_t)rx + (uintptr_t)mid_jit_pool_write_offset()));
        setenv("WINE_IOS_JIT_RW", buf, 1);
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)left);
        setenv("WINE_IOS_JIT_SIZE", buf, 1);
    }
    uint64_t wb, ws;
    if (mid_exe_window(&wb, &ws)) {
        snprintf(buf, sizeof buf, "%llx:%llx", (unsigned long long)wb, (unsigned long long)ws);
        setenv("WINE_IOS_EXE_WINDOW", buf, 1);
    } else {
        mid_log("[wine] WARNING: 0x140000000 window not held; fixed-base .exe images will be displaced");
    }
    setenv("MADEIRA_EXE", exe, 1);
    if (args && *args) setenv("MADEIRA_ARGS", args, 1); else unsetenv("MADEIRA_ARGS");
    mid_capture_stdio();
    follow_wine_log(prefix);
    if (no_jit)
        mid_log("[wine] NO JIT: Wine maps its DLLs from the signed dylibs in %s (cs_flags=0x%x)",
                getenv("MYIOSDECK_PE_DIR"), mid_cs_flags());
    else
        mid_log("[wine] JIT slice for Wine: RX=%p size=%zu MB", rx, left >> 20);

    if (wineserver_start(prefix) != 0) {
        snprintf(err, errlen, "wineserver failed to start");
        return false;
    }
    for (int i = 0; i < 300 && !wineserver_is_ready(); i++) usleep(20000);
    if (!wineserver_is_ready()) {
        snprintf(err, errlen, "wineserver did not become ready in 6 s");
        return false;
    }
    if (wine_process_start(prefix) != 0) {
        snprintf(err, errlen, "Wine process failed to start");
        return false;
    }
    g_booted = 1;
    mid_log("[wine] started %s", exe);
    return true;
}

#else

void mid_wine_seed_prefix(const char *prefix) { (void)prefix; }
void mid_post_key(int vk, int down) { (void)vk; (void)down; }
void mid_post_mouse_button(unsigned int flags) { (void)flags; }
void mid_post_pointer(int x, int y, unsigned int flags, int data) { (void)x; (void)y; (void)flags; (void)data; }
void mid_post_touch(int phase, int x, int y) { (void)phase; (void)x; (void)y; }
void mid_game_overlay_show(int shown, void *window) { (void)shown; (void)window; }
void mid_pad_set(int slot, int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                 int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    (void)slot; (void)connected; (void)buttons; (void)lt; (void)rt; (void)lx; (void)ly; (void)rx; (void)ry;
}
void mid_hidpad_set(int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                    int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    (void)connected; (void)buttons; (void)lt; (void)rt; (void)lx; (void)ly; (void)rx; (void)ry;
}
void mid_display_set_layer(void *metal_layer) { (void)metal_layer; }
void mid_display_screen_size(int *w, int *h) { *w = 1024; *h = 768; }
void mid_display_layout(double fx, double fy, double fw, double fh,
                        double rx, double ry, double rw, double rh) {
    (void)fx; (void)fy; (void)fw; (void)fh; (void)rx; (void)ry; (void)rw; (void)rh;
}

bool mid_wine_linked(void) { return false; }
int mid_wine_running(void) { return 0; }
int wine_crash_exit_status(uint32_t *status) { (void)status; return 0; }
bool mid_wine_boot(const char *prefix, const char *exe, const char *args, char *err, size_t errlen) {
    (void)prefix; (void)exe; (void)args;
    snprintf(err, errlen, "This build does not include Wine yet (stage 2 in progress)");
    return false;
}

#endif
