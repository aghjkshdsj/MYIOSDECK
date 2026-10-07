// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT step B test program: a Windows console program built as ARM64EC (llvm-mingw, normal
// CRT), so Wine can run it with no x64 CPU. Everything it does goes through Wine's own DLLs
// (kernel32, kernelbase, ucrtbase, ntdll) running from signed dylibs.
//
// Its report goes to the console AND to C:\myiosdeck-output.txt, which the app copies into
// its log after the program exits (WineController.watch), and it exits with 42 when main
// runs to the end: build 51 exited with 0 and no visible output, which proved nothing.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static char report[4096];
static size_t report_len;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(report + report_len, sizeof report - report_len, fmt, ap);
    va_end(ap);
    if (n > 0) report_len += (size_t)n < sizeof report - report_len ? (size_t)n : sizeof report - report_len - 1;
}

int main(void) {
    say("Hello from Windows (ARM64EC) on iPhone, without JIT!\r\n");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    say("processors: %lu, page size: %lu\r\n", si.dwNumberOfProcessors, si.dwPageSize);
    say("process id: %lu, thread id: %lu\r\n", GetCurrentProcessId(), GetCurrentThreadId());

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    unsigned long long x = 0x9E3779B97F4A7C15ull, acc = 0;
    for (int i = 0; i < 50000000; i++) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        acc += x * 0x2545F4914F6CDD1Dull;
    }
    QueryPerformanceCounter(&t1);
    say("50M xorshift steps in %.1f ms (checksum %016llx)\r\n",
        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, acc);

    SetLastError(1234);
    say("SetLastError/GetLastError through the TEB: %lu\r\n", GetLastError());

    // Console: report whether the handle exists and the write succeeded.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    BOOL ok = out && out != INVALID_HANDLE_VALUE && WriteFile(out, report, (DWORD)report_len, &written, NULL);
    DWORD werr = ok ? 0 : GetLastError();
    say("console: handle %p, WriteFile %s (%lu bytes, error %lu), file type %lu\r\n", out,
        ok ? "ok" : "FAILED", written, werr, out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : 0);
    printf("%s", report);
    fflush(stdout);

    // File: the app reads this back into its log.
    HANDLE file = CreateFileA("C:\\myiosdeck-output.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        say("main() reached the end: exit code 42\r\n");
        WriteFile(file, report, (DWORD)report_len, &written, NULL);
        CloseHandle(file);
    }
    return 42;
}
