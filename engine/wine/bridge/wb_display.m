// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part D: the Direct3D presentation hook DXMT finds with
// dlsym(RTLD_DEFAULT, "macdrv_functions"), the layer every swapchain presents into, and the
// guest's screen size. Written from the table and behaviour in the spec only.

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include "wb_internal.h"
#include "wb_log.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

#define TAG "wb-display"

/* Nothing references these by name at link time: keep them through dead-code stripping and
 * in the export table. */
#define WB_EXPORT __attribute__((used, visibility("default")))

NSString *const wb_display_mode_notification = @"WBDisplayModeChanged";

/* Four pointers, as DXMT reads it. */
struct win_data {
    HWND hwnd;
    void *window;
    void *view;
    void *client_view;
};

/* Ten function pointers, in DXMT's order. Entries 1 and 4 stay NULL. */
struct wb_macdrv_functions {
    void (*init_display_devices)(int);
    struct win_data *(*get_win_data)(HWND);
    void (*release_win_data)(struct win_data *);
    void *(*get_cocoa_window)(HWND, int);
    void *(*create_metal_device)(void);
    void (*release_metal_device)(void *);
    void *(*view_create_metal_view)(void *, void *);
    void *(*view_get_metal_layer)(void *);
    void (*view_release_metal_view)(void *);
    void (*on_main_thread)(dispatch_block_t);
};

/* ---- the layer ---- */

static pthread_mutex_t g_layer_lock = PTHREAD_MUTEX_INITIALIZER;
static CALayer *g_layer;

void wb_display_set_layer(void *metal_layer)
{
    CALayer *layer = (__bridge CALayer *)metal_layer;

    pthread_mutex_lock(&g_layer_lock);
    g_layer = layer;
    pthread_mutex_unlock(&g_layer_lock);
    wb_log(TAG, "Metal layer set (%p)", metal_layer);
}

/* ---- the table's functions ---- */

static struct win_data g_win_data;   /* calls are not concurrent */
static char g_dummy_device;

WB_EXPORT struct win_data *get_win_data(HWND hwnd)
{
    g_win_data.hwnd = hwnd;
    g_win_data.window = &g_win_data;   /* any non-NULL value */
    g_win_data.view = &g_win_data;
    g_win_data.client_view = hwnd;     /* so the metal view call learns the window */
    return &g_win_data;
}

WB_EXPORT void release_win_data(struct win_data *data)
{
    (void)data;
}

static void *create_metal_device(void)
{
    return &g_dummy_device;   /* DXMT makes its own device */
}

static void release_metal_device(void *device)
{
    (void)device;
}

WB_EXPORT void *macdrv_view_create_metal_view(void *view, void *device)
{
    CALayer *layer;

    (void)device;
    pthread_mutex_lock(&g_layer_lock);
    layer = g_layer;
    pthread_mutex_unlock(&g_layer_lock);
    if (!layer) {
        wb_log(TAG, "create_metal_view(%p): no Metal layer has been set", view);
        return NULL;
    }
    wb_windows_mark_metal((HWND)view);
    return (void *)CFBridgingRetain(layer);
}

WB_EXPORT void *macdrv_view_get_metal_layer(void *metal_view)
{
    return metal_view;
}

WB_EXPORT void macdrv_view_release_metal_view(void *metal_view)
{
    if (metal_view) CFRelease(metal_view);   /* balances create_metal_view's retain */
}

static void on_main_thread(dispatch_block_t block)
{
    if (!block) return;
    if (pthread_main_np()) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

WB_EXPORT struct wb_macdrv_functions macdrv_functions = {
    NULL,
    get_win_data,
    release_win_data,
    NULL,
    create_metal_device,
    release_metal_device,
    macdrv_view_create_metal_view,
    macdrv_view_get_metal_layer,
    macdrv_view_release_metal_view,
    on_main_thread,
};

/* ---- screen size ---- */

static pthread_mutex_t g_mode_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_mode_w, g_mode_h;   /* 0 until a program publishes a mode */

static int env_positive(const char *name, int def)
{
    const char *s = getenv(name);
    char *end;
    long v;

    if (!s || !*s) return def;
    v = strtol(s, &end, 10);
    if (*end || v <= 0 || v > INT_MAX) return def;
    return (int)v;
}

void wb_screen_size(int *w, int *h)
{
    int mw, mh;

    pthread_mutex_lock(&g_mode_lock);
    mw = g_mode_w;
    mh = g_mode_h;
    pthread_mutex_unlock(&g_mode_lock);
    if (mw <= 0 || mh <= 0) {
        mw = env_positive("MADEIRA_SCREEN_W", 1024);
        mh = env_positive("MADEIRA_SCREEN_H", 768);
    }
    if (w) *w = mw;
    if (h) *h = mh;
}

void winios_display_mode_changed(int w, int h)
{
    int changed;

    if (w <= 0 || h <= 0) return;
    pthread_mutex_lock(&g_mode_lock);
    changed = w != g_mode_w || h != g_mode_h;
    g_mode_w = w;
    g_mode_h = h;
    pthread_mutex_unlock(&g_mode_lock);
    if (!changed) return;

    wb_log(TAG, "display mode %dx%d", w, h);
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:wb_display_mode_notification object:nil];
    });
}

void wb_display_reset_mode(void)
{
    pthread_mutex_lock(&g_mode_lock);
    g_mode_w = 0;
    g_mode_h = 0;
    pthread_mutex_unlock(&g_mode_lock);
}
