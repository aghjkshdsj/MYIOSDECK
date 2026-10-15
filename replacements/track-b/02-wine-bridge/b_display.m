// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#include "b_internal.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdatomic.h>

#define B_EXPORT __attribute__((used, visibility("default")))
static pthread_mutex_t wb_b_layer_lock = PTHREAD_MUTEX_INITIALIZER;
static void *wb_b_metal_layer; // Contract: layer outlives Wine. No UIKit access under lock.
static pthread_mutex_t wb_b_size_lock = PTHREAD_MUTEX_INITIALIZER;
static int wb_b_width, wb_b_height, wb_b_has_size;
static NSString *const wb_b_size_notification = @"MYIOSDECKTrackBDisplayModeChanged";
static _Atomic unsigned int wb_b_copy_failures;

@interface wb_BBridgeWindow : NSObject
@property(nonatomic) CGRect visibleRect;
@property(nonatomic) CGRect clientRect;
@property(nonatomic) BOOL hasPosition;
@property(nonatomic) BOOL visible;
@property(nonatomic) BOOL hasClient;
@property(nonatomic) BOOL exclusionLogged;
@property(nonatomic, strong) CALayer *layer;
@property(nonatomic, strong) id image;
@property(nonatomic) int surfaceWidth;
@property(nonatomic) int surfaceHeight;
@end
@implementation wb_BBridgeWindow
@end

// Main queue only from here to the raw-layer / display-size locks below.
static NSMutableDictionary<NSValue *, wb_BBridgeWindow *> *wb_b_windows;
static NSMutableSet<NSValue *> *wb_b_metal_windows;
static UIView *wb_b_overlay_view;
static CGRect wb_b_overlay_frame, wb_b_desktop_rect;
static BOOL wb_b_has_frame, wb_b_has_desktop;
static id wb_b_size_observer;

