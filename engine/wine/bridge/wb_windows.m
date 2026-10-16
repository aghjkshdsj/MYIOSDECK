// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part E "The overlay" and "Game picture": the images of a game's ordinary GDI
// windows (launchers, dialogs) drawn above the game view, one CALayer per top-level window.
// All state below lives on the main thread; the Wine-thread entry points copy their
// arguments and continue on the main queue, which keeps their order.

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include "wb_internal.h"
#include "wb_log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wb-windows"

@interface wb_GameWindow : NSObject
@property (nonatomic, strong) CALayer *layer;
@property (nonatomic, strong) id image;            /* CGImageRef */
@property (nonatomic) int surfW, surfH;
@property (nonatomic) BOOL hasFrame, visible;
@property (nonatomic) int x, y, w, h;              /* visible rect, desktop pixels */
@property (nonatomic) BOOL hasClient;
@property (nonatomic) int cx, cy, cw, ch;          /* client rect, desktop pixels */
@property (nonatomic) BOOL loggedExclusion;
@end

@implementation wb_GameWindow
@end

@interface wb_OverlayView : UIView
@end

static NSMutableDictionary<NSNumber *, wb_GameWindow *> *g_windows;
static NSMutableSet<NSNumber *> *g_metal;
static wb_OverlayView *g_overlay;
static BOOL g_frame_set;
static CGRect g_frame;
static BOOL g_desk_set;
static CGRect g_desk;

static void relayout_all(void);

@implementation wb_OverlayView
- (void)layoutSubviews
{
    [super layoutSubviews];
    relayout_all();
}
@end

static int overlay_enabled(void)
{
    const char *v = getenv("MADEIRA_GAME_WINDOWS");
    return !(v && strcmp(v, "0") == 0);
}

static NSNumber *key_for(HWND hwnd)
{
    return @((uintptr_t)hwnd);
}

static void run_on_main(dispatch_block_t block)
{
    if (pthread_main_np()) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

static void ensure_state(void)
{
    static dispatch_once_t once;

    if (!g_windows) {
        g_windows = [NSMutableDictionary new];
        g_metal = [NSMutableSet new];
    }
    dispatch_once(&once, ^{
        [NSNotificationCenter.defaultCenter addObserverForName:wb_display_mode_notification
                                                        object:nil
                                                         queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *note) {
            (void)note;
            relayout_all();
        }];
    });
}

static wb_GameWindow *window_for(NSNumber *key)
{
    wb_GameWindow *win = g_windows[key];

    if (!win) {
        win = [wb_GameWindow new];
        g_windows[key] = win;
    }
    return win;
}

static UIWindow *find_window(void)
{
    UIWindow *first = nil;

    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *w in ((UIWindowScene *)scene).windows) {
            if (w.isKeyWindow) return w;
            if (!first) first = w;
        }
    }
    return first;
}

static void ensure_overlay(void)
{
    static BOOL logged_no_window;
    UIWindow *window;

    if (g_overlay || !overlay_enabled()) return;
    window = find_window();
    if (!window) {
        if (!logged_no_window) wb_log(TAG, "overlay: no window to add it to yet");
        logged_no_window = YES;
        return;
    }
    g_overlay = [[wb_OverlayView alloc] initWithFrame:g_frame_set ? g_frame : window.bounds];
    g_overlay.backgroundColor = UIColor.clearColor;
    g_overlay.opaque = NO;
    g_overlay.clipsToBounds = YES;
    g_overlay.userInteractionEnabled = NO;   /* touches reach the game view below */
    g_overlay.autoresizingMask = g_frame_set ? UIViewAutoresizingNone
                                             : (UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight);
    [window addSubview:g_overlay];
    wb_log(TAG, "overlay created in window %p, %.0fx%.0f at %.0f,%.0f", (__bridge void *)window,
           g_overlay.frame.size.width, g_overlay.frame.size.height, g_overlay.frame.origin.x,
           g_overlay.frame.origin.y);
    relayout_all();
}

/* Desktop pixels to overlay points. */
static CGRect desktop_to_view(int x, int y, int w, int h)
{
    CGSize size = g_overlay.bounds.size;
    double sx, sy, ox, oy;
    int dw, dh;

    wb_screen_size(&dw, &dh);
    if (g_desk_set) {
        sx = g_desk.size.width / dw;
        sy = g_desk.size.height / dh;   /* may differ from sx when the picture is stretched */
        ox = g_desk.origin.x;
        oy = g_desk.origin.y;
    } else {
        double s = MIN(size.width / dw, size.height / dh);
        sx = sy = s;
        ox = (size.width - dw * s) / 2;
        oy = (size.height - dh * s) / 2;
    }
    return CGRectMake(ox + x * sx, oy + y * sy, w * sx, h * sy);
}

