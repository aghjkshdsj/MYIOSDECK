// SPDX-License-Identifier: GPL-3.0-or-later
// No-JIT step B test program: a Windows console program built as ARM64EC (llvm-mingw, normal
// CRT), so Wine can run it with no x64 CPU. Every line it prints comes through Wine's own
// DLLs (kernel32, ucrtbase, ...) running from signed dylibs.
#include <stdio.h>
#include <windows.h>

int main(void) {
    printf("Hello from Windows (ARM64EC) on iPhone, without JIT!\n");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    printf("processors: %lu, page size: %lu\n", si.dwNumberOfProcessors, si.dwPageSize);
    printf("process id: %lu, thread id: %lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    unsigned long long x = 0x9E3779B97F4A7C15ull, acc = 0;
    for (int i = 0; i < 50000000; i++) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        acc += x * 0x2545F4914F6CDD1Dull;
    }
    QueryPerformanceCounter(&t1);
    printf("50M xorshift steps in %.1f ms (checksum %016llx)\n",
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, acc);
    SetLastError(1234);
    printf("SetLastError/GetLastError through the TEB: %lu\n", GetLastError());
    fflush(stdout);
    return 0;
}
