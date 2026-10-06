#!/bin/bash
# Build the x86-64 guest test programs and embed them as C headers for the app.
# Runs on an x86-64 Linux host (the GitHub Actions ubuntu runner).
#   engine/guest/build.sh <out-dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"

# static-PIE + self-relocation (guest_rt.h): the iOS host loads the image at
# whatever address mmap gives it, never at 0x400000.
CFLAGS=(-O2 -march=x86-64-v2 -fPIE -static-pie -nostdlib -ffreestanding -fno-builtin
        -fno-strict-aliasing -fno-stack-protector -fno-asynchronous-unwind-tables
        -fno-tree-loop-distribute-patterns -Wl,--no-dynamic-linker -Wl,--build-id=none
        -Wl,-z,norelro -Wl,-z,noexecstack -Wl,-z,max-page-size=0x4000 -Wl,-z,separate-code -I"$HERE")

# bench_sse2: the same benchmark for baseline x86-64 (SSE2 only), for the
# interpreter, which does not implement every SSE4 instruction.
for prog in hello bench bench_sse2; do
    src="$HERE/$prog.c"; flags=()
    if [ "$prog" = bench_sse2 ]; then src="$HERE/bench.c"; flags=(-march=x86-64); fi
    gcc "${CFLAGS[@]}" "${flags[@]}" -o "$OUT/$prog.elf" "$src"
    strip "$OUT/$prog.elf"
    file "$OUT/$prog.elf"
    # Sanity check on the build host: the guest runs natively on x86-64 Linux.
    if [ "$prog" = hello ]; then "$OUT/$prog.elf"; else "$OUT/$prog.elf" integer 2; "$OUT/$prog.elf" simd 10; fi
    (cd "$OUT" && xxd -i -n "guest_${prog}_elf" "$prog.elf" > "guest_${prog}.h")
done
ls -la "$OUT"
