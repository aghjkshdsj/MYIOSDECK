// SPDX-License-Identifier: GPL-3.0-or-later
// The x86 (32-bit) test suite for FXI32 inside Wine's WoW64 (docs/NO_JIT_WOW64.md, stage 4): the
// x64 suite's tests (suite-x64.c) for a 32-bit program, plus the WoW64 callback path. Every test
// runs as x86 code in FXI32; Wine's 64-bit half is native:
//   wndproc     a window procedure called back from win32u (WoW64: a nested simulation)
//   callbacks   ucrtbase qsort calling a comparator
//   float       printf of doubles, x87 long double
//   threads     4 threads, InterlockedIncrement, thread-local storage
//   raise       RaiseException caught by a vectored handler (continue execution)
//   int3        EXCEPTION_BREAKPOINT, handler skips it
//   divide      integer divide by zero, handler skips the instruction
//   fault       access violation (a host fault inside FXI32), handler skips it
//   c++         throw/catch across frames (llvm-mingw i686: DWARF unwinding)
//   longjmp     longjmp out of nested frames
//   child       CreateProcess of itself (a second 32-bit process, its own guest window)
// Every result line goes to the console and, as it happens, to C:\myiosdeck-output.txt, which
// the app copies into its log after the program exits. Exit code: 100 + the number of failures.
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int cxx_throw_test(int depth);   // suite-x64-cxx.cpp (portable C++)

static HANDLE g_out = INVALID_HANDLE_VALUE;
static int g_failed;

static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n';
    DWORD w;
    if (g_out != INVALID_HANDLE_VALUE) WriteFile(g_out, line, (DWORD)n, &w, NULL);
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
}
static void result(const char *test, int ok, const char *detail) {
    if (!ok) g_failed++;
    say("[suite] %-10s %s  %s", test, ok ? "ok  " : "FAIL", detail);
}

// ---- wndproc: SendMessage to our own (message-only) window: win32u's unix side calls the
// 32-bit window procedure back through KiUserCallbackDispatcher -> wow64 -> a nested
// BTCpuSimulate, and NtCallbackReturn longjmps back out ----
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_USER + 7) return (LRESULT)(wp * 3 + lp);
    return DefWindowProcA(h, msg, wp, lp);
}
static void t_wndproc(void) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "MyiosdeckSuite";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "MyiosdeckSuite", "suite", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    LRESULT r = h ? SendMessageA(h, WM_USER + 7, 11, 5) : -1;
    LRESULT r2 = h ? SendMessageA(h, WM_USER + 7, 1000, 1) : -1;
    char d[96];
    snprintf(d, sizeof d, "hwnd %p, results %ld %ld (want 38 3001)", (void *)h, (long)r, (long)r2);
    result("wndproc", h && r == 38 && r2 == 3001, d);
    if (h) DestroyWindow(h);
}

// ---- callbacks: qsort calls this function ----
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void t_callbacks(void) {
    int v[64];
    for (int i = 0; i < 64; i++) v[i] = (i * 37 + 11) % 64;
    qsort(v, 64, sizeof v[0], cmp_int);
    int ok = 1;
    for (int i = 0; i < 64; i++) ok &= v[i] == i;
    result("callbacks", ok, "ucrtbase qsort -> comparator");
}

// ---- float ----
static volatile double g_d = 2.0;
static volatile long double g_ld = 3.0L;
static void t_float(void) {
    char buf[96];
    long double r = g_ld / 4.0L + 0.25L;   // x87
    snprintf(buf, sizeof buf, "%.4f %.3e %d", g_d * 1.5 + 0.125, 1.0 / g_d / 1024.0, (int)(r * 4));
    result("float", !strcmp(buf, "3.1250 4.883e-04 4"), buf);
}

// ---- threads ----
static volatile LONG g_counter;
static __thread int t_local;
static DWORD WINAPI worker(void *arg) {
    int id = (int)(INT_PTR)arg;
    t_local = id * 1000;
    for (int i = 0; i < 100000; i++) InterlockedIncrement(&g_counter);
    return t_local == id * 1000 ? 7 : 0;   // TLS kept per thread
}
static void t_threads(void) {
    HANDLE h[4];
    DWORD t0 = GetTickCount();
    for (int i = 0; i < 4; i++) h[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)(i + 1), 0, NULL);
    DWORD wr = WaitForMultipleObjects(4, h, TRUE, 60000);
    int ok = wr == WAIT_OBJECT_0;
    for (int i = 0; i < 4; i++) {
        DWORD code = 0;
        GetExitCodeThread(h[i], &code);
        ok &= code == 7;
        CloseHandle(h[i]);
    }
    char d[96];
    snprintf(d, sizeof d, "counter %ld (want 400000), %lu ms", g_counter, GetTickCount() - t0);
    result("threads", ok && g_counter == 400000, d);
}

