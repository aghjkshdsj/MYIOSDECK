// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part F: the controller stores the unix side reads (contract Sections B and C).
// Any thread, possibly every frame: each call only copies under its own mutex and never
// calls out while holding it.

#include "wb_internal.h"

#include <pthread.h>
#include <string.h>

_Static_assert(sizeof(struct winios_gamepad) == 20, "contract: 20 bytes");
_Static_assert(sizeof(struct winios_hidpad) == 48, "contract: 48 bytes");
_Static_assert(sizeof(struct winios_hidpad_output) == 44, "contract: 44 bytes");
_Static_assert(sizeof(struct winios_gamepad_vibration) == 8, "contract: 8 bytes");

/* ---- XInput slots ---- */

static pthread_mutex_t g_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winios_gamepad g_pads[WINIOS_GAMEPAD_MAX];

void winios_gamepad_set_state(int index, const struct winios_gamepad *state)
{
    struct winios_gamepad nv;

    if (index < 0 || index >= WINIOS_GAMEPAD_MAX) return;
    memset(&nv, 0, sizeof(nv));
    if (state && state->connected) {
        nv = *state;
        nv.connected = 1;
        memset(nv.reserved, 0, sizeof(nv.reserved));
    }

    pthread_mutex_lock(&g_pad_lock);
    nv.packet = g_pads[index].packet;
    if (memcmp(&nv, &g_pads[index], sizeof(nv)) != 0) {
        nv.packet++;
        g_pads[index] = nv;
    }
    pthread_mutex_unlock(&g_pad_lock);
}

int winios_gamepad_get_state(int index, struct winios_gamepad *out)
{
    struct winios_gamepad cur;

    if (index < 0 || index >= WINIOS_GAMEPAD_MAX) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    pthread_mutex_lock(&g_pad_lock);
    cur = g_pads[index];
    pthread_mutex_unlock(&g_pad_lock);

    if (!cur.connected) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    if (out) *out = cur;
    return 1;
}

/* ---- XInput rumble ---- */

static pthread_mutex_t g_vib_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winios_gamepad_vibration g_vib[WINIOS_GAMEPAD_MAX];

void winios_gamepad_set_vibration(int index, uint16_t left, uint16_t right)
{
    struct winios_gamepad_vibration *v;

    if (index < 0 || index >= WINIOS_GAMEPAD_MAX) return;
    pthread_mutex_lock(&g_vib_lock);
    v = &g_vib[index];
    if (v->serial == 0 || v->left != left || v->right != right) {
        v->left = left;
        v->right = right;
        v->serial++;
        if (v->serial == 0) v->serial = 1;
    }
    pthread_mutex_unlock(&g_vib_lock);
}

void wb_gamepad_get_vibration(int index, struct winios_gamepad_vibration *out)
{
    if (!out) return;
    if (index < 0 || index >= WINIOS_GAMEPAD_MAX) {
        memset(out, 0, sizeof(*out));
        return;
    }
    pthread_mutex_lock(&g_vib_lock);
    *out = g_vib[index];
    pthread_mutex_unlock(&g_vib_lock);
}

int winios_gamepad_rumble_caps(void)
{
    /* Rumble is not applied to physical pads yet, so games must not see motors. */
    return 0;
}

/* ---- HID gamepad ---- */

static pthread_mutex_t g_hid_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winios_hidpad g_hid;

void winios_hidpad_set_state(const struct winios_hidpad *state)
{
    struct winios_hidpad nv;

    memset(&nv, 0, sizeof(nv));
    if (state && state->connected) {
        nv = *state;
        nv.connected = 1;
        memset(nv.reserved, 0, sizeof(nv.reserved));
        nv.reserved2 = 0;
    }

    pthread_mutex_lock(&g_hid_lock);
    nv.packet = g_hid.packet;
    if (memcmp(&nv, &g_hid, sizeof(nv)) != 0) {
        nv.packet++;
        g_hid = nv;
    }
    pthread_mutex_unlock(&g_hid_lock);
}

int winios_hidpad_get_state(struct winios_hidpad *out)
{
    struct winios_hidpad cur;

    pthread_mutex_lock(&g_hid_lock);
    cur = g_hid;
    pthread_mutex_unlock(&g_hid_lock);

    if (!cur.connected) {
        if (out) {
            /* Keep the packet: the reader must see a disconnect as a change. */
            uint32_t packet = cur.packet;
            memset(out, 0, sizeof(*out));
            out->packet = packet;
        }
        return 0;
    }
    if (out) *out = cur;
    return 1;
}

/* ---- HID gamepad output ---- */

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static struct winios_hidpad_output g_out;

void winios_hidpad_set_output(const struct winios_hidpad_output *output)
{
    struct winios_hidpad_output nv;

    if (!output) return;
    nv = *output;
    memset(nv.reserved, 0, sizeof(nv.reserved));

    pthread_mutex_lock(&g_out_lock);
    nv.serial = g_out.serial;
    if (memcmp(&nv, &g_out, sizeof(nv)) != 0) {
        nv.serial++;
        g_out = nv;
    }
    pthread_mutex_unlock(&g_out_lock);
}

void wb_hidpad_get_output(struct winios_hidpad_output *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_out_lock);
    *out = g_out;
    pthread_mutex_unlock(&g_out_lock);
}