static void wb_b_layout(void);
static void wb_b_on_main(dispatch_block_t block) {
    if (!block) return;
    if (NSThread.isMainThread) block(); else dispatch_async(dispatch_get_main_queue(), block);
}
static void wb_b_init_windows(void) {
    if (!wb_b_windows) wb_b_windows = [NSMutableDictionary dictionary];
    if (!wb_b_metal_windows) wb_b_metal_windows = [NSMutableSet set];
    if (!wb_b_size_observer) wb_b_size_observer = [NSNotificationCenter.defaultCenter addObserverForName:wb_b_size_notification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *note) { (void)note; wb_b_layout(); }];
}
static BOOL wb_b_windows_enabled(void) {
    const char *value = getenv("MADEIRA_GAME_WINDOWS");
    BOOL enabled = !value || strcmp(value, "0");
    if (!enabled && NSThread.isMainThread) wb_b_overlay_view.hidden = YES;
    return enabled;
}
static UIWindow *wb_b_host_window(void) {
    UIWindow *first = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            if (!first) first = window;
            if (window.isKeyWindow) return window;
        }
    }
    return first;
}
static BOOL wb_b_make_overlay(void) {
    wb_b_init_windows();
    if (!wb_b_windows_enabled()) return NO;
    if (wb_b_overlay_view) return YES;
    UIWindow *window = wb_b_host_window();
    if (!window) return NO;
    wb_b_overlay_view = [[UIView alloc] initWithFrame:wb_b_has_frame ? wb_b_overlay_frame : window.bounds];
    wb_b_overlay_view.backgroundColor = UIColor.clearColor;
    wb_b_overlay_view.opaque = NO;
    wb_b_overlay_view.clipsToBounds = YES;
    wb_b_overlay_view.userInteractionEnabled = NO;
    [window addSubview:wb_b_overlay_view];
    wb_b_log("[wb-windows] overlay created");
    return YES;
}
static wb_BBridgeWindow *wb_b_record(HWND hwnd) {
    wb_b_init_windows();
    NSValue *key = [NSValue valueWithPointer:hwnd];
    wb_BBridgeWindow *record = wb_b_windows[key];
    if (!record) { record = [wb_BBridgeWindow new]; wb_b_windows[key] = record; }
    return record;
}
static CGRect wb_b_map_rect(CGRect rect) {
    int dw, dh; wb_screen_size(&dw, &dh);
    CGFloat sx, sy, ox, oy;
    if (wb_b_has_desktop) {
        sx = wb_b_desktop_rect.size.width / dw; sy = wb_b_desktop_rect.size.height / dh;
        ox = wb_b_desktop_rect.origin.x; oy = wb_b_desktop_rect.origin.y;
    } else {
        CGFloat scale = MIN(wb_b_overlay_view.bounds.size.width / dw, wb_b_overlay_view.bounds.size.height / dh);
        sx = sy = scale;
        ox = (wb_b_overlay_view.bounds.size.width - dw * scale) / 2;
        oy = (wb_b_overlay_view.bounds.size.height - dh * scale) / 2;
    }
    return CGRectMake(ox + rect.origin.x * sx, oy + rect.origin.y * sy, rect.size.width * sx, rect.size.height * sy);
}
static void wb_b_update(NSValue *key, wb_BBridgeWindow *record) {
    int dw, dh; wb_screen_size(&dw, &dh);
    CGRect r = record.visibleRect;
    BOOL metal = [wb_b_metal_windows containsObject:key];
    BOOL covers = r.origin.x <= 0 && r.origin.y <= 0 && CGRectGetMaxX(r) >= dw && CGRectGetMaxY(r) >= dh;
    const char *reason = metal ? "Metal presentation" : !record.hasPosition ? "position unknown" : covers ? "covers desktop" : NULL;
    if (reason || !wb_b_windows_enabled()) {
        [record.layer removeFromSuperlayer]; record.layer = nil;
        if (reason && !record.exclusionLogged) {
            record.exclusionLogged = YES;
            wb_b_log("[wb-windows] omit hwnd=%p: %s", key.pointerValue, reason);
        }
        return;
    }
    if (!wb_b_make_overlay()) return;
    if (!record.layer) {
        record.layer = [CALayer layer];
        record.layer.anchorPoint = CGPointZero;
        record.layer.magnificationFilter = kCAFilterNearest;
        record.layer.minificationFilter = kCAFilterNearest;
        record.layer.opaque = YES;
        [wb_b_overlay_view.layer addSublayer:record.layer];
    }
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    record.layer.frame = wb_b_map_rect(r);
    record.layer.hidden = !record.visible;
    record.layer.contents = record.image;
    if (record.surfaceWidth > 0 && record.surfaceHeight > 0) {
        record.layer.contentsRect = CGRectMake(0, 0,
            MIN(MAX(0, r.size.width) / record.surfaceWidth, 1), MIN(MAX(0, r.size.height) / record.surfaceHeight, 1));
    }
    [CATransaction commit];
}
static void wb_b_layout(void) {
    if (!NSThread.isMainThread || !wb_b_windows_enabled()) return;
    if (wb_b_overlay_view) {
        CGRect frame = wb_b_has_frame ? wb_b_overlay_frame : wb_b_overlay_view.window.bounds;
        [CATransaction begin]; [CATransaction setDisableActions:YES]; wb_b_overlay_view.frame = frame; [CATransaction commit];
    }
    for (NSValue *key in wb_b_windows) wb_b_update(key, wb_b_windows[key]);
}

