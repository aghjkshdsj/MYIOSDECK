# FXR: the no-JIT x86-64 interpreter at 20% of native

FXR (engine/fxr) is FXI (engine/fxi, docs/FAST_INTERPRETER.md) with the guest registers pinned
in host registers. Like FXI it creates no executable memory: every handler is ordinary compiled C
(clang `musttail` + `preserve_none`), so it fits App Store guideline 4.7. This page records how
it reached 20% of native speed, how that is measured, its Windows mode, and why it became the
default x64 CPU of 64-bit games.

## Result (ubuntu-24.04-arm, Neoverse N2)

Share of native speed, mean over the kernels, each kernel the median of 5 runs in rotating
order, checksums equal to native, native and guest built by the same clang 19 at
`-O2 -ffp-contract=off` (engine/fxr/compare.sh, .github/workflows/fxr.yml):

| run | commit | standard (5 kernels) | held-out (11 kernels) |
|---|---|---|---|
| [37932118924](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37932118924) | 56610c7 | 20.88% | 28.78% |
| [37933739423](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37933739423) | 7bad816 (same engine) | 21.90% | 28.37% |
| [37935481572](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37935481572) | 988b2b3 | 21.84% | 28.60% |
| [37938411090](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37938411090) | 653fcf5 | 20.61% | 28.78% |
| [37940557650](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37940557650) | 5951962 | 21.05% | 28.38% |
| [37947027011](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37947027011) | 651368e (R15 in x20) | 21.50% | 28.91% |

FXI on the same runs: about 7.3% (standard) and 13.3% (held-out). Run-to-run noise is several
points per kernel (code layout: the frontend is the limit), so read single kernels with care.

The held-out benchmark (engine/guest/heldout.c) was written and frozen before the work (commit
57ce94e; CI checks its blob), and no engine code refers to it: it checks that a speed-up is
general. Superinstructions are chosen from a corpus of other programs (engine/fxr/corpus.sh:
system tools and libraries, game libraries, Madeira's Windows test programs), never from the
benchmark; nothing detects or special-cases a benchmark loop.

## What made the difference

Measured with perf and with engine/fxr-probe/dispatch_probe.c on the runner:

- A threaded dispatch costs about 1.3 cycles when every indirect branch has one successor, but
  3-4 cycles when the same handler's branch has several (predicted right, no mispredictions: the
  frontend stalls), and 4 cycles when the next uop pointer comes from a load.
- **Loop traces** (fxr_pin.c, fxr_lower): a loop's blocks laid out one after another in copies;
  edges along the trace step to the next uop with an add (PCHAIN_T) instead of loading the link.
- **Handler replicas** (fxr_pin_*_r1.c): the hot handler files compiled twice; a handler used at
  two places in a block or loop copy runs the replica there, so each indirect branch keeps one
  successor (Ertl and Gregg's replication). Copies alternate replicas only when every handler of
  the copy has one.
- **No tail merging** (`-mllvm -enable-tail-merge=false`): clang merged a branch handler's two
  dispatch tails into one indirect jump with two successors.
- Superinstructions from the corpus: compare/test + branch forms, ALU + branch, loop steps, the
  3-operand `mov D,S ; op D,imm` pair (also across one independent instruction), SSE copy + op,
  load + test/compare + branch; lean `[base + index]` loads and stores.
- Flag liveness across blocks, flag stubs on the edges that need flags.

## On the phone

The app's Performance tab runs both sets ("Also measure without JIT"): FXI and FXR at 1/20 of
each kernel's workload, native at the same size, checksums compared. The report (Share report,
or Files › MYIOSDECK › benchmarks) has both tables.

## Windows mode (FXR as the x64 CPU of Wine's ARM64EC processes)

FXR is the x64 CPU of 64-bit Windows games without JIT by default since build 178
(Settings › Without JIT › Windows games: FXR, on; off gives FXI). FXI (engine/fxi/fxi_win.c,
App/Sources/Native/fxi_win_host.c) ran them first. FXR has the same entry points (fxr_win_*,
engine/fxr/fxi_win.c), but keeps the guest registers in host registers, so what Windows needs
from a CPU had to be rebuilt:

