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
| 845e2ab | [38409262613](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38409262613) | 23.88% | 28.18% | 8.49% | 13.96% |
| 39daaf6 | [38409596148](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38409596148) | 23.76% | 29.39% | 8.34% | 14.03% |
| ede3693 | [38450894640](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38450894640) | 27.04% | 31.41% | 8.34% | 13.97% |
| f087717 | [38455426012](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38455426012) | 26.96% | 31.09% | 8.29% | 13.98% |
| d13fd8f | [38455809669](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38455809669) | 26.50% | 31.39% | 8.17% | 14.00% |
| 8285258 | [38456267739](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38456267739) | 27.69% | 31.78% | 8.20% | 13.96% |
| 11de320 | [38458064927](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38458064927) | 26.28% | 26.94% | 8.40% | 13.98% |
| 9a102a0 | [38488394754](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38488394754) | 27.01% | 31.16% | 8.27% | 13.96% |
| f6ce896 | [38488865639](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38488865639) | 27.63% | 31.33% | 8.30% | 13.98% |
| 19ed75e | [38489068854](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38489068854) | 28.06% | 31.53% | 8.19% | 13.98% |
| 1f2d829 | [38489428585](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38489428585) | 27.75% | 30.66% | 8.39% | 13.97% |
| f15a6a0 | [38489988449](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38489988449) | 27.62% | 31.29% | 8.24% | 14.02% |
| 2a44b68 | [38490572829](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38490572829) | 24.77% | 30.73% | 8.31% | 13.97% |
| 8930ebc | [38491164550](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38491164550) | 24.71% | 30.47% | 8.16% | 13.99% |
| 2e34eab | [38491558263](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38491558263) | 28.16% | 30.58% | 8.31% | 13.98% |
| 439ac59 | [38492014687](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38492014687) | 27.29% | 30.15% | 8.27% | 13.94% |
| e723470 | [38492613538](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38492613538) | 28.29% | 31.11% | 8.20% | 14.00% |
| 2120516 | [38492982469](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38492982469) | 27.49% | 31.40% | 8.12% | 13.98% |
| b782565 | [38493209646](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38493209646) | 27.53% | 30.87% | 8.30% | 13.98% |
| 45ee02a | [38493782478](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38493782478) | 28.08% | 29.78% | 8.49% | 14.00% |
| 5ada1ce | [38494977196](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38494977196) | 28.69% | 30.61% | 8.32% | 13.96% |
| 7d267f1 | [38495163093](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38495163093) | 28.41% | 31.05% | 8.27% | 13.96% |
| 60ff6de | [38497514290](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38497514290) | 28.80% | 30.86% | 8.33% | 14.02% |
| f9d296c | [38497748838](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38497748838) | 28.26% | 30.17% | 8.35% | 13.98% |
| d34483e | [38498960864](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38498960864) | 28.43% | 31.15% | 8.27% | 13.93% |
| 650279c | [38499323627](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38499323627) | 28.71% | 30.80% | 8.32% | 13.98% |
| fe5a70c | [38500223751](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38500223751) | 27.87% | 30.60% | 8.25% | 13.97% |
| 9a01af4 | [38641833700](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38641833700) | 29.02% | 31.29% | 8.33% | 13.97% |

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
experiments passed all gates, but packed low XMM lanes strongly regressed
standard SIMD. The next trial restores full-vector pinning and adds general
zero-displacement immediate stores. No program-dependent layout is selected.

Raw per-sample nanoseconds, checksums, commands and return codes are in each
run's `gpta-evidence-ubuntu-24.04-arm` artifact (`gpta-results.json`). The first
run is an inherited FXR baseline, not a new speed improvement. The second adds
single-dispatch SSE memory arithmetic and direct result-only flag checks.
GPTA iPhone performance is **not measured**.

The trace-local XMM allocation trial passed every gate, including the same
Windows state checks with allocation enabled, but regressed. The standard
float profile shows map-entry overhead. It selects eight
full vectors by uniform counts of explicit operands, breaking ties by guest
register number. Each selected set has a stable permutation; constants,
symbols, addresses and checksums never participate. External block entries
adapt the vector map, while internal trace edges keep it. Unpinned physical
slots use a separate CPU bank, and FXI fallbacks receive architectural XMM
state and original operand indices. The hot uop stride stays unchanged.

The adapter trial passed every gate; both means remain below 50%.
Traces with no explicit vector
operands retain the caller's map. Direct links can skip an equal-map adapter
only when their source establishes an immutable map; map-free sources retain
the target adapter because different callers can supply different maps.
Indirect caches continue to point at complete block entries. A differential
case reaches one vector target through a shared map-free jump under different
incoming maps.

