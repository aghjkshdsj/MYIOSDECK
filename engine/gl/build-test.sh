#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The OpenGL test program (engine/gl/test/gl-triangle-x64.c) for the Library, built on the
# Linux CI host with the same llvm-mingw as engine/pedylib/build-spike.sh (shared cache).
#   engine/gl/build-test.sh <out-dir> [toolchain-cache-dir]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?out dir}"
CACHE="${2:-$HERE/toolchain}"
LLVM_MINGW=llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20260421/$LLVM_MINGW.tar.xz
LLVM_MINGW_SHA256=f8b8cce779affeab47bcaec6ce6e9e768b166094a366123ef64b2d0b389cc121

TC="$CACHE/$LLVM_MINGW/bin"
if [ ! -x "$TC/clang" ]; then
    mkdir -p "$CACHE"
    curl -fsSL -o "$CACHE/llvm-mingw.tar.xz" "$LLVM_MINGW_URL"
    echo "$LLVM_MINGW_SHA256  $CACHE/llvm-mingw.tar.xz" | sha256sum -c -
    tar -C "$CACHE" -xf "$CACHE/llvm-mingw.tar.xz"
    rm "$CACHE/llvm-mingw.tar.xz"
fi
mkdir -p "$OUT"
"$TC/x86_64-w64-mingw32-clang" -O2 -Wall -mwindows -o "$OUT/gl-triangle-x64.exe" \
    "$HERE/test/gl-triangle-x64.c" -lopengl32 -lgdi32 -luser32
"$TC/llvm-readobj" --coff-imports "$OUT/gl-triangle-x64.exe" | grep -E 'Name:' | sort -u
ls -la "$OUT/gl-triangle-x64.exe"
