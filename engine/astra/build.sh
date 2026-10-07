#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
CXX="${CXX:-clang++}"
"$CXX" --version | head -1
for source in astra decode vector fusion; do
  "$CXX" -O2 -ffp-contract=off -std=c++17 -g -Wall -Wextra ${ASTRA_EXTRA_FLAGS:-} \
    -c "$HERE/$source.cpp" -o "$OUT/$source.o"
done
"$CXX" -O2 -std=c++17 ${ASTRA_EXTRA_FLAGS:-} "$OUT/"*.o "$HERE/main.cpp" -o "$OUT/astra"
"$CXX" -O2 -std=c++17 ${ASTRA_EXTRA_FLAGS:-} "$OUT/"*.o "$HERE/safety.cpp" -o "$OUT/safety"
gcc -O2 -ffp-contract=off "$HERE/native.c" -o "$OUT/native"
