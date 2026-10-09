// SPDX-License-Identifier: GPL-3.0-or-later
// FXI32's WoW64 API (engine/fxi32/fxi_wow.c), for the app's host (App/Sources/Native/
// fxi_wow_host.c). docs/NO_JIT_WOW64.md: FXI32 is the x86 CPU of a 32-bit Wine process.
#ifndef FX32_H
#define FX32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fx32Cpu Fx32Cpu;

/// One CPU per thread. All threads of a process share its window (gbase = B) and block cache;
/// bop is the guest address of the system-call entry (bop + 2: the unix-call entry).
Fx32Cpu *fx32_wow_cpu_new(uint64_t gbase, uint32_t bop);

enum { FX32_RUN_ERROR = 0, FX32_RUN_SYSCALL = 1, FX32_RUN_UNIXCALL = 2, FX32_RUN_EXCEPTION = 3 };
/// Interpret from the CPU's EIP until control reaches the BOP page (a system or unix call), or
/// an instruction raises an exception, or an error (fx32_wow_error) stops it.
int fx32_wow_run(Fx32Cpu *c);
const char *fx32_wow_error(Fx32Cpu *c);

/// The 32-bit register state as an I386_CONTEXT (0x2cc bytes: integer, control, segments,
/// FloatSave and ExtendedRegisters). load reads what ContextFlags selects; teb32 is the FS base.
void fx32_wow_load_context(Fx32Cpu *c, const void *i386_context, uint32_t teb32);
void fx32_wow_save_context(Fx32Cpu *c, void *i386_context);

enum { FX32_EAX, FX32_ECX, FX32_EDX, FX32_EBX, FX32_ESP, FX32_EBP, FX32_ESI, FX32_EDI };
uint32_t fx32_wow_reg(Fx32Cpu *c, int r);
void fx32_wow_set_reg(Fx32Cpu *c, int r, uint32_t v);
uint32_t fx32_wow_eip(Fx32Cpu *c);
void fx32_wow_set_eip(Fx32Cpu *c, uint32_t eip);

/// After FX32_RUN_EXCEPTION: the Windows exception the instruction at *eip raised (the state is
/// as before the instruction).
typedef struct { uint32_t code, flags, nparams, eip; uint64_t info[2]; } fx32_wow_exc;
void fx32_wow_exception(Fx32Cpu *c, fx32_wow_exc *e);
/// A host fault while interpreting: the instruction whose memory access faulted (the state is
/// as before it), *is_fetch = 1 when it happened fetching code.
uint32_t fx32_wow_fault_eip(Fx32Cpu *c, int *is_fetch);

/// Code in [start, start + len) of the process at gbase changed (len 0: every block).
void fx32_wow_invalidate(uint64_t gbase, uint32_t start, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
