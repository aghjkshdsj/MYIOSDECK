#!/bin/bash
# Build the FXI host tool (Linux/macOS, clang): engine/fxi/build.sh <out-dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
CC="${CC:-clang}"
"$CC" --version | head -1
# cmpxchg16b needs inline 16-byte atomics: -mcx16 on an x86-64 host (ARM64 has them built in).
ARCH_FLAGS=()
if [ "$(uname -m)" = x86_64 ]; then ARCH_FLAGS=(-mcx16); fi
"$CC" -O2 -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare "${ARCH_FLAGS[@]}" \
    -o "$OUT/fxi" "$HERE"/fxi_core.c "$HERE"/fxi_decode.c "$HERE"/fxi_flags.c "$HERE"/fxi_ops.c "$HERE"/fxi_sse.c \
    "$HERE"/fxi_atomic.c "$HERE"/fxi_x87.c "$HERE"/fxi_win.c "$HERE"/fxi_main.c -lm -lpthread
ls -la "$OUT/fxi"
