#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$1"
gcc -O2 -march=x86-64 -fPIE -static-pie -nostdlib -ffreestanding -fno-builtin \
  -fno-strict-aliasing -fno-stack-protector -fno-asynchronous-unwind-tables \
  -fno-tree-loop-distribute-patterns -Wl,--no-dynamic-linker -Wl,--build-id=none \
  -Wl,-z,norelro -Wl,-z,noexecstack -Wl,-z,max-page-size=0x4000 -Wl,-z,separate-code \
  "$HERE/probe.c" -o "$OUT/astra-probe.elf"
"$OUT/astra-probe.elf" > "$OUT/astra-probe-native.txt"
