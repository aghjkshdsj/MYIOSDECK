// SPDX-License-Identifier: GPL-3.0-or-later
// FXR as Wine's x64 CPU (experimental, Settings › Without JIT): FXR's Windows-mode entry points
// (engine/fxr/fxi_win.c, renamed fxr_win_*), wrapped with opaque CPU pointers for
// fxi_win_host.c, which cannot include FXR's header next to FXI's (same type names).

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "../../../engine/fxr/fxi.h"   // FXR's API, renamed to fxr_* (fxr_rename.h)

static pthread_once_t g_index_once = PTHREAD_ONCE_INIT;
static void build_index(void) { fxr_win_index(); }

void *mid_fxr_win_cpu_new(void) {
    pthread_once(&g_index_once, build_index);   // before any fault or stop (not async-signal-safe)
    return fxr_win_cpu_new();
}
uint64_t mid_fxr_win_run(void *c) { return fxr_win_run(c); }
const char *mid_fxr_win_error(void *c) { return fxr_win_error(c); }
void mid_fxr_win_set_teb(void *c, uint64_t teb) { fxr_win_set_teb(c, teb); }
uint64_t mid_fxr_win_rip(void *c) { return fxr_win_rip(c); }
void mid_fxr_win_load_context(void *c, const void *ctx) { fxr_win_load_context(c, ctx); }
void mid_fxr_win_save_context(void *c, void *ctx, uint64_t rip) { fxr_win_save_context(c, ctx, rip); }
int mid_fxr_win_exception(void *c, void *e) { return fxr_win_exception(c, e); }
uint64_t mid_fxr_win_fault_rip(void *c, int *is_fetch) { return fxr_win_fault_rip(c, is_fetch); }
int mid_fxr_win_trail(void *c, uint64_t *rips, int max) { return fxr_win_trail(c, rips, max); }
int mid_fxr_win_df_source(void *c, uint64_t *rip) { return fxr_win_df_source(c, rip); }
uint64_t mid_fxr_win_profile(void *c, uint64_t *lookups, uint64_t *exits, uint64_t *blocks) {
    return fxr_win_profile(c, lookups, exits, blocks);
}
int mid_fxr_win_exit_counts(void *c, uint64_t *targets, uint64_t *counts, int max) {
    return fxr_win_exit_counts(c, targets, counts, max);
}
// Native functions FXR runs itself: kind(target) 1 TlsGetValue, 2 RtlEnterCriticalSection,
// 3 RtlLeaveCriticalSection, 0 none.
void mid_fxr_win_set_native(int (*kind)(uint64_t target)) { fxr_win_set_native(kind); }
// Profiler: where a stopped thread's time goes (FXR_WS_*: 1 pinned handler, 2 FXI's handler for
// *rip, 3 block lookup, 4 entering from native code, 0 unknown), its counters, a handler's name.
int mid_fxr_win_sample(void *c, uint64_t pc, uint64_t x21, int (*rd)(uint64_t, void *, size_t), uint64_t *rip,
                       const void **fn) {
    return fxr_win_sample(c, pc, x21, rd, rip, fn);
}
void mid_fxr_win_counters(void *c, uint64_t out[4]) { fxr_win_counters(c, out); }
const char *mid_fxr_win_op_name(const void *fn, char *buf, size_t n) { return fxr_win_op_name(fn, buf, n); }

// The x64 state from the host's registers of the thread running c (x0-x30, sp, pc, q0-q7):
// fault = 1 at a host fault, 0 at a stop. 1 = exact, the CPU loaded and *rip set; *pc may move.
int mid_fxr_win_host_state(void *c, const uint64_t *x31, uint64_t sp, uint64_t *pc, const void *q8x16, int fault,
                           uint64_t *rip) {
    fxr_host_state h;
    memcpy(h.x, x31, sizeof h.x);
    h.sp = sp;
    h.pc = *pc;
    memcpy(h.q, q8x16, sizeof h.q);
    int ok = fxr_win_host_state(c, &h, fault, rip);
    *pc = h.pc;
    return ok;
}
