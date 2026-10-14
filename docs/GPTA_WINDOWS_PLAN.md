# GPTA Windows and device integration plan

Implementation follows the speed threshold: both frozen evaluation suites must
reach the required arithmetic mean on ARM64 CI with all correctness gates passing.
The current Linux results do not establish Wine or iPhone performance.

## Precise architectural state

Retain the current immutable handler index, original guest RIP on each real
instruction boundary, vector permutation recovery, and separate boundaries for
static direct continuations. Build the index before threads can fault or be
suspended; signal handlers must not allocate or initialize it.
Gate the recovery register map against the actual Linux and iOS compiler
output. A successful iOS compile alone does not validate the hard-coded
`preserve_none` register assignment used by host-state recovery.

Windows must retain the flags visible at a fault or suspension even when later
guest code overwrites them. Begin with conservative Windows flag recording:
every flag writer updates its lazy representation. Keep materialization lazy.
Validate the old flags before faulting memory writers, divide errors, invalid
instructions and native transitions. Remove the inherited dead-flag fault
exemptions only when these cases are exact. Report any Windows performance
cost separately; the Linux measurements do not substitute for it.

Preserve the fixed GPR/RIP/XMM layout consumed by the transition assembly.
Check both low and high vector banks, partial scalar writes, aliases, direct
continuations, native return-ring re-entry and changed contexts.

## Thread suspension and Mono GC

Integrate the actual context/suspension path with coherent guest state. The
current host bridge calls pinned recovery for faults; that alone does not
establish Mono GC suspension correctness.

Use a cooperative request/park handshake at bounded interpreter safe points.
Publish the complete CPU snapshot before acknowledging a stop. Native-call
and fallback phases need an explicit coherence protocol; a partially executed
FXI fallback is not a safe snapshot. Resume by restoring all architectural
state and re-entering at the requested guest RIP, including vector-map setup.
Reads and writes of another thread's context must follow the same handshake.

Exercise forced suspension and context changes during hot traces, direct
continuations, adapters, block translation, slow handlers and native calls.
Keep fault recovery and stopped-thread recovery distinct. Then test Mono's
real thread-context/GC path on the device.

## Shared state and Wine API

Audit every post-publication shared control-flow link and block-cache access.
Pair release publication with suitable atomic reads. Per-thread caches and
immutable pre-publication metadata need their own documented ownership.
Keep translation, return-ring re-entry and native callback registration safe
when several Wine threads execute concurrently.

Add an opaque GPTA shim alongside `fxr_win_shim.c`, using the existing
`win_cpu` function table in `fxi_win_host.c`. Cover CPU creation, running,
TEB, context load/save, exceptions, fault RIP, native calls, profiler samples
and counters. Preserve the `fxi_win_*` contract and transition glue. Validate
namespacing while linking FXI, FXR and GPTA together.

## App and IPA

Add GPTA's ELF bridge, run mode, Performance column and Without JIT selection.
Keep existing engine preferences explicit. Compile and sign all engine code
statically, and retain the iOS arm64 compile/symbol check. Build the IPA with
the existing build workflow after runtime gates pass.

Upgrade device measurement to retain raw nanoseconds and checksums for every
sample, use median-of-five with rotating native/GPTA/FXI order, and compare
identical workloads. Compile native and interpreter code with the same iOS
compiler and optimization flags. Display truncated percentages and means
computed from unrounded ratios. Keep both frozen suites available without
editing or inspecting held-out program internals.

Provide the IPA and ask the user to run Performance on the iPhone. Report
the device measurements explicitly as device results, alongside the linked
ARM64 Linux CI evidence. Also exercise Stick Fight's menu and the Wine
sampling profiler; do not infer game FPS or device speed from Linux ratios.