static CGRect crop_rect(wb_GameWindow *win)
{
    /* Surfaces are rounded up past the window size. */
    if (!win.hasFrame || win.surfW <= 0 || win.surfH <= 0) return CGRectMake(0, 0, 1, 1);
    return CGRectMake(0, 0, MAX(0.0, MIN((double)win.w / win.surfW, 1.0)),
                      MAX(0.0, MIN((double)win.h / win.surfH, 1.0)));
}

static void drop_layer(wb_GameWindow *win)
{
    if (!win.layer) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [win.layer removeFromSuperlayer];
    [CATransaction commit];
    win.layer = nil;
}

/* Decide whether the window is drawn and bring its layer up to date. */
static void update_window(NSNumber *key, wb_GameWindow *win)
{
    const char *why = NULL;
    int dw, dh;

    if (!overlay_enabled()) {
        drop_layer(win);
        return;
    }
    wb_screen_size(&dw, &dh);
    if ([g_metal containsObject:key])
        why = "it presents through Metal";
    else if (win.hasFrame && win.x <= 0 && win.y <= 0 && win.x + win.w >= dw && win.y + win.h >= dh)
        why = "it covers the whole desktop (the game's own window)";
    if (why) {
        drop_layer(win);
        if (!win.loggedExclusion) {
            win.loggedExclusion = YES;
            wb_log(TAG, "window %p not drawn: %s", (void *)(uintptr_t)key.unsignedLongValue, why);
        }
        return;
    }
    /* Position unknown: no layer yet. The image is kept on `win` and fills the layer as soon
     * as winios_window_frame places the window (spec 02 clarification 16). */
    if (!win.hasFrame) {
        drop_layer(win);
        return;
    }
    if (!g_overlay) return;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (!win.layer) {
        CALayer *layer = [CALayer layer];
        layer.anchorPoint = CGPointZero;
        layer.magnificationFilter = kCAFilterNearest;
        layer.opaque = YES;
        [g_overlay.layer addSublayer:layer];
        win.layer = layer;
    }
    win.layer.frame = desktop_to_view(win.x, win.y, win.w, win.h);
    win.layer.hidden = !win.visible;
    if (win.image && win.layer.contents != win.image) win.layer.contents = win.image;
    win.layer.contentsRect = crop_rect(win);
    [CATransaction commit];
}

static void relayout_all(void)
{
    if (!g_overlay) return;
    [g_windows enumerateKeysAndObjectsUsingBlock:^(NSNumber *key, wb_GameWindow *win, BOOL *stop) {
        (void)stop;
        update_window(key, win);
    }];
}

static void apply_overlay_frame(void)
{
    CGRect frame;

    if (!g_overlay) return;
    if (g_frame_set) frame = g_frame;
    else if (g_overlay.superview) frame = g_overlay.superview.bounds;
    else frame = g_overlay.frame;
    g_overlay.autoresizingMask = g_frame_set ? UIViewAutoresizingNone
                                             : (UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight);
    if (!CGRectEqualToRect(g_overlay.frame, frame)) g_overlay.frame = frame;
    relayout_all();
}

/* ================= Wine-thread entry points ================= */

void winios_window_frame(HWND hwnd, int x, int y, int w, int h, int visible,
                         int cx, int cy, int cw, int ch)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        NSNumber *key = key_for(hwnd);
        wb_GameWindow *win;

        ensure_state();
        ensure_overlay();
        win = window_for(key);
        win.hasFrame = YES;
        win.x = x;
        win.y = y;
        win.w = w;
        win.h = h;
        win.visible = visible != 0;
        /* The client rect, for the game-picture query, while it can be the picture. */
        win.hasClient = win.visible && cw > 0 && ch > 0;
        win.cx = cx;
        win.cy = cy;
        win.cw = cw;
        win.ch = ch;
        update_window(key, win);
    });
}

static atomic_uint g_copy_failures;

