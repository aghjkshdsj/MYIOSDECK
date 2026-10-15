// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "wine_host.h"
void mid_display_screen_size(int *w,int *h) { *w=1024; *h=768; }
int mid_game_picture_rect(int dw,int dh,int *x,int *y,int *w,int *h) { return 0; }
void mid_post_key(int key,int down) {}
void mid_post_pointer(int x,int y,unsigned flags,int data) {}
void mid_post_mouse_button(unsigned flags) {}
void mid_post_touch(int phase,int x,int y) {}
void mid_wow_trace_calls(double seconds) {}
void mid_pad_set(int slot,int connected,uint16_t b,uint8_t lt,uint8_t rt,int16_t lx,int16_t ly,int16_t rx,int16_t ry) {}
void mid_hidpad_set(int connected,uint16_t b,uint8_t lt,uint8_t rt,int16_t lx,int16_t ly,int16_t rx,int16_t ry) {}
