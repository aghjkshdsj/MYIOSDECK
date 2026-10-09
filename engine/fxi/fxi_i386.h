// SPDX-License-Identifier: GPL-3.0-or-later
// FXI32: FXI's sources compiled for i386 guests (FXI_I386, engine/fxi32; docs/NO_JIT_WOW64.md).
// The i386 copy links into the app next to FXI itself, so every external fxi_* name, and the
// CPU and instance types, get an fx32_ prefix here. fxi.h includes this first when FXI_I386 is
// set. CI (fxi.yml, FXI32 job) fails if an FXI32 object still defines a global fxi_ symbol.
#ifndef FXI_I386_H
#define FXI_I386_H
#define FxiCpu Fx32Cpu
#define Fxi Fx32
#define fxi_alu_tab fx32_alu_tab
#define fxi_atomic_named fx32_atomic_named
#define fxi_cmov_tab fx32_cmov_tab
#define fxi_cond fx32_cond
#define fxi_ext_tab fx32_ext_tab
#define fxi_f80_load fx32_f80_load
#define fxi_f80_store fx32_f80_store
#define fxi_fail fx32_fail
#define fxi_fjcc_tab fx32_fjcc_tab
#define fxi_flag_af fx32_flag_af
#define fxi_flag_cf fx32_flag_cf
#define fxi_flag_of fx32_flag_of
#define fxi_flag_pf fx32_flag_pf
#define fxi_flag_sf fx32_flag_sf
#define fxi_flag_zf fx32_flag_zf
#define fxi_imul2_tab fx32_imul2_tab
#define fxi_imul3_tab fx32_imul3_tab
#define fxi_insn_length fx32_insn_length
#define fxi_lea_tab fx32_lea_tab
#define fxi_lookup fx32_lookup
#define fxi_mov_tab fx32_mov_tab
#define fxi_muldiv_tab fx32_muldiv_tab
#define fxi_named fx32_named
#define fxi_probe fx32_probe
#define fxi_raise fx32_raise
#define fxi_rflags fx32_rflags
#define fxi_run_elf fx32_run_elf
#define fxi_set_echo_fd fx32_set_echo_fd
#define fxi_set_rflags fx32_set_rflags
#define fxi_setcc_tab fx32_setcc_tab
#define fxi_shift_tab fx32_shift_tab
#define fxi_sse_named fx32_sse_named
#define fxi_stop fx32_stop
#define fxi_syscall fx32_syscall
#define fxi_translate fx32_translate
#define fxi_unary_tab fx32_unary_tab
#define fxi_version fx32_version
#define fxi_vm_new fx32_vm_new
#define fxi_win_exit_block fx32_win_exit_block
#define fxi_win_is_ec fx32_win_is_ec
#define fxi_wow_bop_block fx32_wow_bop_block
#define fxi_x87_fxrstor fx32_x87_fxrstor
#define fxi_x87_fxsave fx32_x87_fxsave
#define fxi_x87_init fx32_x87_init
#define fxi_x87_named fx32_x87_named
#endif
