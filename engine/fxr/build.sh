#!/bin/bash
# Build the FXR host tool: engine/fxr/build.sh <out-dir>. Needs clang 19+ for preserve_none on ARM64
# (CC=clang-19); an older clang still builds FXR, without the pinned-register calling convention.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
CC="${CC:-clang}"
"$CC" --version | head -1
ARCH_FLAGS=()
if [ "$(uname -m)" = x86_64 ]; then ARCH_FLAGS=(-mcx16); fi
"$CC" -O2 -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-sign-compare -ffp-contract=off "${ARCH_FLAGS[@]}" \
    -o "$OUT/fxr" "$HERE"/fxi_core.c "$HERE"/fxi_decode.c "$HERE"/fxi_flags.c "$HERE"/fxi_ops.c "$HERE"/fxi_sse.c \
    "$HERE"/fxi_atomic.c "$HERE"/fxi_x87.c "$HERE"/fxi_win.c "$HERE"/fxi_main.c "$HERE"/fxr_pin.c -lm -lpthread
ls -la "$OUT/fxr"
