// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part C "Exit status", and winios_phase.

#include "wb_internal.h"
#include "wb_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define EXIT_ERROR_FLAG (UINT64_C(1) << 32)

/* 0: nothing stored. Otherwise EXIT_ERROR_FLAG | status. */
static _Atomic uint64_t g_exit_status;

void wine_launched_process_did_exit(int status)
{
    if ((uint32_t)status >= 0xC0000000u)
        atomic_store_explicit(&g_exit_status, EXIT_ERROR_FLAG | (uint32_t)status, memory_order_release);
}

void wb_exit_status_reset(void)
{
    atomic_store_explicit(&g_exit_status, 0, memory_order_release);
}

int wb_crash_exit_status(uint32_t *status)
{
    uint64_t v = atomic_load_explicit(&g_exit_status, memory_order_acquire);

    if (!(v & EXIT_ERROR_FLAG)) return 0;
    if (status) *status = (uint32_t)v;
    return 1;
}

int wine_crash_exit_status(uint32_t *status)
{
    return wb_crash_exit_status(status);
}

static pthread_once_t g_phase_once = PTHREAD_ONCE_INIT;
static struct timespec g_phase_start;

static void phase_start(void)
{
    clock_gettime(CLOCK_MONOTONIC, &g_phase_start);
}

void winios_phase(const char *name)
{
    struct timespec now;
    double secs;

    pthread_once(&g_phase_once, phase_start);
    clock_gettime(CLOCK_MONOTONIC, &now);
    secs = (double)(now.tv_sec - g_phase_start.tv_sec) +
           (double)(now.tv_nsec - g_phase_start.tv_nsec) / 1e9;
    wb_log("phase", "%s t+%.3fs", name ? name : "(null)", secs);
}