The carry-only flag trial passed every gate; both means remain below 50%.
A separate backward liveness
bit tracks non-carry arithmetic flags. When only carry remains live, register
and immediate ADD/SUB/AND/OR/XOR retain their usual lazy kind but update only
the operands its carry formula needs. INC/DEC replace non-carry flags; unknown
readers, including SYSCALL's R11 flag snapshot, remain conservative. Windows
and GPTA_WINLOWER retain full flag bookkeeping. Differential cases cover both
widths, register aliases, immediate forms, narrow unary consumers and direct
block boundaries.

The loop-region allocation trial passed every gate; both means remain below
50%. Newly decoded vector
blocks inside a cached ELF loop region inherit its immutable map instead of
choosing a different map for each block. The entry adapter stores the region
boundaries in otherwise unused metadata, with the same lifetime as its block.
The first map still comes from uniform explicit-operand counts. Every guest
instruction and branch remains interpreted. Windows retains the preceding
allocation policy. Differential coverage alternates vector subsets across
conditional paths and an inner loop while keeping all sixteen vectors live.

The unary adjacent-edge trial passed every gate but regressed both suite
means. Existing register INC/DEC
plus ZF/SF branch pairs receive handlers that use a successor proven adjacent
by trace construction. Other edges retain the existing links and flag exit
stubs; every unary instruction and branch still executes. Handler replicas
and host-state indexing include the new forms. Differential coverage checks
both widths, all four supported conditions, both adjacent edges and final
exits after replicated copies, including sign transitions and zero extension.

The SSE load/arithmetic pair trial passed every gate, including its new fault
cases; both suite means remain below 50%. The independent corpus
counts SSE load followed by an operation on its destination. Base-only loads
followed by register ADD/SUB/MUL now share a handler for packed single/double
and scalar single/double forms. Operand registers are static, displacement
remains general, and aliases read the newly loaded value. The one guest access
precedes every visible side effect. New differential cases cover all sixteen
XMM destinations, upper-lane clearing, aliases and preserved flags. New exact
fault sites 49-52 check nonzero upper lanes for each type, with both Windows
allocation policies; the unchanged assembly audit checks every new handler.

The branch-layout trial passed every gate, but its standard branch kernel
regressed sharply. The following rollback restores the preceding handler
selection; its independent run passed every gate and restored the branch
timing, while both suite means remain below 50%. The unused variants remain
compiled and indexed for reproducibility. Existing immediate-compare step
pairs and unary zero/sign pairs use a handler selected from the layout of
each actual copy: a proven adjacent edge steps directly, and other copies
chain through links without runtime adjacency checks. This revisits the
arithmetic adjacent handlers disabled after iteration 21, together with
non-adjacent handlers for ordinary blocks and final backedges. Replication
still uses each copy's actual handler. The existing complete condition,
width, alias and wraparound cases now also compare final exit flags against
native execution. All guest instructions and branches remain interpreted.

The loop-only allocation trial passed every gate but regressed both means
relative to its predecessor. Its run still contains the branch-layout trial;
the next iteration measures that selection's rollback independently.
Vector blocks outside a
known cached or newly identified ELF loop region use the common identity map.
Nonidentity allocation is reserved for a region where its map can be reused
across iterations. This is a uniform CFG policy, using the same operand counts
and existing bounded backedge scan; no program identity or data participates.
The preceding transition/nested-loop/fallback tests cover both layouts.
Windows allocation and all recovery gates retain their preceding behavior.

The effective-address reuse trial passed every gate, including faults after
a completed first load, but regressed both means. Its selection is rolled
back next while retaining all stronger correctness cases and indexed
handlers. That rollback passed every gate, but did not recover the standard
memory timing in its run. Both suite means remain below 50%. Consecutive GPR/vector
loads with the same base/index/scale compute that address once in T. Each load
still has its own dispatch, original RIP and instruction boundary; canonical
address metadata remains intact. A first GPR destination that changes the
base or index prevents reuse. Kinds cover 32/64-bit GPR loads, zero-extended
bytes, and full/scalar vector loads, including mixed pairs and aliases. The
independent corpus contains consecutive GPR loads and scalar SSE loads.
Differential cases cover address scales, displacement differences, zero
differences, aliases and rejected base/index changes. New fault sites 53-56
check first-load faults and second-load faults after a completed GPR/vector
update, including the second instruction's exact RIP, with both Windows
allocation modes. Handler replicas and the unchanged recovery audit include
every new handler.

