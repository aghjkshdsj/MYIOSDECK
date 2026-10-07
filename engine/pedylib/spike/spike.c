// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT feasibility spike (docs/NO_JIT_WINDOWS.md, milestone A): a Windows DLL built
// the way Wine's ARM64EC DLLs are (llvm-mingw, arm64ec-w64-mingw32) that MYIOSDECK
// runs on iPhone from a signed dylib, with no JIT. It exercises what a real Wine
// DLL needs from the loader:
//   - code in .text calling across sections with ADRP (.data, .rdata, .bss)
//   - writable globals (.data / .bss)
//   - pointer tables in .rdata that need base relocations (bk_kernels[])
//   - imports through the IAT from another "DLL" (the app: myiosdeck.dll)
//   - ARM64EC indirect calls through __os_arm64x_check_icall (function pointers,
//     import thunks), which the loader fills from the CHPE metadata
// and the same benchmark kernels as the Performance tab, so its speed can be
// compared with the app's own native build of them.

#include "bench_kernels.h"

#define EXPORT __declspec(dllexport)

// Provided by the app (engine/pedylib/spike/myiosdeck.def -> import library).
__declspec(dllimport) void host_log(const char *line);
__declspec(dllimport) int host_twice(int x);

static int spike_counter_value = 41;                   // .data
static const char *const spike_words[] = { "signed", "dylib", "no", "jit" }; // .rdata + DIR64 relocs

EXPORT int spike_add(int a, int b) { return a + b; }

EXPORT int spike_counter(void) { return ++spike_counter_value; }

EXPORT int spike_words_len(void) {
    int n = 0;
    for (unsigned i = 0; i < sizeof spike_words / sizeof spike_words[0]; i++)
        for (const char *p = spike_words[i]; *p; p++) n++;
    return n; // 6 + 5 + 2 + 3 = 16
}

// Import call through the IAT (ARM64EC: aux IAT thunk -> __os_arm64x_check_icall).
EXPORT int spike_import(int x) {
    host_log("hello from a Windows DLL running from a signed dylib");
    return host_twice(x);
}

// Indirect call through a function pointer from the host.
EXPORT int spike_callback(int (*cb)(int), int x) { return cb(x) + 1; }

EXPORT int spike_kernel_count(void) { return (int)BK_KERNEL_COUNT; }

// Indirect call through the relocated .rdata table.
EXPORT bk_u64 spike_kernel(int i, bk_u32 scale) { return bk_kernels[i].fn(scale); }

// Default entry point name for a mingw DLL linked with -nostdlib.
int __stdcall DllMainCRTStartup(void *module, unsigned reason, void *reserved) {
    (void)module; (void)reason; (void)reserved;
    return 1;
}
