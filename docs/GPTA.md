# GPTA

GPTA is a separately namespaced fork of FXR at
`f9bf149bae500728d11484ea943c2374bcae2568`. It preserves FXI and FXR and inherits
their GPL licensing. All handlers are statically compiled; it generates no
executable code.

## Measured progress

Both suites remain below the 50% target. These are ARM64 Linux arithmetic means
of exact native/engine median ratios; displayed values are truncated. All
required Linux, Windows-state and iOS compile gates passed on every measured
row below. Interrupted or defective trials are recorded separately.

| Commit | CI run | Standard GPTA | Held-out GPTA | Standard FXI | Held-out FXI |
|---|---|---:|---:|---:|---:|
| 750038f | [37978872152](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37978872152) | 22.73% | 23.59% | 8.27% | 13.99% |
| 850e583 | [37980142939](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37980142939) | 24.44% | 23.87% | 8.27% | 14.01% |
| 56d9267 | [38008077789](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38008077789) | 22.42% | 23.66% | 8.20% | 13.99% |
| c886273 | [38008906404](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38008906404) | 25.34% | 23.94% | 8.33% | 13.97% |
| 0b03207 | [38009158655](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38009158655) | 25.01% | 24.11% | 8.34% | 13.99% |
| c2e200f | [38104183988](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38104183988) | 24.97% | 23.85% | 8.46% | 13.99% |
| 92c2ee4 | [38104303289](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38104303289) | 24.60% | 24.16% | 8.47% | 14.04% |
| 75b2c45 | [38104450060](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38104450060) | 25.07% | 23.73% | 8.28% | 13.95% |
| b6ce108 | [38105141310](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38105141310) | 26.82% | 24.14% | 8.20% | 13.99% |
| 828ed0e | [38137673954](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38137673954) | 25.72% | 24.48% | 8.37% | 14.04% |
| 6bb20ff | [38137973007](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38137973007) | 25.50% | 29.74% | 8.22% | 14.00% |
| bcef2e9 | [38138298861](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38138298861) | 26.45% | 28.78% | 8.25% | 13.90% |
| 9aac850 | [38138524225](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38138524225) | 25.80% | 29.46% | 8.22% | 14.00% |
| 21b3843 | [38138628709](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38138628709) | 26.30% | 29.83% | 8.33% | 14.03% |
| c5e05cd | [38138855206](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38138855206) | 26.76% | 29.89% | 8.30% | 13.96% |
| a8564c7 | [38139160111](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38139160111) | 26.25% | 30.52% | 8.20% | 13.98% |
| f7548e5 | [38196176226](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38196176226) | 24.71% | 30.18% | 8.29% | 13.99% |
| 2babf4d | [38322298784](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38322298784) | 27.36% | 30.25% | 8.30% | 13.99% |
| 2e9d06d | [38322530884](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38322530884) | 27.16% | 30.44% | 8.21% | 13.94% |
| 2633909 | [38323423231](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38323423231) | 26.88% | 30.29% | 8.33% | 14.01% |
| 854f508 | [38323651889](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38323651889) | 26.29% | 30.07% | 8.32% | 14.00% |
| 606c551 | [38336291017](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38336291017) | 24.78% | 29.05% | 8.41% | 14.03% |
| 2efd5a0 | [38336924280](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38336924280) | 25.32% | 28.76% | 8.15% | 13.94% |

The third trial (result-width branches and simplified floating comparisons)
regressed. Its ARM64, x86 and iOS gates passed. That trial is rolled back in
the next iteration. The branch
specialization also enabled cross-copy handler replication, so the trial
does not isolate the effect of the flag check. Its standard integer perf
evidence shows increased front-end stalls despite fewer retired instructions.

The fourth run specializes high-XMM register operands and rolls back the third
trial. The fifth adds independent carry liveness for INC/DEC in ELF mode. It
did not improve the standard mean. All gates passed on both runs. The earlier
carry commit 651be44 had a declaration-order error and was superseded by
0b03207; its cancelled run has no valid performance result.

c2e200f specializes SSE memory addressing on each scale; 92c2ee4 specializes
floating compare branches across all XMM registers. Neither increased the
standard mean over its predecessor. 75b2c45 retries result-width branches
without the original trial's replication change. It improves the standard
integer timing but remains well below the required suite means.

The next pair family is `mov D,S ; ALU D,R`, selected from the independent
[corpus run](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37933433358).
Its lowering requires dead output flags and an arithmetic width no greater
than the copy width, handles R aliasing D, and does not reuse the old gap
reordering rule for register operands. Differential cases cover those guards.
Its b6ce108 run passed every gate; both suite means remain below 50%.

Subsequent trials specialize INC/DEC carry preservation from a locally proven
flag producer, add pinned byte/word register ALU and moves (including legacy
high bytes), and remove redundant successor checks from floating compare
branches whose trace edge is statically adjacent. Each remains a general
instruction or pair optimization. No held-out program is inspected to select
these changes. Results must be recorded after CI; unrecorded trials are not
measured.

