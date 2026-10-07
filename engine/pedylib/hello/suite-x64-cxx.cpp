// SPDX-License-Identifier: GPL-3.0-or-later
// suite-x64: C++ throw/catch across several x64 frames. llvm-mingw's x86-64 C++ exceptions
// unwind with Windows SEH (RtlUnwindEx over the frames' .pdata), so this exercises Wine's
// unwinder on interpreted x64 code.
#include <stdexcept>

struct Guard {
    int *count;
    ~Guard() { ++*count; }   // destructors run while unwinding
};

static __attribute__((noinline)) void thrower(int depth, int *count) {
    Guard g{count};
    if (depth == 0) throw std::runtime_error("x64");
    thrower(depth - 1, count);
}

extern "C" int cxx_throw_test(int depth) {
    int count = 0;
    try {
        thrower(depth, &count);
    } catch (const std::runtime_error &e) {
        return 100 + count - (e.what()[0] == 'x' ? 1 : 0);   // depth 5: 6 guards -> 105
    }
    return -1;
}
