#!/bin/bash
# Build the FXI32 host tool (Linux/macOS, clang): engine/fxi32/build.sh <out-dir>
# FXI's sources compiled for i386 guests (FXI_I386, docs/NO_JIT_WOW64.md 2.8), symbols fx32_*.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT/obj32"
CC="${CC:-clang}"
"$CC" --version | head -1
ARCH_FLAGS=()
if [ "$(uname -m)" = x86_64 ]; then ARCH_FLAGS=(-mcx16); fi
for f in fx32_core fx32_decode fx32_flags fx32_ops fx32_sse fx32_atomic fx32_x87 fxi_wow fx32_main; do
    "$CC" -O2 -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare "${ARCH_FLAGS[@]}" \
        -c -o "$OUT/obj32/$f.o" "$HERE/$f.c"
done
# Every external name is renamed (engine/fxi/fxi_i386.h): none may clash with FXI's in the app.
if nm -g --defined-only "$OUT"/obj32/*.o | grep -E ' [TDBR] _?fxi_'; then
    echo "error: an FXI32 object defines a global fxi_ symbol (add it to engine/fxi/fxi_i386.h)" >&2
    exit 1
fi
"$CC" -o "$OUT/fxi32" "$OUT"/obj32/*.o -lm -lpthread
# wow_test: the WoW64 API the app's host drives (docs/NO_JIT_WOW64.md, stage 3).
"$CC" -O2 -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter "${ARCH_FLAGS[@]}" -I"$HERE" -o "$OUT/wow_test" "$HERE/wow_test.c" \
    $(ls "$OUT"/obj32/*.o | grep -v fx32_main.o) -lm -lpthread
ls -la "$OUT/fxi32" "$OUT/wow_test"
