// SPDX-License-Identifier: GPL-3.0-or-later
// FXI32 WoW64 mode (docs/NO_JIT_WOW64.md): FXI32 as the x86 CPU of a 32-bit Wine process, in
// the role FEX's xtajit.dll plays with JIT. The x86 code runs from the process's guest window;
// a run ends when control reaches the BOP page (the system-call entry g, the unix-call entry
// g + 2), at an exception or at a fault, and the host (App/Sources/Native/fxi_wow_host.c)
// carries out the call through wow64.dll.
#define FXI_I386 1
#include <stdlib.h>

#include "../fxi/fxi_internal.h"

// A lookup of g or g + 2 returns a one-uop block that stops the run there (the bytes at g are
// never decoded: cd 2e cd 2e, FEX's convention, only marks the page).
Block *fxi_wow_bop_block(struct Fxi *vm, uint64_t rip) {
    if (!vm->bop || (rip != vm->bop && rip != vm->bop + 2)) return NULL;
    Block *b = calloc(1, sizeof(Block) + sizeof(Uop));
    b->rip = rip;
    b->n = 1;
    b->u[0].fn = fxi_named("bop_exit");
    b->u[0].imm = rip;
    b->u[0].rip = rip;
    return b;
}