| need | how | status |
|---|---|---|
| exact fault rip and registers | `fxr_win_host_state(c, host, 1, &rip)`: from the host registers at the fault (they hold the state before the instruction), the uop from x21 and the handler containing the pc | done; CI (engine/fxr-probe/winstate_test.c): 12 fault sites (load, store, 16-byte load/store with index, push, pop, call, ret, call through memory, cmp memory + jcc, load + test + jcc, read-modify-write) give rip, GPRs, flags and XMM0-7 exactly |
| no handler writes guest-visible state before its last memory access | engine/fxr-probe/precise.py over the handler section's disassembly; R15 lives in x20 (no guest register in the compiler's first scratch registers), barriers bound to the host register keep the flag words and destinations until the access; the pair fusions that would (call/pop/push pairs) are not used in Windows mode | done; CI check |
| state recoverable at a thread suspension | `fxr_win_host_state(c, host, 0, &rip)`: exact at a handler's start or dispatch, else "run on and retry"; fused pairs report their first instruction, reordered pairs and flag stubs are no boundary | done; CI: a thread stops the guest every 20 us, every state reported exact matches the loop's known registers (about 1300 per run, 0 wrong) |
| faults outside the pinned handlers | block lookups, fs/gs calls and slow paths spill the state first; FXI's rule then applies (c->cur) | done |
| atomic link updates | links are filled with release stores, traces are linked before they are published, the indirect-branch cache is per thread | done |
| another program in the session | fxi_win_invalidate_all (fxr_invalidate_all, fxi_core.c): an empty block table, and every old block's first uop becomes a re-lookup (p_relookup), which is where links, loop traces and indirect-branch cache entries lead; each thread empties its cache when the generation changed (fxr_ibtc_sync). Nothing is freed (a thread of an ended game may be inside a block). wine_host.c calls it before a later program, as for FXI (docs/SESSION_REUSE.md) | done (build 178); CI: engine/fxr-probe/invalidate_test.c, program A then B at the same addresses 400 times with a thread looping inside a dropped block, FXR and FXI |
| fxi_win_* API | the same functions, renamed fxr_win_* (App/Sources/Native/fxr_win_shim.c wraps them) | done |
| host integration | fxi_win_host.c picks the CPU (FXR, the default since build 178, or FXI with Settings › Without JIT › Windows games: FXR off) and passes Wine's ARM64 context, or the Mach thread and NEON state, to fxr_win_host_state at a fault | done; on the device since build 118 (Stick Fight, Crab Game) |
| suspension in Wine | Wine's NtGetContextThread on an ARM64EC thread in simulation would ask the CPU (as for faults: a hook in Madeira's server); retry while fxr_win_host_state returns 0 | to do; not needed for FXR to be the default: on iOS another thread's context is the state at its last call into Windows for every CPU (FXI and FEX too, docs/NO_JIT_WOW64.md), and there FXR's state is in memory as FXI's is |
| cheap calls into Wine | TlsGetValue and the uncontended RtlEnter/LeaveCriticalSection run in FXR itself (fxr_pin.c, p_nat_*: Wine's code paths, same fields and atomics; Wine's function when a lock is held elsewhere or has waiters); the host names the targets (fxi_win_host.c, host_native_kind) | done (build 119); CI: winstate_test winnative |
| where the time goes | the profiler samples FXR threads' pc and x21: pinned handlers, FXI handlers by instruction kind, lookups, entering from native code | done (build 119) |
| pinned forms games need | fxr_pin_win.c: inc/dec/not/neg of memory, 8/16-bit loads, `mov r, gs:[disp]` (the TEB), one-operand mul/imul/div/idiv (a divide error or a 128-bit dividend: FXI's handler) | done (build 119); difftest |

One relaxation, as in FXI: flags that dead-flag elimination never computed are not recovered.
At a fault in an instruction that writes the flags itself (cmp/test/add with a memory operand),
the flags of the instruction before it were dead and are reported stale; running the faulting
instruction again recomputes them.

## FXR as the default x64 CPU (build 178)

What decided it, against FXI:
- **Every 64-bit game of a kept session** (docs/SESSION_REUSE.md): FXR now drops its blocks before
  a later program, as FXI does (the table row above). Until then only the first 64-bit game of an
  app run could use FXR.
- **Thread suspension is not worse.** iOS has no real suspend: another thread's GetThreadContext
  gets the state of the thread's last call into Windows, for FXI, FXR and FEX alike
  (docs/NO_JIT_WOW64.md, section 4 risk 2, for the 32-bit side). At such a call the glue hands
  Wine the x64 registers from the CPU structure, for FXR as for FXI. A garbage collector that
  stops the world (Mono's, in Unity games such as Stick Fight) sees the same on both;
  Wine asking the CPU (the open row above) would help both.
- **Code changed in place** (a runtime patching code it generated): neither CPU drops the blocks
  of a range in Windows mode (Wine's notifications to the no-JIT emulator are empty), so equal.
- **On the device**: Stick Fight and Crab Game since builds 118-119; Stick Fight after 32-bit
  games in a kept session (build 173 on). FXR-only lowering bugs are the risk (build 118's
  loop-step hoist, Crab Game): CI runs difftest, atomics and x87 on both, and the toggle stays.

Every 64-bit game of an app run uses the first one's CPU (WineController `fxrThisRun`): the
choice is one global of the Wine session (fxi_win_host.c `g_cpu`), and threads an ended game
left running keep the CPU they were made for. Changing the toggle applies after a restart then.
