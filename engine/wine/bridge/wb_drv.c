// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part E "Driver hooks" and "Input queue". A C file on purpose: BOOL here is the unix
// side's 32-bit int (wb_internal.h), so the hooks return exactly the contract's type.
// Win32 flag values from Microsoft's MOUSEINPUT and KEYBDINPUT documentation.

#include "wb_internal.h"
#include "wb_log.h"

#include <pthread.h>
#include <stdatomic.h>

#define TAG "wb-input"

#define MOUSEEVENTF_MOVE     0x0001u
#define MOUSEEVENTF_LEFTDOWN 0x0002u
#define MOUSEEVENTF_LEFTUP   0x0004u
#define MOUSEEVENTF_ABSOLUTE 0x8000u
#define KEYEVENTF_KEYUP      0x0002u

#define RING_SIZE 256

enum { EV_KEY, EV_MOUSE };

struct wb_event {
    int kind;
    int x, y;
    unsigned int flags;
    unsigned int data;   /* key: the virtual-key code; mouse: wheel delta or XBUTTON */
};

static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wb_event g_ring[RING_SIZE];
static unsigned int g_head, g_count;
static atomic_uint g_dropped;

/* Never blocks for long: when the ring is full the new event is dropped. */
static void push(const struct wb_event *ev)
{
    int stored = 0;
    unsigned int dropped;

    pthread_mutex_lock(&g_ring_lock);
    if (g_count < RING_SIZE) {
        g_ring[(g_head + g_count) % RING_SIZE] = *ev;
        g_count++;
        stored = 1;
    }
    pthread_mutex_unlock(&g_ring_lock);

    if (stored) return;
    dropped = atomic_fetch_add(&g_dropped, 1) + 1;
    if (dropped <= 3 || dropped % 1000 == 0)
        wb_log(TAG, "input queue full, %u event(s) dropped so far", dropped);
}

static int pop(struct wb_event *ev)
{
    int got = 0;

    pthread_mutex_lock(&g_ring_lock);
    if (g_count > 0) {
        *ev = g_ring[g_head];
        g_head = (g_head + 1) % RING_SIZE;
        g_count--;
        got = 1;
    }
    pthread_mutex_unlock(&g_ring_lock);
    return got;
}

void wb_input_key(int vk, int down)
{
    struct wb_event ev = { EV_KEY, 0, 0, down ? 0u : KEYEVENTF_KEYUP, (unsigned int)vk };
    push(&ev);
}

void wb_input_pointer(int x, int y, unsigned int flags, unsigned int data)
{
    struct wb_event ev = { EV_MOUSE, x, y, flags, data };
    push(&ev);
}

void wb_input_touch(int phase, int x, int y)
{
    struct wb_event ev = { EV_MOUSE, x, y, 0, 0 };

    switch (phase) {
    case 0: ev.flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE; break;
    case 1: ev.flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE; break;
    case 2: ev.flags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE; break;
    default: return;
    }
    push(&ev);
}

/* ---- user driver hooks (Wine threads) ---- */

BOOL winios_pProcessEvents(DWORD mask)
{
    struct wb_event ev;
    BOOL delivered = 0;

    (void)mask;
    while (pop(&ev)) {   /* the lock is released for each delivery */
        if (ev.kind == EV_KEY)
            winios_drv_post_key((unsigned short)ev.data, ev.flags);
        else
            winios_drv_post_mouse(ev.x, ev.y, ev.flags, ev.data, NULL);
        delivered = 1;
    }
    return delivered;
}

BOOL winios_pCreateWindow(HWND hwnd)
{
    (void)hwnd;
    return 1;
}

void winios_pDestroyWindow(HWND hwnd)
{
    wb_windows_forget(hwnd);
}

UINT winios_pShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp)
{
    (void)hwnd; (void)cmd; (void)rect; (void)swp;
    return ~0u;   /* no override: any other value replaces the SWP_* flags */
}

void winios_pWindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface)
{
    (void)hwnd; (void)insert_after; (void)owner_hint; (void)swp_flags; (void)new_rects; (void)surface;
}

void winios_pSetCursor(HWND hwnd, HCURSOR cursor)
{
    (void)hwnd;
    (void)cursor;
}

void winios_pDestroyCursorIcon(HCURSOR cursor)
{
    (void)cursor;
}