// ---- exceptions through a vectored handler ----
// The faulting instructions sit at known labels; the handler resumes after them. (i386 mingw
// decorates C names with an underscore.)
extern char x_int3[], x_int3_end[], x_div[], x_div_end[], x_fault[], x_fault_end[];
__asm__(".text\n"
        ".globl _do_int3\n_do_int3:\n"
        "_x_int3: int3\n_x_int3_end: ret\n"
        ".globl _do_div\n_do_div:\n"
        "  movl 4(%esp), %eax\n  xorl %edx, %edx\n  xorl %ecx, %ecx\n"
        "_x_div: divl %ecx\n_x_div_end: ret\n"
        ".globl _do_fault\n_do_fault:\n"
        "  movl 4(%esp), %eax\n"
        "_x_fault: movl (%eax), %eax\n_x_fault_end: ret\n");
void do_int3(void);
unsigned do_div(unsigned x);
unsigned do_fault(const void *p);

static volatile LONG g_seen_code;
static volatile ULONG_PTR g_seen_addr, g_seen_info1;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char *resume = NULL;
    if (r->ExceptionCode == 0xE0000001) {   // our RaiseException
        g_seen_code = (LONG)r->ExceptionCode;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // i386 Windows reports a breakpoint with Eip after the int3; the record has its address.
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT && (char *)r->ExceptionAddress == x_int3) resume = x_int3_end;
    if (r->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO && (char *)c->Eip == x_div) resume = x_div_end;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (char *)c->Eip == x_fault) resume = x_fault_end;
    if (!resume) return EXCEPTION_CONTINUE_SEARCH;
    g_seen_code = (LONG)r->ExceptionCode;
    g_seen_addr = (ULONG_PTR)r->ExceptionAddress;
    g_seen_info1 = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    c->Eax = 0x5a5a;
    c->Eip = (DWORD)(ULONG_PTR)resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void t_raise(void) {
    g_seen_code = 0;
    RaiseException(0xE0000001, 0, 0, NULL);
    result("raise", g_seen_code == (LONG)0xE0000001, "RaiseException -> vectored handler -> continue");
}
static void t_int3(void) {
    g_seen_code = 0;
    do_int3();
    char d[96];
    snprintf(d, sizeof d, "code %08lx at %p (int3 at %p)", (unsigned long)g_seen_code, (void *)g_seen_addr, (void *)x_int3);
    result("int3", g_seen_code == (LONG)EXCEPTION_BREAKPOINT && g_seen_addr == (ULONG_PTR)x_int3, d);
}
static void t_divide(void) {
    g_seen_code = 0;
    unsigned r = do_div(1234);
    char d[96];
    snprintf(d, sizeof d, "code %08lx, result %#x (handler sets 0x5a5a)", (unsigned long)g_seen_code, r);
    result("divide", g_seen_code == (LONG)EXCEPTION_INT_DIVIDE_BY_ZERO && r == 0x5a5a, d);
}
static void t_fault(void) {
    g_seen_code = 0;
    unsigned r = do_fault((const void *)(ULONG_PTR)0x18);
    char d[128];
    snprintf(d, sizeof d, "code %08lx at %p, address %#lx, result %#x", (unsigned long)g_seen_code,
             (void *)g_seen_addr, (unsigned long)g_seen_info1, r);
    result("fault", g_seen_code == (LONG)EXCEPTION_ACCESS_VIOLATION && g_seen_addr == (ULONG_PTR)x_fault &&
                    g_seen_info1 == 0x18 && r == 0x5a5a, d);
}

// ---- C++ exceptions, longjmp ----
static void t_cxx(void) {
    int r = cxx_throw_test(5);
    char d[64];
    snprintf(d, sizeof d, "caught %d (want 105)", r);
    result("c++", r == 105, d);
}
static jmp_buf g_jb;
static __attribute__((noinline)) void deep(int n) {
    volatile char pad[64];
    pad[0] = (char)n;
    if (n == 0) longjmp(g_jb, 77);
    deep(n - 1);
    pad[1] = 0;
}
static void t_longjmp(void) {
    int r = setjmp(g_jb);
    if (r == 0) { deep(6); result("longjmp", 0, "deep() returned"); return; }
    char d[48];
    snprintf(d, sizeof d, "setjmp returned %d", r);
    result("longjmp", r == 77, d);
}

// ---- child process ----
static void t_child(void) {
    char self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, self, sizeof self);
    snprintf(cmd, sizeof cmd, "\"%s\" child", self);
    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        char d[64];
        snprintf(d, sizeof d, "CreateProcess error %lu", GetLastError());
        result("child", 0, d);
        return;
    }
    DWORD wr = WaitForSingleObject(pi.hProcess, 60000), code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    char d[64];
    snprintf(d, sizeof d, "wait %lu, child exit code %lu (want 33)", wr, code);
    result("child", wr == WAIT_OBJECT_0 && code == 33, d);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return 33;
    g_out = CreateFileA("C:\\myiosdeck-output.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    say("[suite] x86 (32-bit) test suite (FXI32 in WoW64, no JIT) starting");
    AddVectoredExceptionHandler(1, veh);
    t_wndproc();
    t_callbacks();
    t_float();
    t_threads();
    t_raise();
    t_int3();
    t_divide();
    t_fault();
    t_cxx();
    t_longjmp();
    t_child();
    say("[suite] done: %d failed", g_failed);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    return 100 + g_failed;
}
