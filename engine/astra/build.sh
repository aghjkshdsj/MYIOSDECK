#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
CXX="${CXX:-clang++}"
"$CXX" --version | head -1
"$CXX" -O2 -ffp-contract=off -std=c++17 -g -Wall -Wextra \
  "$HERE/astra.cpp" "$HERE/decode.cpp" "$HERE/vector.cpp" "$HERE/main.cpp" -o "$OUT/astra"
gcc -O2 -ffp-contract=off "$HERE/native.c" -o "$OUT/native"
