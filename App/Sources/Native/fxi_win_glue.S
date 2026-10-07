// SPDX-License-Identifier: GPL-3.0-or-later
// FXI as the x64 CPU of Wine's ARM64EC processes without JIT (docs/NO_JIT_WINDOWS.md step D):
// the transitions between native ARM64EC code and FXI-interpreted x64 code. The emulator DLL
// Wine loads as xtajit64.dll (engine/pedylib/emu) forwards its ExitToX64 / DispatchJump /
// RetToEntryThunk / BeginSimulation exports here. Conventions follow the ARM64EC ABI as
// FEX implements it (FEX Source/Windows/ARM64EC/Module.S):
//
//   x64 registers live in the ARM64EC mapping: RAX=x8 RCX=x0 RDX=x1 RBX=x27 RSP=sp RBP=x29
//   RSI=x25 RDI=x26 R8-R11=x2-x5 R12-R15=x19-x22, XMM0-15=v0-v15. x9 = target address.
//   x10-x12, x15-x17 are free (unmapped, volatile) at these points.
//
// Per thread, TEB->ChpeV2CpuAreaInfo (CPU area, TEB+0x1788) holds: +0x0 InSimulation,
// +0x8 EmulatorStackBase, +0x18 ContextAmd64, +0x30 EmulatorData[0] = our FxiCpu (set by
// ThreadInit). The TEB comes from TPIDRRO_EL0's TSD array (iOS clears x18).
//
// FxiCpu layout (engine/fxi/fxi_internal.h, asserted in fxi_win.c): GPRs in x86 order at
// 0x00 (RSP 0x20, GS base 0x90), rip 0x98, xmm0-15 at 0xa0.

.macro LOAD_TEB dst, tmp
    mrs \dst, TPIDRRO_EL0
    and \dst, \dst, #0xfffffffffffffff8
    adrp \tmp, _fxi_win_tsd_offset@PAGE
    ldrsw \tmp, [\tmp, _fxi_win_tsd_offset@PAGEOFF]
    ldr \dst, [\dst, \tmp]
.endm

    .text
    .p2align 2

// [ExitToX64] __os_arm64x_dispatch_call_no_redirect: an exit thunk calls x64 code at x9 with
// `blr x16`. Push lr: the x64 code's `ret` comes back into the exit thunk.
    .globl _fxi_win_exit_to_x64