int winios_surface_present(HWND hwnd, int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                           int surf_w, int surf_h, int stride, const void *bits)
{
    size_t size;
    void *copy;
    NSData *data;

    (void)dirty_x; (void)dirty_y; (void)dirty_w; (void)dirty_h;   /* the whole surface is copied */
    if (!overlay_enabled()) return 1;
    if (!bits || surf_w <= 0 || surf_h <= 0 || stride < surf_w * 4) return 1;

    size = (size_t)stride * (size_t)surf_h;
    copy = malloc(size);
    if (!copy) {
        unsigned int n = atomic_fetch_add(&g_copy_failures, 1) + 1;
        if (n <= 3 || n % 100 == 0)
            wb_log(TAG, "surface of %p: cannot copy %zu bytes (%u failures)", hwnd, size, n);
        return 0;   /* win32u keeps the damage and flushes again */
    }
    memcpy(copy, bits, size);

    @autoreleasepool {
        /* Freed when the last user (the block, the image, the layer) lets go. */
        data = [[NSData alloc] initWithBytesNoCopy:copy length:size freeWhenDone:YES];
        dispatch_async(dispatch_get_main_queue(), ^{
            static CGColorSpaceRef rgb;
            CGDataProviderRef provider;
            CGImageRef image;
            NSNumber *key = key_for(hwnd);
            wb_GameWindow *win;

            if (!rgb) rgb = CGColorSpaceCreateDeviceRGB();
            provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
            if (!provider) return;
            image = CGImageCreate((size_t)surf_w, (size_t)surf_h, 8, 32, (size_t)stride, rgb,
                                  kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst,
                                  provider, NULL, false, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            if (!image) return;

            ensure_state();
            ensure_overlay();
            win = window_for(key);
            win.image = (__bridge_transfer id)image;
            win.surfW = surf_w;
            win.surfH = surf_h;
            update_window(key, win);
        });
    }
    return 1;
}

void wb_windows_forget(HWND hwnd)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        NSNumber *key = key_for(hwnd);
        wb_GameWindow *win = g_windows[key];

        if (!win) return;
        drop_layer(win);
        [g_windows removeObjectForKey:key];
    });
}

void wb_windows_mark_metal(HWND hwnd)
{
    if (!hwnd) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSNumber *key = key_for(hwnd);
        wb_GameWindow *win;

        ensure_state();
        if ([g_metal containsObject:key]) return;
        [g_metal addObject:key];
        wb_log(TAG, "window %p presents through Metal", hwnd);
        win = g_windows[key];
        if (win) update_window(key, win);
    });
}

/* Clarifications 6 and 20: per-window state, the Metal set, the overlay view and the
 * published screen size go; the app's frame and desktop rect stay (it republishes them on
 * its next layout). */
void winios_session_reset(void)
{
    wb_display_reset_mode();
    dispatch_async(dispatch_get_main_queue(), ^{
        ensure_state();
        [g_metal removeAllObjects];
        for (wb_GameWindow *win in g_windows.allValues) drop_layer(win);
        [g_windows removeAllObjects];
        if (g_overlay) {
            [g_overlay removeFromSuperview];
            g_overlay = nil;
            wb_log(TAG, "session reset: overlay removed");
        }
    });
}

/* ================= app-facing (wine_bridge.h) ================= */

void wb_overlay_set_frame(double x, double y, double w, double h)
{
    CGRect r = CGRectMake(x, y, w, h);

    run_on_main(^{
        if (g_frame_set && CGRectEqualToRect(g_frame, r)) return;
        g_frame_set = YES;
        g_frame = r;
        apply_overlay_frame();
    });
}

void wb_overlay_set_desktop_rect(double x, double y, double w, double h, int set)
{
    BOOL on = set && w >= 1 && h >= 1;
    CGRect r = CGRectMake(x, y, w, h);

    run_on_main(^{
        if (!on && !g_desk_set) return;
        if (on && g_desk_set && CGRectEqualToRect(g_desk, r)) return;
        g_desk_set = on;
        g_desk = on ? r : CGRectZero;
        relayout_all();
    });
}

void wb_overlay_show(int shown, void *window)
{
    UIWindow *target = (__bridge UIWindow *)window;

    if (!g_overlay) return;
    if (shown && target && g_overlay.superview != target) {
        [target addSubview:g_overlay];
        apply_overlay_frame();
    }
    g_overlay.hidden = !shown;
    if (shown) [g_overlay.superview bringSubviewToFront:g_overlay];
}

int wb_game_picture_rect(int dw, int dh, int *x, int *y, int *w, int *h)
{
    struct wb_client_rect *rects;
    int n = 0, desk_w, desk_h, found;

    if (g_windows.count == 0) return 0;
    rects = calloc(g_windows.count, sizeof(*rects));
    if (!rects) return 0;
    for (NSNumber *key in g_windows) {
        wb_GameWindow *win = g_windows[key];
        if (!win.hasClient) continue;
        rects[n].x = win.cx;
        rects[n].y = win.cy;
        rects[n].w = win.cw;
        rects[n].h = win.ch;
        rects[n].metal = [g_metal containsObject:key];
        n++;
    }
    wb_screen_size(&desk_w, &desk_h);
    found = wb_pick_picture_rect(rects, n, dw, dh, desk_w, desk_h, x, y, w, h);
    free(rects);
    return found;
}
