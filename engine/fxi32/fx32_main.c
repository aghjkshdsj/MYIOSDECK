// SPDX-License-Identifier: GPL-3.0-or-later
// fxi32 CLI (CI): engine/fxi/fxi_main.c for i386 guests: runs a static i386 Linux ELF inside a
// 4 GB guest window, and scans PE32 files (--scan-pe). docs/NO_JIT_WOW64.md, stage 2.
#define FXI_I386 1
#include "../fxi/fxi_main.c"