The vector memory-cost allocation trial passed every gate; both suite means
remain below 50%. Instead of
counting each operand mention once, allocation counts uniform high-bank
reads and writes from instruction semantics: arithmetic/partial destinations
need both, full overwrites need only a write, and aliased reads count once.
The existing corpus-selected copy/arithmetic and load/arithmetic pairs keep
their intermediate result in a handler temporary, so the model counts only
their actual source reads and final destination write. No guest data,
constants, symbols, checksums or program identity participate. Stable tie
breaking, region policy and all architectural recovery paths are unchanged.
The complete existing XMM differential and allocation-enabled Windows gates
validate the newly chosen permutations.

The register-copy/test/branch pair trial passed every gate; both suite means
remain below 50%. The independent
corpus records this adjacent family. A register copy followed by the existing
fused TEST of its destination and JCC now uses one handler, covering both
copy and test widths, all conditions and source/destination aliases. Chaining
retains the preceding behavior; live flag exits use existing TEST stubs.
Differential cases compare the copied value, direction and defined flags
across all width combinations, conditions and self copies. Static handler
replicas and host-state indexing cover every form. No guest loop is replaced.

The repaired scalar arithmetic/comparison group trial passed every gate,
but did not improve the standard float timing. Both means remain below 50%.
The next diagnostics-only run leaves its interpreter unchanged and likewise
passes all gates, with fresh timings recorded above. Its family
was selected from the independent system/library/Madeira corpus in run
[38493680532](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38493680532),
which records arithmetic followed by COMIS/UCOMIS comparing the result and a
branch. Register ADD/SUB/MUL/DIV for either scalar precision plus comparison
of the result as the first operand and JCC now share a handler. No guest loop
is replaced. Every condition, low/high operand class and alias is supported;
scalar upper lanes survive, and live flag exits retain existing comparison
stubs. The static FCMP condition selector is shared unchanged with the
preceding floating trace handlers. Differential cases check finite arithmetic
results, unordered comparisons, all conditions, upper lanes, aliases and
live exit flags. The preceding floating trace case exercises final backedges.
All replicas are indexed for host recovery and the unchanged precision audit.

Its first commit, 1fe3bc4, failed compilation in
[38494669280](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38494669280):
the existing C16 helper supplies a trailing empty argument when no extra
arguments are passed, and a forwarding replica macro expanded table aliases
before token pasting. The isolated repair accepts the empty condition
argument and names replica tables directly. No timing ran on the failed
commit; its speed is not measured and its gates did not pass.

45ee02a changes only corpus diagnostics, leaving the interpreter identical to
b782565. Its manually dispatched full gate/measurement run passed every gate;
the fresh timings are recorded above without selecting between either run.
The next diagnostics extension counts same-width immediate shifts/rotates
followed by register XOR consuming the result, recording both operand roles.
It still reads only independent system/library/Madeira programs. No new
shift/XOR handler is selected before corpus evidence is available.

The repaired immediate shift/XOR pair trial passed every gate in 38497514290.
It did not improve standard integer timing. Its two operand roles
are present in independent corpus run
[38495163837](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38495163837).
Same-width register SHL/SHR/SAR/ROL/ROR followed by XOR consuming the result
now share a handler. Counts remain general and use the architectural mask;
there is no constant-specific selection. Both surviving register results and
XOR's live flags remain correct, including self aliases and 32-bit zero
extension. The first shift's flags are overwritten by XOR. Differential
coverage checks every effective count plus wraparound encodings, all five
operations, both widths, both roles, aliases, and live/dead flag paths. All
static replicas are registered for recovery and the unchanged precision gate.

Its first commit, 7f2c73e, failed compilation before gates or timing in
[38496942585](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38496942585):
token pasting retained the literal opcode macro name and generated an
undeclared SX_SX_OP call. The isolated repair expands the opcode before
pasting both handler names and operation names. The failed trial's speed is
not measured. No correctness or recovery gate is weakened.

Standard profiling evidence now includes the standard guest's disassembly
alongside the host handler disassembly and perf reports. This diagnoses why
a newly selected corpus family may leave the sampled dominant handlers
unchanged. Neither held-out source nor assembly nor profiles are inspected.
This evidence-only workflow change does not change interpreter behavior.
The ARM GNU objdump invocation in f9d296c produced an empty disassembly.
The next diagnostic revision creates it on the x86 guest builder, checks
that it contains instructions, and copies it into the ARM evidence. The
engine remains unchanged. Both measured suite means remain below 50%.

The next arithmetic/COMIS/JCC trial supports the result as either comparison
operand, as already observed in independent corpus run 38493680532. Four
additional static condition variants preserve the distinct unordered cases
when operands are swapped: greater-or-unordered, ordered less-or-equal,
greater-or-equal-or-unordered, and ordered less-than. Other COMIS conditions
are symmetric. Exit flag stubs retain the original operand order. No
immediate value, program identity or guest loop is recognized. Differential
tests cover both orders for every scalar arithmetic operation and condition,
low/high registers, aliases, upper lanes, NaNs and live exit flags. Static
handler names now expand the operation macro before token pasting, making
perf names distinguish the operations. This passed all gates in 38499323627,
but did not improve standard floating-point timing.

