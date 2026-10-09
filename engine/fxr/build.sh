#!/bin/bash
# Build the FXR host tool: engine/fxr/build.sh <out-dir>. Needs clang 19+ for preserve_none on ARM64
# (CC=clang-19); an older clang still builds FXR, without the pinned-register calling convention.
# The pinned handlers are tens of thousands of small functions in several files: compiled in
# parallel (one job per core: each needs a few GB), without debug info (perf still has the names).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT/obj"
CC="${CC:-clang}"
"$CC" --version | head -1
ARCH_FLAGS=()
if [ "$(uname -m)" = x86_64 ]; then ARCH_FLAGS=(-mcx16); fi
CFLAGS="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
-Wno-unused-but-set-variable -Wno-sign-compare -ffp-contract=off -fno-math-errno ${ARCH_FLAGS[*]:-} ${EXTRA_CFLAGS:-}"
SRCS="fxr_pin_x3.c fxr_pin_arj.c fxr_pin_lcj.c fxr_pin_stub.c fxr_pin_step1.c fxr_pin_step2.c fxr_pin_step3.c fxr_pin_mem.c fxr_pin_mem2.c fxr_pin_fuse.c fxr_pin_sse.c fxr_pin_alu.c fxr_pin_br.c fxr_pin.c
      fxr_pin_alu_r1.c fxr_pin_sse_r1.c fxr_pin_mem_r1.c fxr_pin_mem2_r1.c fxr_pin_step3_r1.c
      fxi_core.c fxi_decode.c fxi_flags.c fxi_ops.c fxi_sse.c fxi_atomic.c fxi_x87.c fxi_win.c fxi_main.c"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
export CC CFLAGS HERE OUT
printf '%s\n' $SRCS | xargs -P "$JOBS" -I{} sh -c '$CC $CFLAGS -c "$HERE/{}" -o "$OUT/obj/$(basename {} .c).o" || exit 255'
"$CC" -o "$OUT/fxr" "$OUT"/obj/*.o -lm -lpthread
ls -la "$OUT/fxr" "$OUT"/obj/fxr_pin*.o
