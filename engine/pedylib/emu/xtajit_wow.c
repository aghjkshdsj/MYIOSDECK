// SPDX-License-Identifier: GPL-3.0-or-later
// The WoW64 CPU module Wine loads into every 32-bit process (as xtajit.dll; FEX with JIT).
// Without JIT this is FXI32's front end (docs/NO_JIT_WOW64.md, stage 3): wow64.dll's BTCpu*
// interface, with the x86 code interpreted by FXI32 in the app (App/Sources/Native/fxi_wow_host.c).
//
// The contract is Wine's dlls/wow64/syscall.c and Madeira's FEX WOW64 module
// (Source/Windows/WOW64/Module.cpp, MIT; Thanks to André Zwing's hangover for the original
// ideas): guest addresses below 4 GB in the process's window [B, B + 4 GB), the BOP page whose
// guest address wow64 stores in the 32-bit ntdll, the CPU area as the 32-bit register state
// outside x86 code, and a frame trampoline (MyiosdeckWowCall, FEX's SEHFrameTrampoline2Args)
// so Windows unwinding steps from wow64 straight back to BTCpuSimulate over the app's frames.
//
// The 64-bit half of a WoW64 process loads only ntdll, wow64, wow64win and this module, so this
// imports ntdll alone (wow64's functions are looked up at process init).
//
// The app's table reaches this DLL through MyiosdeckWowHost: Wine's no-JIT map hook
// (engine/wine/patches/nojit_dylib.py) stores it when it maps this image.

#include <stdint.h>

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint64_t SIZE_T, ULONG_PTR;
typedef void *HANDLE;
#define EXPORT __declspec(dllexport)
#define WINAPI
#define CURRENT_PROCESS ((HANDLE)~(ULONG_PTR)0)
#define CURRENT_THREAD ((HANDLE)~(ULONG_PTR)1)

typedef struct { USHORT Length, MaximumLength; uint16_t *Buffer; } UNICODE_STRING;
typedef struct { USHORT Length, MaximumLength; char *Buffer; } ANSI_STRING;

__declspec(dllimport) unsigned long DbgPrint(const char *fmt, ...);
__declspec(dllimport) NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, void *, ULONG, ULONG *);
__declspec(dllimport) NTSTATUS NtAllocateVirtualMemory(HANDLE, void **, ULONG_PTR, SIZE_T *, ULONG, ULONG);
__declspec(dllimport) NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
__declspec(dllimport) NTSTATUS NtSuspendThread(HANDLE, ULONG *);
__declspec(dllimport) NTSTATUS NtQueryVirtualMemory(HANDLE, const void *, int, void *, SIZE_T, SIZE_T *);
__declspec(dllimport) NTSTATUS RtlWow64GetThreadContext(HANDLE, void *);
__declspec(dllimport) NTSTATUS RtlWow64SetThreadContext(HANDLE, const void *);
__declspec(dllimport) NTSTATUS RtlWow64GetCurrentCpuArea(USHORT *, void **, void **);
__declspec(dllimport) NTSTATUS LdrGetDllHandle(const uint16_t *, ULONG, const UNICODE_STRING *, void **);
__declspec(dllimport) NTSTATUS LdrGetProcedureAddress(void *, const ANSI_STRING *, ULONG, void **);
// (the import library: xtajit_wow_ntdll.def; RtlCaptureContext is called from BTCpuSimulate's asm)

// Filled by the map hook: mid_fxi_wow_host_table (fxi_wow_host.c).
EXPORT void **MyiosdeckWowHost;

enum { H_PROCESS_INIT, H_THREAD_INIT, H_THREAD_TERM, H_SIMULATE, H_RESET, H_INVALIDATE, H_FEATURE, H_CPU_INFO };

// What the app needs from Windows, handed over once (H_PROCESS_INIT). Layout shared with
// fxi_wow_host.c (struct wow_process).
struct wow_process {
    uint64_t base;                       // B: host address of guest 0
    uint64_t bop;                        // guest address of the system-call entry (+2: unix calls)
    void *call;                          // MyiosdeckWowCall
    void *system_service;                // wow64!Wow64SystemServiceEx(UINT num, UINT *args)
    void *pass_exception;                // wow64!Wow64PassExceptionToGuest(EXCEPTION_POINTERS *)
    void *raise_exception;               // wow64!Wow64RaiseException(int code, EXCEPTION_RECORD *)
    void *pending_items;                 // wow64!Wow64ProcessPendingCrossProcessItems(void)
    void *unix_call;                     // ntdll's __wine_unix_call_dispatcher (handle, code, args)
    uint32_t tsd_offset;                 // ntdll!ios_teb_tsd_offset (diagnostics)
};

