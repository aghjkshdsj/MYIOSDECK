// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK JIT core: debugger-backed executable memory for iOS 26/27 (TXM).
//
// iOS only lets a process execute freshly written code when a debugger is
// attached (CS_DEBUGGED). On TXM devices (A15 and newer, e.g. iPhone 15 Pro
// Max) every executable region must also be "prepared" through the debug
// connection. StikDebug runs our script (Resources/myiosdeck-jit.js), which
// answers the BRK #0xf00d protocol below.
//
// Design follows Madeira's JITAllocator (github.com/willfaust/Madeira,
// GPL-3.0-or-later): one large RX pool allocated by the debugger, a vm_remap'd
// RW alias for writing, and a bump allocator on top. FEX writes through the
// alias and executes from the RX view (W^X is never violated).

#ifndef MYIOSDECK_JIT_CORE_H
#define MYIOSDECK_JIT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mid_log_fn)(const char *line);

/// All native layers log through this one sink (forwarded to the Swift log store).
void mid_set_log_sink(mid_log_fn sink);
void mid_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// --- Code-signing state -----------------------------------------------------

/// csops(CS_OPS_STATUS) flags; 0 if the kernel refuses.
uint32_t mid_cs_flags(void);
/// CS_DEBUGGED is set: a debugger attached at least once this run.
bool mid_jit_debugged(void);
/// get-task-allow is present in the signature (needed for any debugger to attach).
bool mid_has_get_task_allow(void);

// --- BRK #0xf00d protocol (StikDebug universal protocol) -------------------

/// x16 = 1. addr NULL asks the debugger to allocate an RX mapping of len bytes.
/// Returns the prepared RX address, or NULL when nobody answered.
void *mid_jit26_prepare_region(void *addr, size_t len);
/// x16 = 0. Tells the script to detach; JIT pages stay executable afterwards.
void mid_jit26_detach(void);
/// SIGTRAP fallback so an unanswered BRK #0xf00d returns 0 instead of killing us.
void mid_jit_arm_trap_fallback(void);

// --- The JIT pool -------------------------------------------------------------

/// Allocate the pool (debugger RX + RW alias). Must run while the debugger is
/// attached. Returns false and fills err on failure. Idempotent.
bool mid_jit_pool_create(size_t size, char *err, size_t errlen);
bool mid_jit_pool_ready(void);
void *mid_jit_pool_rx_base(void);
void *mid_jit_pool_rw_base(void);
size_t mid_jit_pool_size(void);
size_t mid_jit_pool_used(void);
/// RW address minus RX address (FEXCore::DualMap::WriteOffset).
int64_t mid_jit_pool_write_offset(void);
/// Bump-allocate page-aligned RX memory from the pool. NULL when exhausted.
void *mid_jit_pool_alloc(size_t size);
/// Executes `mov x0,#42; ret` from the pool. Returns 42 when JIT really works.
int64_t mid_jit_selftest(void);

// --- Memory ---------------------------------------------------------------------

/// os_proc_available_memory(): bytes this process can still allocate before jetsam.
uint64_t mid_available_memory(void);
/// phys_footprint from TASK_VM_INFO.
uint64_t mid_phys_footprint(void);
/// Highest user address of this task's map (63 GB standard, 512 GB extended).
uint64_t mid_task_max_address(void);

#ifdef __cplusplus
}
#endif

#endif
