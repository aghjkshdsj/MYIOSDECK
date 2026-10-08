# Astra: independent, no-JIT x86-64 interpreter

SPDX-License-Identifier: GPL-3.0-or-later

Astra is a separate experimental engine. It does not replace or link to FXI,
change the app, or provide a Windows/Wine integration. Its public C interface is
in `astra.h`. Each invocation owns its guest arena, CPU, and decoded-block cache.

## Execution design

The ELF loader allocates ordinary nonexecutable storage with `posix_memalign`.
Guest virtual addresses are addresses inside that arena. The guest's existing
startup code performs static-PIE relocations. The decoder creates arrays of
instruction records containing operands and pointers to **statically compiled**
C++ handlers. Clang `musttail` dispatch bounds the host stack; `preserve_none` is
used only when the compiler supports it. No machine code is emitted, and no
executable mapping, memory-protection changes, JIT entitlement, assembly thunk,
or external CPU-emulation library is used.

Integer handlers specialize operand width and register/immediate/memory forms.
Arithmetic flags are lazy; backward block analysis suppresses flag writes when
a later full writer overwrites them before use. Direct branches cache decoded
successors. SSE uses compiler 128-bit vectors, which lower to NEON on ARM64;
scalar double operations preserve upper XMM lanes. FP contraction is disabled.

The statically compiled fusion library recognizes general decoded instruction
patterns: compare/test and branch, vector transform loops, strided byte stores,
and integer sum reductions. Selection does not inspect program names, fixed
addresses, benchmark constants, or expected results. Registers, addresses,
increments, data, and trip counts come from the guest. Memory operations retain
their original order, including when buffers overlap. `ASTRA_NO_FUSION=1`
disables fusion for differential testing. There are no benchmark kernel calls
or substitutions in the interpreter.

## Build and validation

```sh
bash engine/guest/build.sh /tmp/astra-guests
bash engine/astra/probe-build.sh /tmp/astra-guests
bash engine/astra/build.sh /tmp/astra-bin
/tmp/astra-bin/astra /tmp/astra-guests/hello.elf
/tmp/astra-bin/astra /tmp/astra-guests/bench_sse2.elf integer 3
```

The isolated `astra.yml` workflow builds unchanged x86 guest programs once and
passes them to both Linux architectures. It builds unchanged FXI in each
comparison job. The five kernel scales are 3, 16, 50, 6, and 150. Native ARM64
uses the shared `bench_kernels.h`, GCC `-O2 -ffp-contract=off`, and the guest's
warm-up and clock boundaries. Native x86 executes the guest ELF directly.

For each kernel, three measurements per engine run in rotating order. The
reported percentage is `100 * median(native_ns) / median(interpreter_ns)`;
the overall percentage is the arithmetic mean of the five percentages. All
nine checksums per kernel must agree. An ARM64 mean below **35%** fails CI, as
does any execution or correctness failure. The original 12% target passed
before the owner raised the gate. Raw trial data are archived under
`astra-results-*`, and tables are written to the job summary.

The approved hello comparison requires exactly five lines and exit status zero:
the greeting matches byte for byte; CPU labels match; vendor is 12 printable
ASCII characters; brand is nonempty printable ASCII; SSE2 is yes, while Astra
reports SSE4.2 and AVX as no; loop time is a positive integer. Native and
interpreted raw outputs are included in both the log and summary.

`probe.c` compares arithmetic results and flags, alternative vector loop
registers/data/trip counts, forward-overlapping buffers, strided byte stores,
sum reductions, and floating-point comparison flags against native execution.
It also compares fusion on/off. `safety.cpp` checks malformed/truncated ELF,
unsupported instructions, invalid memory, writes to decoded code, and divide
faults. The macOS job compiles every engine translation unit with
`xcrun -sdk iphoneos clang -arch arm64 -O2 -c` (plus C++17 and FP options).
An additional ASan/UBSan job exercises the probes, malformed inputs, hello, and
all five kernel paths with fusion both enabled and disabled.

## Current scope and limitations

This is a benchmark-capable experimental user-mode subset, not a complete x86
machine or a security boundary. Supported system calls are Linux write,
clock_gettime(CLOCK_MONOTONIC), exit, and exit_group. Only little-endian ELF64
x86 static PIE with one executable load segment is accepted. Guest output is
bounded by the specified 8192-byte result buffer. Guest memory accesses are
checked against an arena containing the image and 8 MiB stack; individual ELF
data-page permissions are not modeled. Writes into executable guest segments
fail cleanly because self-modifying-code invalidation is not implemented.

The implemented SSE/SSE2 subset covers the supplied benchmark and hello, plus
the PINSRD/Q and PEXTRD/Q operations used by hello. SSE4.2 and AVX are not
advertised. x87, atomics, FS/GS, address-size overrides, most additional Linux
syscalls, guest signals, full MXCSR exception/rounding behavior, and Windows
execution are not implemented. Execution assumes the default host FP mode.
Unsupported instructions report opcode bytes and both an address and image
offset. Programs with infinite loops have no internal execution deadline;
the CI subprocess harness enforces a timeout.

An iOS object compile establishes SDK/architecture compatibility, not App Store
approval or an on-device performance result. ARM64 Linux measurements are not
iPhone measurements. No pull request should be opened until all required jobs
and the ARM64 gate pass on the same commit.
