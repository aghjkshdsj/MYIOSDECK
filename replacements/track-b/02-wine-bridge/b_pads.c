// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_internal.h"
#include <string.h>

_Static_assert(sizeof(struct winios_gamepad) == 20, "gamepad ABI");
_Static_assert(sizeof(struct winios_hidpad) == 48, "hidpad ABI");
_Static_assert(sizeof(struct winios_hidpad_output) == 44, "hid output ABI");
static pthread_mutex_t wb_b_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wb_b_hid_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wb_b_output_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wb_b_vibration_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winios_gamepad wb_b_pads[WINIOS_GAMEPAD_MAX];
static struct winios_hidpad wb_b_hid;
static struct winios_hidpad_output wb_b_output;
static struct winios_gamepad_vibration wb_b_vibrations[WINIOS_GAMEPAD_MAX];

void winios_gamepad_set_state(int index, const struct winios_gamepad *s) {
    if (index < 0 || index >= WINIOS_GAMEPAD_MAX) return;
    struct winios_gamepad next = {0};
    if (s && s->connected) { next = *s; next.connected = 1; memset(next.reserved, 0, sizeof next.reserved); }
    pthread_mutex_lock(&wb_b_pad_lock);
    next.packet = wb_b_pads[index].packet;
    if (memcmp(&next, &wb_b_pads[index], sizeof next)) { ++next.packet; wb_b_pads[index] = next; }
    pthread_mutex_unlock(&wb_b_pad_lock);
}
int winios_gamepad_get_state(int index, struct winios_gamepad *out) {
    struct winios_gamepad value = {0};
    if (index >= 0 && index < WINIOS_GAMEPAD_MAX) {
        pthread_mutex_lock(&wb_b_pad_lock);
        value = wb_b_pads[index];
        pthread_mutex_unlock(&wb_b_pad_lock);
    }
    if (!value.connected) memset(&value, 0, sizeof value);
    if (out) *out = value;
    return value.connected ? 1 : 0;
}
void winios_hidpad_set_state(const struct winios_hidpad *s) {
    struct winios_hidpad next = {0};
    if (s && s->connected) {
        next = *s; next.connected = 1;
        memset(next.reserved, 0, sizeof next.reserved); next.reserved2 = 0;
    }
    pthread_mutex_lock(&wb_b_hid_lock);
    next.packet = wb_b_hid.packet;
    if (memcmp(&next, &wb_b_hid, sizeof next)) { ++next.packet; wb_b_hid = next; }
    pthread_mutex_unlock(&wb_b_hid_lock);
}
int winios_hidpad_get_state(struct winios_hidpad *out) {
    pthread_mutex_lock(&wb_b_hid_lock);
    struct winios_hidpad value = wb_b_hid;
    pthread_mutex_unlock(&wb_b_hid_lock);
    if (!value.connected) {
        uint32_t packet = value.packet;
        memset(&value, 0, sizeof value); value.packet = packet;
    }
    if (out) *out = value;
    return value.connected ? 1 : 0;
}
void winios_hidpad_set_output(const struct winios_hidpad_output *s) {
    if (!s) return;
    struct winios_hidpad_output next = *s;
    memset(next.reserved, 0, sizeof next.reserved);
    pthread_mutex_lock(&wb_b_output_lock);
    next.serial = wb_b_output.serial;
    if (memcmp(&next, &wb_b_output, sizeof next)) { ++next.serial; wb_b_output = next; }
    pthread_mutex_unlock(&wb_b_output_lock);
}
void wb_b_get_hid_output(struct winios_hidpad_output *out) {
    if (!out) return;
    pthread_mutex_lock(&wb_b_output_lock); *out = wb_b_output; pthread_mutex_unlock(&wb_b_output_lock);
}
void winios_gamepad_set_vibration(int i, uint16_t left, uint16_t right) {
    if (i < 0 || i >= WINIOS_GAMEPAD_MAX) return;
    pthread_mutex_lock(&wb_b_vibration_lock);
    struct winios_gamepad_vibration *v = &wb_b_vibrations[i];
    if (!v->serial || v->left != left || v->right != right) {
        if (++v->serial == 0) ++v->serial;
        v->left = left; v->right = right;
    }
    pthread_mutex_unlock(&wb_b_vibration_lock);
}
void wb_b_get_vibration(int i, struct winios_gamepad_vibration *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= WINIOS_GAMEPAD_MAX) return;
    pthread_mutex_lock(&wb_b_vibration_lock); *out = wb_b_vibrations[i]; pthread_mutex_unlock(&wb_b_vibration_lock);
}
int winios_gamepad_rumble_caps(void) { return 0; }