typedef long long hostarg;
static hostarg host_call3(int slot, hostarg a0, hostarg a1, hostarg a2) {
    if (!MyiosdeckWowHost || !MyiosdeckWowHost[slot]) return (hostarg)(long long)(int32_t)0xC0000001;
    register hostarg x0 __asm__("x0") = a0;
    register hostarg x1 __asm__("x1") = a1;
    register hostarg x2 __asm__("x2") = a2;
    register void *x16 __asm__("x16") = MyiosdeckWowHost[slot];
    __asm__ volatile("blr x16"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16)
                     :
                     : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17",
                       "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20",
                       "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory", "cc");
    return x0;
}
static hostarg host_call(int slot, hostarg a0) { return host_call3(slot, a0, 0, 0); }

static void *teb64(void) {
    void *t;
    __asm__("mov %0, x18" : "=r"(t));   // rewritten at build time into a TSD read (pe2dylib.py)
    return t;
}
#define TLS_SLOT(teb, i) (((void **)((char *)(teb) + 0x1480))[i])
enum { WOW64_TLS_CPURESERVED = 1, WOW64_TLS_WOW64INFO = 10 };

static struct wow_process g_proc;

static void fatal(const char *what, NTSTATUS st) {
    DbgPrint("[xtajit-fxi] %s (status %#x): this 32-bit program cannot run without JIT\n", what, (unsigned)st);
    NtTerminateProcess(CURRENT_PROCESS, (NTSTATUS)0xE0F0F002);
}

static void *module(const char *name) {
    uint16_t w[32];
    unsigned n = 0;
    while (name[n] && n < 31) { w[n] = (uint16_t)name[n]; n++; }
    w[n] = 0;
    UNICODE_STRING s = { (USHORT)(n * 2), (USHORT)(n * 2 + 2), w };
    void *h = 0;
    return LdrGetDllHandle(0, 0, &s, &h) ? 0 : h;
}
static void *proc(void *mod, const char *name) {
    unsigned n = 0;
    while (name[n]) n++;
    ANSI_STRING s = { (USHORT)n, (USHORT)(n + 1), (char *)name };
    void *p = 0;
    return mod && !LdrGetProcedureAddress(mod, &s, 0, &p) ? p : 0;
}

// The trampoline every call from the app into Windows code goes through (FEX's
// SEHFrameTrampoline2Args): fn(a0, a1, a2) with an unwind frame whose caller is the context
// BTCpuSimulate captured, so an unwind (wow64's longjmp from NtCallbackReturn, an exception
// dispatch) continues in BTCpuSimulate, not in the app's frames.
//   x0 fn, x1-x3 arguments, x4 the captured CONTEXT (Sp at 0x100, Pc at 0x108)
hostarg MyiosdeckWowCall(void *fn, hostarg a0, hostarg a1, hostarg a2, void *entry_ctx);
__asm__(".section .drectve\n"
        ".ascii \" -export:MyiosdeckWowCall -export:BTCpuSimulate\"\n"
        ".text\n"
        ".globl MyiosdeckWowCall\n"
        ".p2align 2\n"
        ".seh_proc MyiosdeckWowCall\n"
        "MyiosdeckWowCall:\n"
        "  ldr x5, [x4, #0x100]\n"
        "  ldr x6, [x4, #0x108]\n"
        "  stp x5, x6, [sp, #-16]!\n"
        "  .seh_pushframe\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  .seh_save_fplr_x 16\n"
        "  .seh_endprologue\n"
        "  mov x16, x0\n"
        "  mov x0, x1\n"
        "  mov x1, x2\n"
        "  mov x2, x3\n"
        "  blr x16\n"
        "  ldp x29, x30, [sp], #32\n"
        "  ret\n"
        ".seh_endproc\n");