void wb_display_set_layer(void *metal_layer) {
    pthread_mutex_lock(&wb_b_layer_lock); wb_b_metal_layer = metal_layer; pthread_mutex_unlock(&wb_b_layer_lock);
}
static int wb_b_positive_env(const char *name, int fallback) {
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return fallback;
    char *end; long n = strtol(s, &end, 10);
    return !*end && n > 0 && n <= INT_MAX ? (int)n : fallback;
}
void wb_screen_size(int *w, int *h) {
    pthread_mutex_lock(&wb_b_size_lock);
    int known = wb_b_has_size, width = wb_b_width, height = wb_b_height;
    pthread_mutex_unlock(&wb_b_size_lock);
    if (!known) { width = wb_b_positive_env("MADEIRA_SCREEN_W", 1024); height = wb_b_positive_env("MADEIRA_SCREEN_H", 768); }
    if (w) *w = width;
    if (h) *h = height;
}
void winios_display_mode_changed(int w, int h) {
    if (w <= 0 || h <= 0) return;
    int oldw = wb_b_positive_env("MADEIRA_SCREEN_W", 1024), oldh = wb_b_positive_env("MADEIRA_SCREEN_H", 768);
    pthread_mutex_lock(&wb_b_size_lock);
    if (wb_b_has_size) { oldw = wb_b_width; oldh = wb_b_height; }
    wb_b_width = w; wb_b_height = h; wb_b_has_size = 1;
    pthread_mutex_unlock(&wb_b_size_lock);
    if (oldw != w || oldh != h) dispatch_async(dispatch_get_main_queue(), ^{
        wb_b_init_windows();
        [NSNotificationCenter.defaultCenter postNotificationName:wb_b_size_notification object:nil];
    });
}
void wb_overlay_set_frame(double x, double y, double w, double h) {
    CGRect frame = CGRectMake(x, y, w, h);
    wb_b_on_main(^{
        if (wb_b_has_frame && CGRectEqualToRect(frame, wb_b_overlay_frame)) return;
        wb_b_has_frame = YES; wb_b_overlay_frame = frame; wb_b_layout();
    });
}
void wb_overlay_set_desktop_rect(double x, double y, double w, double h, int set) {
    CGRect rect = CGRectMake(x, y, MAX(1, w), MAX(1, h));
    wb_b_on_main(^{
        if (wb_b_has_desktop == (set != 0) && (!set || CGRectEqualToRect(rect, wb_b_desktop_rect))) return;
        wb_b_has_desktop = set != 0; wb_b_desktop_rect = rect; wb_b_layout();
    });
}
void wb_overlay_show(int shown, void *window) {
    NSCAssert(NSThread.isMainThread, @"overlay show requires main thread");
    if (!wb_b_overlay_view || !wb_b_windows_enabled()) return;
    if (shown && window) {
        UIWindow *target = (__bridge UIWindow *)window;
        if (wb_b_overlay_view.superview != target) { [target addSubview:wb_b_overlay_view]; wb_b_layout(); }
    }
    wb_b_overlay_view.hidden = !shown;
    if (shown) [wb_b_overlay_view.superview bringSubviewToFront:wb_b_overlay_view];
}
void winios_window_frame(HWND hwnd, int x, int y, int w, int h, int visible, int cx, int cy, int cw, int ch) {
    wb_b_on_main(^{
        wb_b_make_overlay();
        wb_BBridgeWindow *record = wb_b_record(hwnd);
        record.visibleRect = CGRectMake(x, y, w, h); record.hasPosition = YES; record.visible = visible != 0;
        record.hasClient = visible && cw > 0 && ch > 0;
        record.clientRect = record.hasClient ? CGRectMake(cx, cy, cw, ch) : CGRectZero;
        wb_b_update([NSValue valueWithPointer:hwnd], record);
    });
}
void winios_pDestroyWindow(HWND hwnd) {
    wb_b_on_main(^{
        NSValue *key = [NSValue valueWithPointer:hwnd];
        [wb_b_windows[key].layer removeFromSuperlayer];
        [wb_b_windows removeObjectForKey:key]; [wb_b_metal_windows removeObject:key];
    });
}
int winios_surface_present(HWND hwnd, int dx, int dy, int dw, int dh, int sw, int sh, int stride, const void *bits) {
    (void)dx; (void)dy; (void)dw; (void)dh;
    if (!wb_b_windows_enabled()) return 1;
    if (!bits || sw <= 0 || sh <= 0 || stride <= 0 || (uint64_t)sw * 4 > (uint64_t)stride || (size_t)stride > SIZE_MAX / (size_t)sh) return 0;
    size_t length = (size_t)stride * (size_t)sh;
    void *copy = malloc(length);
    if (!copy) {
        if (atomic_fetch_add_explicit(&wb_b_copy_failures, 1, memory_order_relaxed) < 4) wb_b_log("[wb-windows] surface allocation failed bytes=%zu", length);
        return 0;
    }
    memcpy(copy, bits, length);
    NSData *data = [NSData dataWithBytesNoCopy:copy length:length freeWhenDone:YES];
    if (!data) { free(copy); return 0; }
    wb_b_on_main(^{
        if (!wb_b_windows_enabled()) return;
        wb_b_make_overlay();
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = provider && color ? CGImageCreate(sw, sh, 8, 32, stride, color,
            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, NULL, false, kCGRenderingIntentDefault) : NULL;
        if (image) {
            wb_BBridgeWindow *record = wb_b_record(hwnd);
            record.image = (__bridge id)image; record.surfaceWidth = sw; record.surfaceHeight = sh;
            wb_b_update([NSValue valueWithPointer:hwnd], record);
            CGImageRelease(image);
        }
        if (provider) CGDataProviderRelease(provider);
        if (color) CGColorSpaceRelease(color);
    });
    return 1;
}
void wb_b_mark_metal(HWND hwnd) {
    wb_b_on_main(^{
        wb_b_init_windows();
        NSValue *key = [NSValue valueWithPointer:hwnd];
        if ([wb_b_metal_windows containsObject:key]) return;
        [wb_b_metal_windows addObject:key];
        wb_b_log("[wb-windows] Metal hwnd=%p", hwnd);
        wb_BBridgeWindow *record = wb_b_windows[key];
        if (record) wb_b_update(key, record);
    });
}
void winios_session_reset(void) {
    pthread_mutex_lock(&wb_b_size_lock);
    wb_b_has_size = 0; wb_b_width = 0; wb_b_height = 0;
    pthread_mutex_unlock(&wb_b_size_lock);
    wb_b_on_main(^{
        [wb_b_metal_windows removeAllObjects]; [wb_b_windows removeAllObjects];
        [wb_b_overlay_view removeFromSuperview]; wb_b_overlay_view = nil;
        wb_b_has_frame = NO; wb_b_has_desktop = NO; wb_b_overlay_frame = CGRectZero; wb_b_desktop_rect = CGRectZero;
    });
}
int wb_game_picture_rect(int dw, int dh, int *x, int *y, int *w, int *h) {
    NSCAssert(NSThread.isMainThread, @"picture query requires main thread");
    NSArray<NSValue *> *keys = wb_b_windows.allKeys;
    // Stable pointer order also gives deterministic tie-breaking independent of insertion order.
    keys = [keys sortedArrayUsingComparator:^NSComparisonResult(NSValue *a, NSValue *b) {
        uintptr_t av = (uintptr_t)a.pointerValue, bv = (uintptr_t)b.pointerValue;
        return av < bv ? NSOrderedAscending : av > bv ? NSOrderedDescending : NSOrderedSame;
    }];
    size_t count = keys.count;
    if (!count) return 0;
    struct wb_b_picture *candidates = calloc(count, sizeof *candidates);
    if (!candidates) return 0;
    for (size_t i = 0; i < count; ++i) {
        wb_BBridgeWindow *record = wb_b_windows[keys[i]];
        if (!record.hasClient) continue;
        CGRect r = record.clientRect;
        candidates[i] = (struct wb_b_picture){(int)r.origin.x, (int)r.origin.y, (int)r.size.width, (int)r.size.height,
            [wb_b_metal_windows containsObject:keys[i]] ? 1 : 0};
    }
    int sw, sh; wb_screen_size(&sw, &sh);
    int index = wb_b_choose_picture(candidates, count, dw, dh, sw, sh);
    if (index >= 0) {
        struct wb_b_picture p = candidates[index];
        if (x) *x = p.x; if (y) *y = p.y; if (w) *w = p.w; if (h) *h = p.h;
    }
    free(candidates);
    return index >= 0;
}

