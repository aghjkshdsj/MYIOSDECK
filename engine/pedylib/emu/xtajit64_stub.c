// SPDX-License-Identifier: GPL-3.0-or-later
// The x64 emulator DLL Wine loads into every ARM64EC process (as xtajit64.dll; FEX with JIT).
// Without JIT this is FXI's front end (docs/NO_JIT_WINDOWS.md step D): Wine's emulator
// interface (dlls/ntdll/signal_arm64ec.c) forwarded to the app, which interprets the x64 code
// with FXI (App/Sources/Native/fxi_win_glue.S, fxi_win_host.c).
//
// The app's table of entry points reaches this DLL through MyiosdeckFxiHost: Wine's no-JIT
// map hook (engine/wine/patches/nojit_dylib.py) stores it when it maps this image.
//
// ExitToX64, DispatchJump, RetToEntryThunk and BeginSimulation have register-level
// conventions (x9 = target, lr = return address, ...) and are exported as DATA, as FEX does,
// so the loader hands out their real address rather than an x64 entry thunk: each is a jump
// through the host table that keeps every register except x16.

typedef long NTSTATUS;
typedef unsigned long long SIZE_T;
typedef void *HANDLE;
#define EXPORT __declspec(dllexport)
#define WINAPI __stdcall

__declspec(dllimport) unsigned long DbgPrint(const char *fmt, ...);

// Filled by the map hook: mid_fxi_win_host (fxi_win_host.c).
EXPORT void **MyiosdeckFxiHost;

enum { H_EXIT_TO_X64, H_DISPATCH_JUMP, H_RET_TO_ENTRY_THUNK, H_BEGIN_SIMULATION, H_PROCESS_INIT,
       H_THREAD_INIT, H_FEATURE_PRESENT, H_RESET_TO_CONSISTENT };

// The app raises x64 exceptions through ntdll's KiUserExceptionDispatcher (fxi_win_host.c).
__declspec(dllimport) void KiUserExceptionDispatcher(void);

#define DISPATCH(name, slot)                                       \
    __asm__(".text\n"                                              \
            ".p2align 2\n"                                         \
            ".globl " #name "\n"                                   \
            #name ":\n"                                            \
            "adrp x16, MyiosdeckFxiHost\n"                         \
            "ldr x16, [x16, #:lo12:MyiosdeckFxiHost]\n"            \
            "ldr x16, [x16, #" #slot "]\n"                         \
            "br x16\n"                                             \
            ".section .drectve\n"                                  \
            ".ascii \" -export:" #name ",DATA\"\n"                 \
            ".text\n")
DISPATCH(ExitToX64, 0);
DISPATCH(DispatchJump, 8);
DISPATCH(RetToEntryThunk, 16);
DISPATCH(BeginSimulation, 24);

// Call a native app function (Apple arm64 ABI) with up to two arguments. A plain blr: an
// ARM64EC indirect call would go through __os_arm64x_check_icall, which treats non-EC code as
// x64. Arguments are 64-bit (long long): Windows' long is 32 bits, and build 63 cut the
// exception dispatcher's address to its low half that way.
typedef long long hostarg;
static hostarg host_call2(int slot, hostarg arg0, hostarg arg1) {
    if (!MyiosdeckFxiHost || !MyiosdeckFxiHost[slot]) return (hostarg)(long)0xC0000001;
    register hostarg x0 __asm__("x0") = arg0;
    register hostarg x1 __asm__("x1") = arg1;
    register void *x16 __asm__("x16") = MyiosdeckFxiHost[slot];
    __asm__ volatile("blr x16"
                     : "+r"(x0), "+r"(x1), "+r"(x16)
                     :
                     : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x15", "x17",
                       "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory", "cc");
    return x0;
}
static hostarg host_call(int slot, hostarg arg) { return host_call2(slot, arg, 0); }

// ARM64EC exports are x64 "fast-forward" thunks (mov rax,rsp; mov [rax+20],rbx; push rbp;
// pop rbp; jmp rel32) whose jmp leads to the native function: native callers follow it.
static void *native_entry(void *export_addr) {
    const volatile unsigned char *p = export_addr;
    const volatile unsigned long long *q = (const volatile unsigned long long *)p;
    if (q[0] == 0x5520588948c48b48ull && p[8] == 0x5d && p[9] == 0xe9) {
        int rel = (int)((unsigned)p[10] | (unsigned)p[11] << 8 | (unsigned)p[12] << 16 | (unsigned)p[13] << 24);
        return (void *)(p + 14 + rel);
    }
    return export_addr;
}

EXPORT NTSTATUS WINAPI ProcessInit(void) {
    if (!MyiosdeckFxiHost) {
        DbgPrint("[xtajit64-fxi] no host table: this process cannot run x64 code without JIT\n");
        return (NTSTATUS)0xC0000001;
    }
    return (NTSTATUS)host_call(H_PROCESS_INIT, (hostarg)native_entry((void *)KiUserExceptionDispatcher));
}
EXPORT NTSTATUS WINAPI ThreadInit(void) { return host_call(H_THREAD_INIT, 0); }
EXPORT unsigned char WINAPI BTCpu64IsProcessorFeaturePresent(unsigned int feature) {
    return host_call(H_FEATURE_PRESENT, feature) != 0;
}

EXPORT void WINAPI ProcessTerm(HANDLE handle, int after, NTSTATUS status) { (void)handle; (void)after; (void)status; }
EXPORT NTSTATUS WINAPI ThreadTerm(HANDLE thread, long status) { (void)thread; (void)status; return 0; }
// FXI keeps no translated code, so nothing to flush or invalidate yet (self-modifying x64
// code is a later step).
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
// Wine calls this for every exception before dispatching it: a fault inside FXI does not
// return (the app re-raises it as an exception of the x64 instruction, on the guest stack).
EXPORT NTSTATUS WINAPI ResetToConsistentState(void *rec, void *x64ctx, void *arm_ctx) {
    (void)x64ctx;
    host_call2(H_RESET_TO_CONSISTENT, (hostarg)rec, (hostarg)arm_ctx);
    return 0;
}
EXPORT void WINAPI UpdateProcessorInformation(void *info) { (void)info; }

int WINAPI DllMainCRTStartup(void *module, unsigned reason, void *reserved) {
    (void)module; (void)reason; (void)reserved;
    return 1;
}