EXPORT void WINAPI BTCpuProcessInit(void) {
    if (!MyiosdeckWowHost) fatal("no host table (the map hook did not install it)", 0);
    NTSTATUS st;
    ULONG_PTR base = 0;
    if ((st = NtQueryInformationProcess(CURRENT_PROCESS, 1010 /* ProcessWineIosWowGuestBase */, &base, sizeof base, 0)) || !base)
        fatal("no guest window (ProcessWineIosWowGuestBase)", st);
    // The BOP page: a guest ceiling below 2 GB puts it inside the window; read/write data,
    // never executable (FXI32 recognises its two addresses before decoding).
    void *addr = 0;
    SIZE_T size = 4;
    if ((st = NtAllocateVirtualMemory(CURRENT_PROCESS, &addr, 0x7fffffff, &size, 0x3000 /* RESERVE|COMMIT */, 0x04 /* RW */)) || !addr)
        fatal("BOP page allocation failed", st);
    if ((ULONG_PTR)addr < base || (ULONG_PTR)addr - base >= (1ull << 32)) fatal("BOP page outside the guest window", 0);
    *(volatile uint32_t *)addr = 0x2ecd2ecd;
    g_proc.base = base;
    g_proc.bop = (ULONG_PTR)addr - base;
    g_proc.call = (void *)MyiosdeckWowCall;
    void *wow64 = module("wow64.dll"), *ntdll = module("ntdll.dll");
    g_proc.system_service = proc(wow64, "Wow64SystemServiceEx");
    g_proc.pass_exception = proc(wow64, "Wow64PassExceptionToGuest");
    g_proc.raise_exception = proc(wow64, "Wow64RaiseException");
    g_proc.pending_items = proc(wow64, "Wow64ProcessPendingCrossProcessItems");
    void **dispatcher = proc(ntdll, "__wine_unix_call_dispatcher");
    g_proc.unix_call = dispatcher ? *dispatcher : 0;
    uint32_t *tsd = proc(ntdll, "ios_teb_tsd_offset");
    g_proc.tsd_offset = tsd ? *tsd : 0;
    if (!g_proc.system_service || !g_proc.pass_exception || !g_proc.raise_exception || !g_proc.unix_call)
        fatal("wow64.dll / ntdll exports missing", 0);
    // Software CPU: wow64 then keeps the cross-process work list (memory changes made by other
    // processes, applied before each system call).
    uint32_t *info = TLS_SLOT(teb64(), WOW64_TLS_WOW64INFO);
    if (info) info[1] = 0x02;   // WOW64INFO.CpuFlags = WOW64_CPUFLAGS_SOFTWARE
    DbgPrint("[xtajit-fxi] guest window B=%p, BOP page at guest %#x\n", (void *)base, (unsigned)g_proc.bop);
    if (host_call(H_PROCESS_INIT, (hostarg)&g_proc)) fatal("the app refused the process", 0);
}

EXPORT void WINAPI BTCpuProcessTerm(HANDLE handle, int after, NTSTATUS status) { (void)handle; (void)after; (void)status; }

// The thread's CPU area (WOW64_CPURESERVED + I386_CONTEXT) is the hand-over: Wine's unix side
// put the thread's initial context there.
EXPORT void WINAPI BTCpuThreadInit(void) {
    void *teb = teb64(), *ctx = 0;
    USHORT machine = 0;
    NTSTATUS st = RtlWow64GetCurrentCpuArea(&machine, &ctx, 0);
    if (st || machine != 0x14c) fatal("no i386 CPU area on this thread", st);
    if (host_call3(H_THREAD_INIT, (hostarg)&g_proc, (hostarg)teb, (hostarg)TLS_SLOT(teb, WOW64_TLS_CPURESERVED)))
        fatal("the app could not set up this thread's CPU", 0);
}

EXPORT void WINAPI BTCpuThreadTerm(HANDLE thread, int32_t code) {
    (void)code;
    if (thread == CURRENT_THREAD) host_call(H_THREAD_TERM, (hostarg)teb64());
}

EXPORT void *WINAPI BTCpuGetBopCode(void) { return (void *)(ULONG_PTR)g_proc.bop; }
EXPORT void *WINAPI __wine_get_unix_opcode(void) { return (void *)(ULONG_PTR)(g_proc.bop + 2); }

// Every call comes while this thread is outside x86 code (a system call, a callback, exception
// dispatch, thread start), when the CPU area holds the exact state: Wine's functions suffice.
EXPORT NTSTATUS WINAPI BTCpuGetContext(HANDLE thread, HANDLE process, void *unknown, void *ctx) {
    (void)process; (void)unknown;
    return RtlWow64GetThreadContext(thread, ctx);
}
// A new state for this thread: the app reloads it from the CPU area before the next x86
// instruction (WOW64_CPURESERVED_FLAG_RESET_STATE, as wow64cpu reads it).
EXPORT NTSTATUS WINAPI BTCpuSetContext(HANDLE thread, HANDLE process, void *unknown, void *ctx) {
    (void)process; (void)unknown;
    NTSTATUS st = RtlWow64SetThreadContext(thread, ctx);
    if (!st && thread == CURRENT_THREAD) {
        volatile USHORT *flags = TLS_SLOT(teb64(), WOW64_TLS_CPURESERVED);
        if (flags) __atomic_fetch_or(flags, (USHORT)1, __ATOMIC_ACQ_REL);
    }
    return st;
}

