# GPTA

GPTA is a separately namespaced fork of FXR at
`f9bf149bae500728d11484ea943c2374bcae2568`. It preserves FXI and FXR and inherits
their GPL licensing. All handlers are statically compiled; it generates no
executable code.

## Measured progress

Both suites remain below the 50% target. These are ARM64 Linux arithmetic means
of exact native/engine median ratios; displayed values are truncated. All
required Linux, Windows-state and iOS compile gates passed on the first two runs.

| Commit | CI run | Standard GPTA | Held-out GPTA | Standard FXI | Held-out FXI |
|---|---|---:|---:|---:|---:|
| 750038f | [37978872152](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37978872152) | 22.73% | 23.59% | 8.27% | 13.99% |
| 850e583 | [37980142939](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/37980142939) | 24.44% | 23.87% | 8.27% | 14.01% |
| 56d9267 | [38008077789](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38008077789) | 22.42% | 23.66% | 8.20% | 13.99% |

The third trial (result-width branches and simplified floating comparisons)
regressed. Its ARM64 gates passed; x86 and iOS were pending when this entry
was written. That trial is rolled back in the next iteration. The branch
specialization also enabled cross-copy handler replication, so the trial
does not isolate the effect of the flag check. Its standard integer perf
evidence shows increased front-end stalls despite fewer retired instructions.

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
