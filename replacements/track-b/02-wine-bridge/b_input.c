// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_internal.h"

struct wb_b_event { int key, x, y; unsigned int flags, data; };
static pthread_mutex_t wb_b_event_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wb_b_event wb_b_events[256];
static unsigned int wb_b_head, wb_b_count;

static void wb_b_enqueue(struct wb_b_event event) {
    pthread_mutex_lock(&wb_b_event_lock);
    if (wb_b_count < 256) { wb_b_events[(wb_b_head + wb_b_count) % 256] = event; ++wb_b_count; }
    pthread_mutex_unlock(&wb_b_event_lock);
}
void wb_input_key(int vk, int down) { wb_b_enqueue((struct wb_b_event){1, vk, 0, down ? 0u : 2u, 0}); }
void wb_input_pointer(int x, int y, unsigned int flags, unsigned int data) {
    wb_b_enqueue((struct wb_b_event){0, x, y, flags, data});
}
void wb_input_touch(int phase, int x, int y) {
    if (phase < 0 || phase > 2) return;
    const unsigned int flags[] = {0x8003, 0x8001, 0x8004};
    wb_input_pointer(x, y, flags[phase], 0);
}
BOOL winios_pProcessEvents(DWORD mask) {
    (void)mask;
    int delivered = 0;
    for (;;) {
        pthread_mutex_lock(&wb_b_event_lock);
        if (!wb_b_count) { pthread_mutex_unlock(&wb_b_event_lock); break; }
        const struct wb_b_event event = wb_b_events[wb_b_head];
        wb_b_head = (wb_b_head + 1) % 256;
        --wb_b_count;
        pthread_mutex_unlock(&wb_b_event_lock);
        if (event.key) winios_drv_post_key((unsigned short)event.x, event.flags);
        else winios_drv_post_mouse(event.x, event.y, event.flags, event.data, NULL);
        delivered = 1;
    }
    return delivered ? 1 : 0;
}

BOOL winios_pCreateWindow(HWND hwnd) { (void)hwnd; return 1; }
UINT winios_pShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp) {
    (void)hwnd; (void)cmd; (void)rect; (void)swp; return ~0u;
}
void winios_pWindowPosChanged(HWND hwnd, HWND after, HWND owner, UINT flags,
                              const struct window_rects *rects, struct window_surface *surface) {
    (void)hwnd; (void)after; (void)owner; (void)flags; (void)rects; (void)surface;
}
void winios_pSetCursor(HWND hwnd, HCURSOR cursor) { (void)hwnd; (void)cursor; }
void winios_pDestroyCursorIcon(HCURSOR cursor) { (void)cursor; }
