# GPTA

GPTA is a separately namespaced fork of FXR at
`f9bf149bae500728d11484ea943c2374bcae2568`. It preserves FXI and FXR and inherits
their GPL licensing. All handlers are statically compiled; it generates no
executable code.

## Measured progress

Both suites remain below the 50% target. These are ARM64 Linux arithmetic means
of exact native/engine median ratios; displayed values are truncated. All
required Linux, Windows-state and iOS compile gates passed through 92c2ee4.
The last listed run has passed ARM64; x86 and iOS were pending at this update.

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
