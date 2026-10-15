#!/bin/bash
# Build the GPTA host tool: engine/gpta/build.sh <out-dir>. Needs clang 19+ for preserve_none on ARM64
# (CC=clang-19); an older clang still builds GPTA, without the pinned-register calling convention.
# The pinned handlers are tens of thousands of small functions in several files: compiled in
# with bounded parallelism (each needs a few GB), without debug info (perf still has the names).
# No tail merging: clang would merge the dispatch tails of a branch handler's two edges into one
# indirect jump with two successors, which Neoverse N2 predicts slower (measured: memory 16% faster).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT/obj"
CC="${CC:-clang}"
"$CC" --version | head -1
ARCH_FLAGS=()
if [ "$(uname -m)" = x86_64 ]; then ARCH_FLAGS=(-mcx16); fi
CFLAGS="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
-Wno-unused-but-set-variable -Wno-sign-compare -ffp-contract=off -fno-math-errno -mllvm -enable-tail-merge=false ${ARCH_FLAGS[*]:-} ${EXTRA_CFLAGS:-}"
SRCS="gpta_pin_pl_paddb.c gpta_pin_pl_paddb_r1.c gpta_pin_pl_paddw.c gpta_pin_pl_paddw_r1.c gpta_pin_pl_paddd.c gpta_pin_pl_paddd_r1.c gpta_pin_pl_paddq.c gpta_pin_pl_paddq_r1.c gpta_pin_pl_psubb.c gpta_pin_pl_psubb_r1.c gpta_pin_pl_psubw.c gpta_pin_pl_psubw_r1.c gpta_pin_pl_psubd.c gpta_pin_pl_psubd_r1.c gpta_pin_pl_psubq.c gpta_pin_pl_psubq_r1.c gpta_pin_pl_pand.c gpta_pin_pl_pand_r1.c gpta_pin_pl_pandn.c gpta_pin_pl_pandn_r1.c gpta_pin_pl_por.c gpta_pin_pl_por_r1.c gpta_pin_pl_pxor.c gpta_pin_pl_pxor_r1.c gpta_pin_lp_s0.c gpta_pin_lp_s0_r1.c gpta_pin_lp_s1.c gpta_pin_lp_s1_r1.c gpta_pin_lp_s2.c gpta_pin_lp_s2_r1.c gpta_pin_lp_s3.c gpta_pin_lp_s3_r1.c gpta_pin_sx_shl.c gpta_pin_sx_shl_r1.c gpta_pin_sx_shr.c gpta_pin_sx_shr_r1.c gpta_pin_sx_sar.c gpta_pin_sx_sar_r1.c gpta_pin_sx_rol.c gpta_pin_sx_rol_r1.c gpta_pin_sx_ror.c gpta_pin_sx_ror_r1.c gpta_pin_af_addss.c gpta_pin_af_addss_r1.c gpta_pin_af_subss.c gpta_pin_af_subss_r1.c gpta_pin_af_mulss.c gpta_pin_af_mulss_r1.c gpta_pin_af_divss.c gpta_pin_af_divss_r1.c gpta_pin_af_addsd.c gpta_pin_af_addsd_r1.c gpta_pin_af_subsd.c gpta_pin_af_subsd_r1.c gpta_pin_af_mulsd.c gpta_pin_af_mulsd_r1.c gpta_pin_af_divsd.c gpta_pin_af_divsd_r1.c gpta_pin_movtest.c gpta_pin_movtest_r1.c gpta_pin_eareuse_r.c gpta_pin_eareuse_r_r1.c gpta_pin_eareuse_x0.c gpta_pin_eareuse_x0_r1.c gpta_pin_eareuse_x1.c gpta_pin_eareuse_x1_r1.c gpta_pin_eareuse_x2.c gpta_pin_eareuse_x2_r1.c gpta_pin_eareuse_x3.c gpta_pin_eareuse_x3_r1.c gpta_pin_eareuse_xs.c gpta_pin_eareuse_xs_r1.c gpta_pin_xload_ps.c gpta_pin_xload_ps_r1.c gpta_pin_xload_pd.c gpta_pin_xload_pd_r1.c gpta_pin_xload_ss.c gpta_pin_xload_ss_r1.c gpta_pin_xload_sd.c gpta_pin_xload_sd_r1.c gpta_pin_unary_trace.c gpta_pin_unary_trace_r1.c gpta_pin_cfonly.c gpta_pin_cfonly_r1.c gpta_pin_csh_shl.c gpta_pin_csh_shl_r1.c gpta_pin_csh_shr.c gpta_pin_csh_shr_r1.c gpta_pin_csh_sar.c gpta_pin_csh_sar_r1.c gpta_pin_csh_rol.c gpta_pin_csh_rol_r1.c gpta_pin_csh_ror.c gpta_pin_csh_ror_r1.c gpta_pin_xscalar.c gpta_pin_xscalar_r1.c gpta_pin_stzero.c gpta_pin_stzero_r1.c gpta_pin_am_add.c gpta_pin_am_add_r1.c gpta_pin_am_sub.c gpta_pin_am_sub_r1.c gpta_pin_am_and.c gpta_pin_am_and_r1.c gpta_pin_am_or.c gpta_pin_am_or_r1.c gpta_pin_am_xor.c gpta_pin_am_xor_r1.c gpta_pin_xzero.c gpta_pin_xzero_r1.c gpta_pin_unary_stub.c gpta_pin_shconst.c gpta_pin_shconst_r1.c gpta_pin_step_trace.c gpta_pin_step_trace_r1.c gpta_pin_fcj_trace.c gpta_pin_narrow.c gpta_pin_op3r.c gpta_pin_op3r_r1.c gpta_pin_xmem.c gpta_pin_xmem_r1.c gpta_pin_xmem_s1.c gpta_pin_xmem_s1_r1.c gpta_pin_xmem_s2.c gpta_pin_xmem_s2_r1.c gpta_pin_xmem_s3.c gpta_pin_xmem_s3_r1.c gpta_pin_x3.c gpta_pin_arj.c gpta_pin_lcj.c gpta_pin_stub.c gpta_pin_step1.c gpta_pin_step2.c gpta_pin_step3.c gpta_pin_mem.c gpta_pin_mem2.c gpta_pin_fuse.c gpta_pin_sse.c gpta_pin_alu.c gpta_pin_br.c gpta_pin.c
      gpta_pin_alu_r1.c gpta_pin_sse_r1.c gpta_pin_mem_r1.c gpta_pin_mem2_r1.c gpta_pin_step3_r1.c gpta_pin_br_r1.c gpta_pin_step1_r1.c gpta_pin_step2_r1.c
      fxi_core.c fxi_decode.c fxi_flags.c fxi_ops.c fxi_sse.c fxi_atomic.c fxi_x87.c fxi_win.c fxi_main.c"
JOBS=2   # bound concurrent compiler memory for the generated static handlers
export CC CFLAGS HERE OUT
printf '%s\n' $SRCS | xargs -P "$JOBS" -I{} sh -c '$CC $CFLAGS -c "$HERE/{}" -o "$OUT/obj/$(basename {} .c).o" || exit 255'
if [ "$(uname -m)" = aarch64 ]; then
    python3 "$HERE/gen_pload_asm.py" "$OUT/asm"
    python3 "$HERE/gen_fload_asm.py" "$OUT/asm"
    find "$OUT/asm" -name '*.S' -print0 | xargs -0 -P "$JOBS" -I{} sh -c '$CC $CFLAGS -I "$HERE" -c "{}" -o "$OUT/obj/$(basename {} .S).o" || exit 255'
fi
"$CC" -o "$OUT/gpta" "$OUT"/obj/*.o -lm -lpthread
ls -la "$OUT/gpta" "$OUT"/obj/gpta_pin*.o