// Run x86 code until wow64 is needed. The context captured here is where Windows unwinding
// resumes when it crosses the app's frames (MyiosdeckWowCall); the app loops inside.
__attribute__((used)) static void simulate(void *entry_ctx) { host_call(H_SIMULATE, (hostarg)entry_ctx); }
void WINAPI BTCpuSimulate(void);   // exported by the .drectve directive above
__asm__(".text\n"
        ".globl BTCpuSimulate\n"
        ".p2align 2\n"
        ".seh_proc BTCpuSimulate\n"
        "BTCpuSimulate:\n"
        "  sub sp, sp, #0x390\n"
        "  .seh_stackalloc 0x390\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  .seh_save_fplr_x 16\n"
        "  .seh_endprologue\n"
        "  add x0, sp, #16\n"
        "  bl RtlCaptureContext\n"
        "  add x0, sp, #16\n"
        "  bl simulate\n"
        "  ldp x29, x30, [sp], #16\n"
        "  add sp, sp, #0x390\n"
        "  ret\n"
        ".seh_endproc\n");

EXPORT NTSTATUS WINAPI BTCpuSuspendLocalThread(HANDLE thread, ULONG *count) { return NtSuspendThread(thread, count); }

// Every exception the 64-bit side dispatches passes here first. Faults in x86 code are turned
// into guest exceptions by the app before Wine sees them (the Mach fault hook), so this only
// reports one that slipped through.
EXPORT NTSTATUS WINAPI BTCpuResetToConsistentState(void *ptrs) {
    host_call(H_RESET, (hostarg)ptrs);
    return 0;
}

// Decoded x86 blocks in a changed range are dropped (addresses arrive as host addresses).
static void invalidate(const void *addr, SIZE_T size) { host_call3(H_INVALIDATE, (hostarg)addr, (hostarg)size, 0); }
EXPORT void WINAPI BTCpuFlushInstructionCache2(const void *addr, SIZE_T size) { invalidate(addr, size); }
EXPORT void WINAPI BTCpuFlushInstructionCacheHeavy(const void *addr, SIZE_T size) { invalidate(addr, size); }
EXPORT void WINAPI BTCpuNotifyMemoryDirty(void *addr, SIZE_T size) { invalidate(addr, size); }
EXPORT void WINAPI BTCpuNotifyMemoryAlloc(void *addr, SIZE_T size, ULONG type, ULONG prot, int after, NTSTATUS st) {
    (void)addr; (void)size; (void)type; (void)prot; (void)after; (void)st;
}
EXPORT void WINAPI BTCpuNotifyMemoryProtect(void *addr, SIZE_T size, ULONG prot, int after, NTSTATUS st) {
    if (after && !st) invalidate(addr, size);
    (void)prot;
}
EXPORT void WINAPI BTCpuNotifyMemoryFree(void *addr, SIZE_T size, ULONG type, int after, NTSTATUS st) {
    if (after && !st) invalidate(addr, size);
    (void)type;
}
EXPORT NTSTATUS WINAPI BTCpuNotifyMapViewOfSection(void *unk1, void *addr, void *unk2, SIZE_T size, ULONG alloc, ULONG prot) {
    (void)unk1; (void)addr; (void)unk2; (void)size; (void)alloc; (void)prot;
    return 0;
}
// The extent of the view at addr: its regions share addr as AllocationBase
// (MEMORY_BASIC_INFORMATION: AllocationBase at 8, RegionSize at 0x18).
static SIZE_T view_size(void *addr) {
    uint8_t mbi[0x30];
    char *p = addr;
    SIZE_T total = 0;
    for (int i = 0; i < 4096; i++) {
        void *alloc_base;
        SIZE_T region;
        if (NtQueryVirtualMemory(CURRENT_PROCESS, p, 0 /* MemoryBasicInformation */, mbi, sizeof mbi, 0)) break;
        __builtin_memcpy(&alloc_base, mbi + 8, 8);
        __builtin_memcpy(&region, mbi + 0x18, 8);
        if (alloc_base != addr || !region) break;
        total += region;
        p += region;
    }
    return total;
}
EXPORT void WINAPI BTCpuNotifyUnmapViewOfSection(void *addr, int after, NTSTATUS st) {
    if (!after) invalidate(addr, view_size(addr));   // size 0 (unknown): every block
    (void)st;
}
EXPORT void WINAPI BTCpuNotifyReadFile(HANDLE file, void *addr, SIZE_T size, int after, NTSTATUS st) {
    if (after && !st) invalidate(addr, size);
    (void)file;
}
EXPORT void WINAPI BTCpuNotifyProcessExecuteFlagsChange(ULONG flags) { (void)flags; }

EXPORT unsigned char WINAPI BTCpuIsProcessorFeaturePresent(unsigned feature) {
    return host_call(H_FEATURE, feature) != 0;
}
EXPORT void WINAPI BTCpuUpdateProcessorInformation(void *info) { host_call(H_CPU_INFO, (hostarg)info); }

int WINAPI DllMainCRTStartup(void *module_, unsigned reason, void *reserved) {
    (void)module_; (void)reason; (void)reserved;
    return 1;
}
