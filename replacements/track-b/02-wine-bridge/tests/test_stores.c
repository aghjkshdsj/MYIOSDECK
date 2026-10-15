// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_internal.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
// Include this unit in the test translation unit to seed wrap boundaries without
// adding test setters or changing the production ABI. Do not also link wb_b_pads.c.
#include "../b_pads.c"

static int wb_b_failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"line%d: %s\n",__LINE__,#condition); ++wb_b_failures; } } while(0)
static unsigned int wb_b_deliveries, wb_b_last_flags, wb_b_last_data;
static int wb_b_last_x, wb_b_reenter;
void winios_drv_post_key(unsigned short vk, unsigned int flags) {
    ++wb_b_deliveries; wb_b_last_x=vk; wb_b_last_flags=flags;
    if (wb_b_reenter) { wb_b_reenter=0; wb_input_pointer(91,92,1,0); }
}
void winios_drv_post_mouse(int x,int y,unsigned int flags,unsigned int data,HWND hwnd) {
    (void)y; (void)hwnd; ++wb_b_deliveries; wb_b_last_x=x; wb_b_last_flags=flags; wb_b_last_data=data;
}
static void wb_b_test_pads(void) {
    struct winios_gamepad p={0}, out;
    CHECK(!winios_gamepad_get_state(-1,&out)); CHECK(out.packet==0);
    p.connected=9; p.packet=123; p.reserved[0]=255;
    winios_gamepad_set_state(0,&p); CHECK(winios_gamepad_get_state(0,&out)); CHECK(out.packet==1 && out.connected==1 && out.reserved[0]==0);
    winios_gamepad_set_state(0,&p); winios_gamepad_get_state(0,&out); CHECK(out.packet==1);
    p.buttons=0x1000; winios_gamepad_set_state(0,&p); winios_gamepad_get_state(0,&out); CHECK(out.packet==2);
    winios_gamepad_set_state(0,NULL); CHECK(!winios_gamepad_get_state(0,&out)); CHECK(out.packet==0 && out.buttons==0);
    winios_gamepad_set_state(0,&p); winios_gamepad_get_state(0,&out); CHECK(out.packet==4);
    winios_gamepad_set_state(8,&p); CHECK(!winios_gamepad_get_state(8,NULL));
    wb_b_pads[0].packet=UINT32_MAX; p.buttons ^= 1;
    winios_gamepad_set_state(0,&p); winios_gamepad_get_state(0,&out); CHECK(out.packet==0 && out.connected);
    struct winios_hidpad h={0}, ho;
    h.connected=1; h.reserved[0]=3; h.reserved2=123; h.battery=255; h.buttons=WINIOS_HIDPAD_TOUCHPAD;
    winios_hidpad_set_state(&h); CHECK(winios_hidpad_get_state(&ho)); CHECK(ho.packet==1 && ho.reserved[0]==0 && ho.reserved2==0);
    winios_hidpad_set_state(&h); winios_hidpad_get_state(&ho); CHECK(ho.packet==1);
    winios_hidpad_set_state(NULL); CHECK(!winios_hidpad_get_state(&ho)); CHECK(ho.packet==2 && ho.buttons==0 && ho.battery==0);
    winios_hidpad_set_state(NULL); winios_hidpad_get_state(&ho); CHECK(ho.packet==2);
    wb_b_hid.packet=UINT32_MAX; winios_hidpad_set_state(&h); winios_hidpad_get_state(&ho); CHECK(ho.packet==0 && ho.connected);
    CHECK(winios_gamepad_rumble_caps()==0);
}
static void wb_b_test_outputs(void) {
    struct winios_gamepad_vibration v;
    wb_b_get_vibration(0,&v); CHECK(v.serial==0);
    winios_gamepad_set_vibration(0,0,0); wb_b_get_vibration(0,&v); CHECK(v.serial==1);
    winios_gamepad_set_vibration(0,0,0); wb_b_get_vibration(0,&v); CHECK(v.serial==1);
    winios_gamepad_set_vibration(0,123,456); wb_b_get_vibration(0,&v); CHECK(v.serial==2 && v.left==123 && v.right==456);
    wb_b_get_vibration(-1,&v); CHECK(v.serial==0);
    wb_b_vibrations[0].serial=UINT32_MAX;
    winios_gamepad_set_vibration(0,124,456); wb_b_get_vibration(0,&v); CHECK(v.serial==1);
    struct winios_hidpad_output o={0}, out;
    o.serial=100; o.reserved[0]=99; winios_hidpad_set_output(&o); wb_b_get_hid_output(&out); CHECK(out.serial==0 && out.reserved[0]==0);
    o.rumble_valid=1; o.rumble_left=12; winios_hidpad_set_output(&o); wb_b_get_hid_output(&out); CHECK(out.serial==1 && out.rumble_left==12);
    winios_hidpad_set_output(&o); wb_b_get_hid_output(&out); CHECK(out.serial==1);
    winios_hidpad_set_output(NULL); wb_b_get_hid_output(&out); CHECK(out.serial==1);
    wb_b_output.serial=UINT32_MAX; o.rumble_left=13; winios_hidpad_set_output(&o); wb_b_get_hid_output(&out); CHECK(out.serial==0);
}
static void wb_b_test_ring(void) {
    CHECK(!winios_pProcessEvents(0));
    for (int i=0;i<257;++i) wb_input_key(i,1);
    CHECK(winios_pProcessEvents(0)); CHECK(wb_b_deliveries==256 && wb_b_last_x==255 && wb_b_last_flags==0);
    wb_input_key(65,0); wb_b_reenter=1;
    CHECK(winios_pProcessEvents(0)); CHECK(wb_b_deliveries==258 && wb_b_last_x==91); // Reentrant enqueue proves delivery is unlocked.
    wb_input_touch(0,4,5); winios_pProcessEvents(0); CHECK(wb_b_last_flags==0x8003);
    wb_input_touch(1,4,5); winios_pProcessEvents(0); CHECK(wb_b_last_flags==0x8001);
    wb_input_touch(2,4,5); winios_pProcessEvents(0); CHECK(wb_b_last_flags==0x8004);
    wb_input_pointer(0,0,0x0800,(unsigned int)-120); winios_pProcessEvents(0); CHECK(wb_b_last_data==(unsigned int)-120 && wb_b_last_flags==0x0800);
    CHECK(winios_pShowWindow(NULL,0,NULL,0)==UINT_MAX); CHECK(winios_pCreateWindow(NULL)==1);
}
int main(void) { wb_b_test_pads(); wb_b_test_outputs(); wb_b_test_ring(); return wb_b_failures ? 1 : 0; }