The carry trial's gates all passed; its standard mean regressed while its
held-out mean increased. The trace trials retain each guest branch and
specialize only an adjacent successor proven during ordinary trace construction.
A subsequent isolated code-layout trial aligns static handler entries to 32
bytes, motivated by front-end stalls in the standard memory/branch profiles.
Its measured results are recorded above; both means remain below 50%.

The narrow-register trial passes all gates. The
standard branch profile also identifies indexed immediate stores as costly;
the next trial specializes all four address scales for every immediate-store
width and adds exact-fault sites 20-23. The held-out source remains frozen and
is not used to select these changes.

a8564c7 fixes both source-register offsets for SSE copy/arithmetic pairs,
including all high XMM sources. All gates passed. The next trial specializes
single-instruction SSE memory arithmetic across all four address scales.
The standard SIMD perf profile and disassembly identify runtime scale loads
in those handlers; its speed effect remains unmeasured until CI completes.

The combined f7548e5 trial adds static ARM64 scalar floating branches, adjacent
arithmetic trace edges, and fixed XMM destination offsets. The first attempts
at 8bd055a and 5c001ab lost their ARM runners during compilation (x86 and iOS
passed); their ARM retries also ended without timings. Their separate ARM speeds
remain not measured. fb5901c introduced a replication bug:
the last trace copy inherited an adjacent-only branch from the first copy,
causing the standard memory workload to crash. Its run and the superseded
470a08d/cc644e1 runs are invalid, not speed results. f7548e5 fixes per-copy
replication, adds direct backedge regression cases, and passes all gates.
Compilers now process the SSE scale variants in separate translation units
with bounded parallelism.

The corrected combined trial regresses the standard mean. Its standard branch
profile retires fewer instructions but shows much higher front-end stalls.
The next comparison disables arithmetic adjacent-edge handler selection while
retaining the replication correctness fix. No held-out internals informed it.

2babf4d disables those arithmetic trace handlers and adds iOS compiler caching.
2e9d06d statically specializes every legal nonzero immediate shift/rotate count
when flags are dead. 2633909 extends INC/DEC-and-branch pairs to a single
flag-reading edge, reconstructing that edge's flags while retaining incoming
carry. 854f508 extends zero-displacement SSE moves to every addressing scale.
All passed the gates; neither later suite mean improves over 2babf4d.

ee638cf carries sequential dispatch lookahead in the spare ARM64 integer
argument register. Its [run](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38323912475)
passed instruction correctness, fault sites, the assembly audit and iOS
compilation, but sampled fewer stopped loop states than the unchanged minimum.
ARM64 speed is not measured. The next trial extends only that diagnostic
workload, retaining the exact-state acceptance threshold. Dispatch lookahead
uses allocated sentinel uops, including one-uop stop and native exit blocks.

606c551 passed all gates with the extended sampling workload. Dispatch
lookahead reduced both means relative to its predecessor. 2efd5a0 adds
general single-handler memory-source ADD/SUB/AND/OR/XOR and passes all gates.
Neither suite reaches 50%. The next experiments pack all sixteen low XMM
lanes into eight vector arguments, then restore ordinary dispatch. These
experiments are not measured until their own CI gates and timings complete.

Raw per-sample nanoseconds, checksums, commands and return codes are in each
run's `gpta-evidence-ubuntu-24.04-arm` artifact (`gpta-results.json`). The first
run is an inherited FXR baseline, not a new speed improvement. The second adds
single-dispatch SSE memory arithmetic and direct result-only flag checks.
GPTA iPhone performance is **not measured**.

The held-out suite stays frozen at commit
`57ce94e8ff5c7d39fbf22b73997a60317c31437e`, blob
`1728cd7f759f26c9d063baaf72ea91501982fb12`. It is evaluation-only. Do not edit
it or use its source, assembly, or profiles to choose optimizations.

The completion threshold is an arithmetic mean of at least 50% of native on
each suite on ubuntu-24.04-arm. The initial 20% milestone is not completion.
Every iteration must run hello, exact atomics/x87/difftest, all ten Madeira PE
scans, ARM64 Windows state checks, and the iOS arm64 compile/symbol check.
Compare five samples per engine, rotating order on one core, identical scales
and matching checksums. Native and engines use clang-19, -O2, and
-ffp-contract=off. Record every sample and exit status; compute percentages
from raw medians, means from exact ratios, and truncate displayed percentages.

Profile standard workloads with perf stat and perf record. Choose any new
instruction-pair families from the independent corpus in engine/gpta/corpus.sh.
Never replace a recognized guest loop with a host loop.

After the speed threshold: audit inherited fault state reconstruction (including
dead flags at faults), complete Wine suspend/context recovery, wire GPTA into
the app's Performance and Without JIT settings, build the IPA and obtain the
user's iPhone measurements. Existing FXR device results are not GPTA results.
