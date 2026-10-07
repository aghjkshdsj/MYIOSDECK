// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT step B: Wine loads an x64 emulator DLL (xtajit64.dll) into every ARM64EC process
// and calls it through this interface (dlls/ntdll/signal_arm64ec.c). With JIT that is FEX.
// Without JIT there is no x64 CPU yet (step D puts FXI here), so this stub accepts the
// process and thread setup, reports no emulated CPU features, and stops the process with
// status 0xE0F00001 if x64 code is ever entered. Enough for ARM64EC programs, which never
// leave native code.

typedef long NTSTATUS;
typedef unsigned long long SIZE_T;
typedef void *HANDLE;
#define EXPORT __declspec(dllexport)
#define WINAPI __stdcall

__declspec(dllimport) NTSTATUS WINAPI NtTerminateProcess(HANDLE process, NTSTATUS status);
__declspec(dllimport) unsigned long DbgPrint(const char *fmt, ...);

static void no_x64_cpu(const char *where) {
    DbgPrint("[xtajit64-stub] x64 code reached (%s): this build has no x64 CPU without JIT yet\n", where);
    NtTerminateProcess((HANDLE)-1, (NTSTATUS)0xE0F00001);
    for (;;) {}
}

EXPORT NTSTATUS WINAPI ProcessInit(void) { return 0; }
EXPORT void WINAPI ProcessTerm(HANDLE handle, int after, NTSTATUS status) { (void)handle; (void)after; (void)status; }
EXPORT NTSTATUS WINAPI ThreadInit(void) { return 0; }
EXPORT NTSTATUS WINAPI ThreadTerm(HANDLE thread, long status) { (void)thread; (void)status; return 0; }
EXPORT unsigned char WINAPI BTCpu64IsProcessorFeaturePresent(unsigned int feature) { (void)feature; return 0; }
EXPORT void WINAPI BTCpu64FlushInstructionCache(const void *addr, SIZE_T size) { (void)addr; (void)size; }
EXPORT void WINAPI FlushInstructionCacheHeavy(const void *addr, SIZE_T size) { (void)addr; (void)size; }
EXPORT void WINAPI BTCpu64NotifyMemoryDirty(void *addr, SIZE_T size) { (void)addr; (void)size; }
EXPORT void WINAPI BTCpu64NotifyReadFile(HANDLE f, void *addr, SIZE_T size, int after, NTSTATUS st) {
    (void)f; (void)addr; (void)size; (void)after; (void)st;
}
EXPORT void WINAPI NotifyMapViewOfSection(void *unk, void *addr, void *unk1, SIZE_T size, unsigned long alloc,
                                          unsigned long prot) {
    (void)unk; (void)addr; (void)unk1; (void)size; (void)alloc; (void)prot;
}
EXPORT void WINAPI NotifyUnmapViewOfSection(void *addr, int after, NTSTATUS st) { (void)addr; (void)after; (void)st; }
EXPORT void WINAPI NotifyMemoryAlloc(void *addr, SIZE_T size, unsigned long type, unsigned long prot, int after,
                                     NTSTATUS st) {
    (void)addr; (void)size; (void)type; (void)prot; (void)after; (void)st;
}
EXPORT void WINAPI NotifyMemoryFree(void *addr, SIZE_T size, unsigned long type, int after, NTSTATUS st) {
    (void)addr; (void)size; (void)type; (void)after; (void)st;
}
EXPORT void WINAPI NotifyMemoryProtect(void *addr, SIZE_T size, unsigned long prot, int after, NTSTATUS st) {
    (void)addr; (void)size; (void)prot; (void)after; (void)st;
}
EXPORT NTSTATUS WINAPI ResetToConsistentState(void *ptrs, void *ctx, void *x64ctx) {
    (void)ptrs; (void)ctx; (void)x64ctx;
    return 0;
}
EXPORT void WINAPI UpdateProcessorInformation(void *info) { (void)info; }

// Entering x64 code: the ARM64EC dispatch points (special register conventions; they never
// return here) and the emulation loop.
EXPORT void WINAPI BeginSimulation(void) { no_x64_cpu("BeginSimulation"); }
EXPORT void ExitToX64(void) { no_x64_cpu("ExitToX64"); }
EXPORT void DispatchJump(void) { no_x64_cpu("DispatchJump"); }
EXPORT void RetToEntryThunk(void) { no_x64_cpu("RetToEntryThunk"); }

int WINAPI DllMainCRTStartup(void *module, unsigned reason, void *reserved) {
    (void)module; (void)reason; (void)reserved;
    return 1;
}