A separate lowering-priority trial keeps an eligible arithmetic/COMIS/JCC
suffix available instead of consuming its arithmetic instruction in an earlier
overlapping pair. The preceding instruction still executes first in its own
handler with its original precise boundary. This changes selection between
existing general handlers, independent of program identity, values or loop
shape. Differential tests add full register copies immediately before the
arithmetic for both comparison orders and all conditions and operations.
This passed every gate in 38500223751. The standard float timing worsened.
Its standard perf report confirms the arithmetic/comparison handlers now
select, and their ARM assembly still loads and compares links to detect
adjacent successors at runtime.

The next adjacency-hint trial marks each arithmetic/comparison trace copy's
actual adjacent edge in private condition-byte bits. The handler can step
directly on that edge, while the final backedge and unproved edges use the
ordinary linked dispatch. It does not multiply the static condition tables.
The flags are attached after each copy's actual destination is known;
replication preserves them. Differential tests cover both comparison orders,
all conditions, NaNs, both vector banks, taken/fallthrough paths, final
backedges and live comparison flags on exit. This trial is not measured yet.

A separate scalar-code-generation trial uses statically compiled ARM scalar
FP instructions in the arithmetic/comparison family, inserting only the low
result into the unchanged destination. This prevents the vector arithmetic
and upper-lane restoration observed in the ARM evidence for 38500223751.
Portable hosts retain the original C operations. Existing differential cases
check arithmetic, upper lanes, aliases, both comparison orders, live flags
and trace exits. This trial is not measured yet. Its accompanying independent
corpus diagnostics count consecutive full-width vector loads, including the
same-base/index/scale subset, with exact raw counts. The engine does not yet
select a new consecutive-load family.

Independent corpus run
[38643061498](https://github.com/aghjkshdsj/MYIOSDECK/actions/runs/38643061498)
found 4114 consecutive full-vector load pairs with equal base/index/scale,
2976 of them in loop bodies. The next trial gives their first handler a
static direct tail branch to a separate, non-inlined second handler. Both
instructions keep their own uop, RIP and recoverable boundary; each handler
performs only one guest access. Displacements remain ordinary operands.
All vector classes, aliases and scales are supported. Existing load/arithmetic
pairs on the second load retain priority. After replica selection the second
uop is assigned the exact compiled continuation called by the first, using
immutable metadata. No recovery identity check or assembly audit is relaxed.
Differential cases cover every vector destination, aliases, signed indices,
displacements, equal address registers and flags. Fault sites 61-64 check
first-load faults and completed first results at second-load faults, including
an aliased destination; existing site 56 also exercises the second boundary.
This trial is not measured yet.

The next indexed-load trial extends the existing independent-corpus load +
arithmetic family to base + index * scale + displacement, retaining every
architectural scale and ADD/SUB/MUL in packed and scalar single/double forms.
Both address registers and pinned vector classes are statically specialized;
the high-bank class reads its ordinary register ordinal. Source/destination
aliases consume the loaded value. The one guest access precedes every
architectural effect, and the old destination remains recoverable until it
succeeds. All tables are included in replica and recovery indexing; the
replica map has additional capacity for them. Differential tests cover all
sixteen vector destinations, source aliases, all scales, positive/negative
displacements, negative indices, equal address operands, upper lanes and
flags. Fault sites 57-60 check exact pre-load state in both Windows allocation
modes. The recovery audit is unchanged. This passed all gates in 38641833700;
the standard SIMD timing improved, while both means remain below 50%.

The next diagnostic revision logs trace candidates, rejection reasons, body
sizes, copy counts and successor flag liveness under `GPTA_TRACE_LOG=1`.
Flag masks distinguish any flags, carry and non-carry flags. The workflow
enables this only in separate standard-guest diagnostic executions; measured
samples and held-out programs never enable it. This investigates the generic
DEC/branch handler still sampled in the standard integer profile. The optional
dispatch counter also counts the new direct continuation; it compiles away
in ordinary builds. No execution handler semantics change in this revision.

Windows mode defaults to the identity permutation. CI additionally enables
the allocation trial with `GPTA_XMAP_WINDOWS=1` and repeats the same native
exit, native API, fault-site and thread-stop checks. Host-state recovery
translates both pinned vectors and the unpinned bank into architectural XMM
slots. Map transitions execute outside the recoverable handler section;
suspend recovery must retry there. The new differential test keeps all
sixteen registers live across different hot sets, explicit-operand fallback
and FXSAVE/FXRSTOR. Full Wine suspend integration still follows the speed goal.

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