_fxi_win_exit_to_x64:
    str lr, [sp, #-8]!
    b Lenter

// [DispatchJump] __os_arm64x_x64_jump: jump to x9 (x64 or native), lr pushed like an x64 call.
    .globl _fxi_win_dispatch_jump
_fxi_win_dispatch_jump:
    str lr, [sp, #-8]!
    b Lcheck

// [RetToEntryThunk] __os_arm64x_dispatch_ret: an entry thunk returns to its x64 caller at lr.
    .globl _fxi_win_ret_to_entry_thunk
_fxi_win_ret_to_entry_thunk:
    mov x9, lr
Lcheck:     // native ARM64EC target? (PEB->EcCodeBitMap, one bit per 4 KB page)
    LOAD_TEB x16, x17
    ldr x16, [x16, #0x60]
    ldr x16, [x16, #0x368]
    lsr x17, x9, #18
    ldr x16, [x16, x17, lsl #3]
    lsr x17, x9, #12
    lsr x16, x16, x17
    tbnz x16, #0, Lexit_ec

// Enter FXI at x9 with the x64 state in the ARM64EC mapping and sp = RSP.
Lenter:
    LOAD_TEB x16, x17
    ldr x17, [x16, #0x1788]         // CPU area
    ldr x16, [x17, #0x30]           // FxiCpu
    cbz x16, Lno_cpu
    str x8,  [x16, #0x00]           // RAX
    str x0,  [x16, #0x08]           // RCX
    str x1,  [x16, #0x10]           // RDX
    str x27, [x16, #0x18]           // RBX
    mov x10, sp
    str x10, [x16, #0x20]           // RSP
    str x29, [x16, #0x28]           // RBP
    stp x25, x26, [x16, #0x30]      // RSI RDI
    stp x2, x3, [x16, #0x40]        // R8 R9
    stp x4, x5, [x16, #0x50]        // R10 R11
    stp x19, x20, [x16, #0x60]      // R12 R13
    stp x21, x22, [x16, #0x70]      // R14 R15
    str x9, [x16, #0x98]            // RIP
    stp q0, q1, [x16, #0xa0]
    stp q2, q3, [x16, #0xc0]
    stp q4, q5, [x16, #0xe0]
    stp q6, q7, [x16, #0x100]
    stp q8, q9, [x16, #0x120]
    stp q10, q11, [x16, #0x140]
    stp q12, q13, [x16, #0x160]
    stp q14, q15, [x16, #0x180]
    LOAD_TEB x10, x11
    str x10, [x16, #0x90]           // GS base = TEB

// x16 = FxiCpu, x17 = CPU area: interpret on the emulator stack until native code is reached.
Lrun:
    mov w10, #1
    strb w10, [x17]                 // InSimulation = 1
    ldr x10, [x17, #0x8]
    mov sp, x10                     // EmulatorStackBase
    mov x0, x16
    bl _fxi_win_glue_run            // returns the native target (does not return on errors)
    mov x9, x0
    LOAD_TEB x16, x17
    ldr x17, [x16, #0x1788]
    strb wzr, [x17]                 // InSimulation = 0
    ldr x16, [x17, #0x30]
    ldr x8,  [x16, #0x00]
    ldr x0,  [x16, #0x08]
    ldr x1,  [x16, #0x10]
    ldr x27, [x16, #0x18]
    ldr x10, [x16, #0x20]
    mov sp, x10
    ldr x29, [x16, #0x28]
    ldp x25, x26, [x16, #0x30]
    ldp x2, x3, [x16, #0x40]
    ldp x4, x5, [x16, #0x50]
    ldp x19, x20, [x16, #0x60]
    ldp x21, x22, [x16, #0x70]
    ldp q0, q1, [x16, #0xa0]
    ldp q2, q3, [x16, #0xc0]
    ldp q4, q5, [x16, #0xe0]
    ldp q6, q7, [x16, #0x100]
    ldp q8, q9, [x16, #0x120]
    ldp q10, q11, [x16, #0x140]
    ldp q12, q13, [x16, #0x160]
    ldp q14, q15, [x16, #0x180]

// Continue in native code at x9, sp = RSP. Either a return into an exit thunk (the word
// before the target is its `blr x16`), or a call to an ARM64EC function: its entry thunk
// sits at target + the offset stored in the 4 bytes before it, and gets the x64 return
// address in lr. If the x64 caller left RSP only 8-byte aligned, the return address stays
// on the stack and the entry thunk returns to an x64 `ret` instead (FEX does the same).
Lexit_ec:
    mov x17, x9
    movz w16, #0x0200
    movk w16, #0xd63f, lsl #16      // blr x16
    ldursw x23, [x17, #-4]
    cmp w23, w16
    b.eq Lbr
    and x23, x23, #-4
    add x17, x17, x23
    mov x4, sp
    tbz x4, #3, Lmisaligned
    ldr x30, [x4], #8
    mov sp, x4
Lbr:
    br x17
Lmisaligned:
    add x4, x4, #8
    adrp x30, _fxi_win_x64_ret@PAGE
    add x30, x30, _fxi_win_x64_ret@PAGEOFF
    br x17

// [BeginSimulation] run the AMD64 CONTEXT at CPU area +0x18 (thread start, NtContinue).
    .globl _fxi_win_begin_simulation
_fxi_win_begin_simulation:
    LOAD_TEB x16, x17
    ldr x17, [x16, #0x1788]
    ldr x10, [x17, #0x8]
    mov sp, x10
    ldr x16, [x17, #0x30]
    cbz x16, Lno_cpu
    mov x0, x16
    ldr x1, [x17, #0x18]
    bl _fxi_win_glue_load_context
    LOAD_TEB x16, x17
    ldr x17, [x16, #0x1788]
    ldr x16, [x17, #0x30]
    b Lrun

// fxi_win_jump_stack(sp, pc): continue at pc on a new stack, abandoning this one (an x64
// exception: KiUserExceptionDispatcher on the guest stack, fxi_win_host.c raise_x64).
    .globl _fxi_win_jump_stack
_fxi_win_jump_stack:
    mov sp, x0
    br x1

// No FxiCpu on this thread (ThreadInit did not run): report on the emulator stack.
Lno_cpu:
    ldr x10, [x17, #0x8]
    mov sp, x10
    bl _fxi_win_glue_no_cpu
    brk #0xf0f

    .data
    .globl _fxi_win_x64_ret
_fxi_win_x64_ret:
    .byte 0xc3                      // x64 `ret`, run by FXI (see Lmisaligned)
    .p2align 3
