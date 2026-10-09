#!/bin/bash
# Build the i386 guest test programs for FXI32 (docs/NO_JIT_WOW64.md, stage 2) on an x86-64
# Linux host with gcc-multilib. Static executables at their link address (fxi32 maps them into
# a 4 GB guest window); the runner runs them natively too, and fxi32's output must equal it.
#   engine/guest/build32.sh <out-dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out32}"
mkdir -p "$OUT"

# -lgcc: 64-bit division and shifts in 32-bit code. -mfpmath=sse: scalar float math in SSE2, as
# MSVC's /arch:SSE2 (the default) compiles games; doubles still return through ST0 (x87).
CFLAGS=(-m32 -O2 -march=i686 -msse2 -mfpmath=sse -static -fno-pie -no-pie -nostdlib -ffreestanding -fno-builtin
        -fno-strict-aliasing -fno-stack-protector -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns
        -Wl,--build-id=none -Wl,-z,noexecstack -I"$HERE")
for prog in hello bench difftest32; do
    gcc "${CFLAGS[@]}" -o "$OUT/$prog.elf" "$HERE/$prog.c" -lgcc
    strip "$OUT/$prog.elf"
    file "$OUT/$prog.elf"
done
# The same benchmark with x87 float math (-mfpmath=387): 32-bit code that predates SSE2.
gcc "${CFLAGS[@]/-mfpmath=sse/-mfpmath=387}" -o "$OUT/bench_x87.elf" "$HERE/bench.c" -lgcc
strip "$OUT/bench_x87.elf"
"$OUT/hello.elf"
"$OUT/bench.elf" integer 1
"$OUT/difftest32.elf" | tail -1
ls -la "$OUT"