struct win_data { HWND hwnd; void *window; void *view; void *client_view; };
static struct win_data wb_b_window_data;
B_EXPORT struct win_data *get_win_data(HWND hwnd) {
    wb_b_window_data = (struct win_data){hwnd, &wb_b_window_data, &wb_b_window_data, hwnd};
    return &wb_b_window_data;
}
B_EXPORT void release_win_data(struct win_data *data) { (void)data; }
static void *wb_b_dummy_device(void) { static int device; return &device; }
static void wb_b_release_device(void *device) { (void)device; }
B_EXPORT void *macdrv_view_create_metal_view(void *view, void *device) {
    (void)device;
    pthread_mutex_lock(&wb_b_layer_lock); void *raw = wb_b_metal_layer; pthread_mutex_unlock(&wb_b_layer_lock);
    if (!raw) { wb_b_log("[wb-display] no Metal layer for hwnd=%p", view); return NULL; }
    wb_b_mark_metal((HWND)view);
    return (void *)CFBridgingRetain((__bridge id)raw);
}
B_EXPORT void *macdrv_view_get_metal_layer(void *metal_view) { return metal_view; }
B_EXPORT void macdrv_view_release_metal_view(void *metal_view) { if (metal_view) CFRelease((CFTypeRef)metal_view); }
struct wb_b_macdrv_table {
    void (*init_display_devices)(BOOL);
    struct win_data *(*window_data)(HWND);
    void (*release_data)(struct win_data *);
    void *(*native_window)(HWND, BOOL);
    void *(*create_device)(void);
    void (*release_device)(void *);
    void *(*create_view)(void *, void *);
    void *(*get_layer)(void *);
    void (*release_view)(void *);
    void (*on_main)(dispatch_block_t);
};
_Static_assert(sizeof(struct win_data) == 4 * sizeof(void *), "win_data ABI");
_Static_assert(sizeof(struct wb_b_macdrv_table) == 10 * sizeof(void *), "DXMT table ABI");
B_EXPORT struct wb_b_macdrv_table macdrv_functions = {
    NULL, get_win_data, release_win_data, NULL, wb_b_dummy_device, wb_b_release_device,
    macdrv_view_create_metal_view, macdrv_view_get_metal_layer, macdrv_view_release_metal_view, wb_b_on_main
};
