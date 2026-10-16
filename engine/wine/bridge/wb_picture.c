// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part E "Game picture": which stored client rectangle is the game's picture.

#include "wb_internal.h"

int wb_pick_picture_rect(const struct wb_client_rect *rects, int n, int dw, int dh,
                         int desk_w, int desk_h, int *x, int *y, int *w, int *h)
{
    const struct wb_client_rect *best = NULL;
    int64_t best_area = -1;
    int64_t quarter = ((int64_t)desk_w * (int64_t)desk_h + 3) / 4;   /* area * 4 >= desktop */

    /* 1. The largest window that presents through Metal. */
    for (int i = 0; i < n; i++) {
        int64_t area = (int64_t)rects[i].w * (int64_t)rects[i].h;
        if (rects[i].metal && area > best_area) {
            best = &rects[i];
            best_area = area;
        }
    }

    /* 2. Otherwise the largest whose client size is exactly the picture size and covers at
     *    least a quarter of the desktop (32-bit games are never marked). */
    if (!best) {
        for (int i = 0; i < n; i++) {
            int64_t area = (int64_t)rects[i].w * (int64_t)rects[i].h;
            if (rects[i].w == dw && rects[i].h == dh && area >= quarter && area > best_area) {
                best = &rects[i];
                best_area = area;
            }
        }
    }

    if (!best) return 0;
    if (x) *x = best->x;
    if (y) *y = best->y;
    if (w) *w = best->w;
    if (h) *h = best->h;
    return 1;
}
