#!/bin/bash
# Build the FXR host tool: engine/fxr/build.sh <out-dir>. Needs clang 19+ for preserve_none on ARM64
# (CC=clang-19); an older clang still builds FXR, without the pinned-register calling convention.
# The pinned handlers are tens of thousands of small functions in several files: compiled in parallel.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT/obj"
CC="${CC:-clang}"
"$CC" --version | head -1
ARCH_FLAGS=()
if [ "$(uname -m)" = x86_64 ]; then ARCH_FLAGS=(-mcx16); fi
CFLAGS=(-O2 -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-sign-compare -ffp-contract=off -fno-math-errno "${ARCH_FLAGS[@]}")
SRCS=(fxi_core.c fxi_decode.c fxi_flags.c fxi_ops.c fxi_sse.c fxi_atomic.c fxi_x87.c fxi_win.c fxi_main.c
      fxr_pin.c fxr_pin_alu.c fxr_pin_br.c fxr_pin_sse.c)
pids=()
for f in "${SRCS[@]}"; do
    "$CC" "${CFLAGS[@]}" -c "$HERE/$f" -o "$OUT/obj/${f%.c}.o" & pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compile failed"; exit 1; }
"$CC" -o "$OUT/fxr" "$OUT"/obj/*.o -lm -lpthread
ls -la "$OUT/fxr" "$OUT"/obj/fxr_pin*.o
