# `walk_candidate_pairs` optimization log

This file is the durable handoff for the long-running solver optimization work.  Timings are wall-clock row times on the 20-logical-CPU i7-12700 host unless stated otherwise.  Preserve exact `cols` output when comparing implementations.

## Final accepted status (authoritative)

All five requested target rows beat vanilla with exact node/column output.  The
case-5 number predates the final P=7 gather and BMI2 expansion wins, so it is a
conservative measurement of the accepted implementation rather than a claimed
best time.

| case | rewrite target | accepted time | GNU maxRSS | vanilla target | speedup |
|---|---:|---:|---:|---:|---:|
| 1 | Row 67 | **0.513225 s** median | ~184,000 KiB | 1.423118082 s | **2.77x** |
| 2 | Row 44 (`w_pos 22[0]`) | **18.357835 s** | ~927,552 KiB | 23.752492488 s | **1.29x** |
| 3 | Row 44 | **15.990350 s** | 5,819,960 KiB | 30.331295883 s | **1.90x** |
| 4 | Row 30 | **6.379407 s** | 1,041,108 KiB | 20.702648451 s | **3.25x** |
| 5 | Row 41 | **24.195092 s** | 7,638,916 KiB | 32.368755528 s | **1.34x** |

Case 1 is 63.9% faster than vanilla and fell from the original rewrite's
20.581758 s without increasing its approximately 188 MiB RSS.  Case 2 is
22.7% faster than vanilla and its 927,552 KiB peak is about 1.08x the user's
839 MiB rewrite baseline.  Case 3 is 47.3% faster and its final peak is 1.007x
the 5,781,276 KiB packed/grouped baseline measured during this work.  Case 4
is 69.2% faster and the final raw-clause peak is 1.032x its earlier accepted
1,008,448 KiB measurement.  No original-rewrite RSS figures were supplied for
cases 3--5, so a direct 1.5x ratio cannot honestly be reported for those rows;
the accepted representation transitions were nevertheless memory-neutral or
memory-reducing, and never introduce a `workers * leaves` allocation.

Final validation passed:

- warning-clean `make -j20`;
- full and smoke search regressions;
- Release CMake configure/build and both registered CTest tests;
- non-native ASan/UBSan smoke testing;
- exhaustive/random differentials for persistent clauses, serial/parallel/
  grouped reification, shifted local-interest ranges, P=7 gathering, and BMI2
  plus non-BMI2 expansion;
- genuine v3 -> v4 checkpoint conversion, reload, and exact continuation.

The accepted implementation uses dynamic 16K sparse restart ranges, implicit
relation factorization, raw-label persistent BCAF clauses, constant-size local
interest intervals, globally ranged reification, parallel expansion, fused
clause writes, the P=7 packed gather, and BMI2 algebraic left-edge expansion.
Case-1 scaling is 10.8x from one to twenty workers, and 20 workers remain 6.5%
faster than 16.  Explicit affinity and active OpenMP waiting are intentionally
not enabled.

## Benchmark row mapping

The vanilla program reports the position before expansion; the rewrite prints the completed, post-expansion row.  Therefore the comparable rewrite rows/checkpoints are:

| case | vanilla target | rewrite target | checkpoint immediately before target |
|---|---:|---:|---:|
| 1 | `w_pos 66` | Row 67 | Row 66 |
| 2 | `w_pos 21[1]` | Row 44 (`w_pos 22[0]`) | Row 43 |
| 3 | `w_pos 43` / 42 | Row 44 / 43 | Row 43 / 42 |
| 4 | `w_pos 29` / 28 | Row 30 / 29 | Row 29 / 28 |
| 5 | `w_pos 40` (or 35..39) | Row 41 (or 36..40) | preceding row |

Case-1 checkpoint:

```sh
/tmp/rlife-bench/case1_implicit_66
```

Canonical case-1 timing command:

```sh
/usr/bin/time -v ./rlife_llsss llsss \
  --load /tmp/rlife-bench/case1_implicit_66 \
  --partials none --halts w_pos:67 --save none \
  --threads 20 --phase-timings
```

The exact Row-67 state is:

```text
[47811/656868\234249/2335423\590791/4706341\1106673/6788635\1505676/7569121\1868888/9455343\2288434/11099829\2548717/11929498\2626423/12086119\2467815/10686186\2099477/8791818\1654097/6618689\1235983/5520109\894950/4306709\546971/2509673\240059/692131\46852/46852\2367/2367\1]
```

## Historical indexed baseline (superseded by the final status above)

- `PairGate` has sparse restart points every 16,384 parent-gate entries.  Each restart stores its raw three-bit history path.
- OpenMP is isolated in `indexed_executor.cpp`; the main solver translation unit remains compiled without `-fopenmp`.
- Indexed ranges use dynamic scheduling, so work stealing responds to skew rather than assuming uniform tree sizes.
- Packed output tags use monotone relaxed atomic ORs.  Pair-gate output remains ordered through one segment per indexed range.
- The uniform expanded tail processes all eight terminal edges in a batch using four-bit tag loads/projections.
- The common one-phase geometry path uses a fixed five-history lookup.  The two-phase `c6d` geometry has a guarded fixed 5/4/3-table kernel with nine shared history loads.
- BCAF clean planes were proven redundant.  With `L/R` normal reachability and `P/S` prefix/suffix witnesses:

  ```text
  K(v)   = L(v) & R(v) & (P(v) | S(v))
  F(u,v) = E(u,v) & L(u) & R(v) & (P(u) | S(v))
  ```

  This removed two packed tag planes and one cleanup recurrence.
- Final gate emission is fused into the reverse partial/completion suffix walk, reducing the usual path to three paired-tree traversals.
- Slice reification is parallel across independent slice tries when no concrete partial/completion path needs reconstruction.

Case-1 progression:

| implementation | Row 67 | max RSS | note |
|---|---:|---:|---|
| original rewrite | 20.65 s | ~188 MiB | serial generic geometry/four sweeps |
| serial optimized kernels | 9.07 s | ~160 MiB | direct BCAF live formula and three walks |
| indexed relation walks, before parallel reify | 2.63–2.71 s | ~160 MiB | reify was ~0.94 s |
| current, 20 workers | **1.84–1.89 s** | **161 MiB** | exact output |

Representative current phase breakdown at 1.841 s:

```text
right reach + BCAF suffix       0.680 s
left reach + BCAF prefix        0.642 s
BCAF gate/completion suffix     0.317 s
parallel slice reification      0.142 s
expand + boundary/support       0.059 s
```

The case-1 vanilla target is 1.423118082 s and about 1001 MiB.  The rewrite remains far below the allowed RSS ceiling; the task is not yet complete.

The guarded `c6d` fast path improved a truncated case-2 Row 36 from 2.164221 s to 1.513821 s (1.43x), exact output and unchanged RSS.

The first from-scratch case-2 checkpoint run exposed an index-propagation bug: pre-BCAF rows emitted unindexed gates, so all later rows silently used the serial fallback.  Non-BCAF gate emission now retains sparse restart paths, and checkpoint format v2 serializes them; v1 remains loadable.  Loading the v1 Row-43 checkpoint rebuilt indexes outside the row timer and measured the real parallel Row 44:

```text
Row 44: 31.374360 s, 1323 MiB RSS
right sweep 8.129 s, left sweep 8.298 s, gate 11.850 s,
reify 2.836 s, other 0.341 s
```

Exact Row-44 state:

```text
[42441/18902612\18902612/861229636\108396559/1892199819\9593638/9593638\16325/16325\1]
```

The vanilla target is 23.752492488 s.  Witness and completion planes are being changed to leaf-local allocation; on this row, avoiding bits for historical internal trie nodes is expected to remove roughly 98 MiB and bring RSS below the conservative `1.5 * 839 MiB` ceiling.

## Critique of historical `multithreading` branch

Commit `1a426c9` builds a full traversal plan, assumes coarse partitions have similar cost, and allocates dense result planes proportional to `planes * threads * leaf_words`.  That combination explains both reported failures:

- subtree sizes and accepted work vary greatly across searches, so static/coarse partitions leave workers idle;
- raw candidate counts are a poor proxy for internal paired-trie work;
- case 2 would require roughly 495 MiB just for dense per-thread planes;
- planning, team setup, and reduction are paid in addition to the actual walks.

The current sparse restart/dynamic-task design retains the useful concept of restartable traversal without those assumptions or the `threads * leaves` memory term.

## Rejected or neutral experiments

- Cache every terminal active mask (~90 MiB): 2.736 s and 278 MiB RSS; no gain.  Geometry lookup is no longer dominant.
- Cache decoded child navigation (~78 MiB): no gain and ~266 MiB RSS.
- Per-worker direct child cache: ~11% slower.
- Unsafe non-atomic tag OR: no measurable gain; atomics are not the limiting cost.
- Atomic load-before-OR: neutral on case 1 (currently retained because it can avoid redundant writes on other cases).
- Pair-gate index quantum 16K -> 4K: no benefit.
- OpenMP affinity/topology variants: no benefit on this host.
- Raw active-mask preorder plan (186.8 MiB): the ideal replay decoder saved only ~0.185 s over three replays but cost ~0.232 s to record.  Entropy coding cannot improve that time bound.
- Whole-row endpoint plan: best measured coding was at least 129.6 MiB before terminal masks/summaries.
- One-adjacency eight-byte terminal records, capped at 100 MiB: exact and 211 MiB RSS, but Row 67 slowed to 2.78 s due write/read bandwidth and cache pollution.  Removed.

Case-1 paired-walk counts per pass are 92,239,356 interior states, 94,583,633 terminal batches, and 338,366,118 active terminal edges.  This makes topology/rank traversal—not geometry acceptance—the remaining relation-walk cost.

## Validation

Run after material changes:

```sh
bash tests/search_regression.sh ./rlife_llsss smoke
```

The smoke suite and multiple exact case-1 checkpoint comparisons have passed throughout.  Run the full regression suite before final handoff.

## In-progress: implicit induced relation

The next structural experiment removes the dense pair-gate payload from native
searches.  A recoverable copy of the tested indexed baseline is in
`/tmp/rlife-implicit-baseline-20260824`.

The representation is justified by the exact support recurrences.  With raw
accepted relation `E`, left/right reachability `L/R`, and BCAF prefix/suffix
witnesses `P/S`:

```text
normal K(v) = L(v) & R(v)
normal F    = E restricted to retained endpoints

BCAF K(v) = L(v) & R(v) & (P(v) | S(v))
BCAF F(u,v) = E(u,v) & (P(u) | S(v)), restricted to retained endpoints
```

Every `F` edge itself proves that both endpoints satisfy `K`.  Consequently,
ordinary support adds no binary correlation, while each BCAF row adds only the
unary historical clause `P(left) || S(right)`.  Store two persistent bits on
the retained trie nodes at that depth and test the clause while descending a
future paired DFS.  The full-history geometry is already recomputed there.

This identity passed exhaustive enumeration of all 262,144 arbitrary
three-layer/two-node relation, boundary, and local-interest configurations.
An exact two-plane completion-prefix recurrence, fused into the left sweep,
was separately compared with the old completion-suffix recurrence on
2,985,984 configurations with no disagreement.  A tempting four-state
raw-relation automaton is *not* exact because the projected BCAF graph can
splice prefix- and suffix-witnessed paths.

Expected native row structure:

1. right sweep computes `R/S` through geometry plus historical clauses;
2. left sweep computes `L/P`, propagates completion-prefix tags, and emits
   only sparse accepted-path restart indexes for `F`;
3. write the current `P/S` clause bits and reify retained endpoints.

This deletes the third dense gate-emission traversal.  On case 2 it also
replaces roughly 330 MiB of gate payload by about 79 MiB of persistent clause
bits and rejects failed historical clauses at their original depth.  v1/v2
checkpoints containing false dense gates must remain on the legacy engine;
their historical factorization cannot be recovered exactly.  New checkpoints
will use a versioned implicit format.

Separately, a level-wise stable parallel reifier was validated in
`/tmp/rlife-par-reify-clean.c8Q8kq`: eight randomized irregular tries matched
serial output byte-for-byte, smoke passed, and case-2 Row 44 reification fell
from 3.0876 s to 0.34386 s with only 404 KiB additional maxRSS.  It will be
merged after the implicit clause payload compaction is validated.

### First native implicit results

The implicit engine, v3 checkpoints, clause-aware serial/batched/indexed
walkers, payload-aware parallel reifier, and exact completion-prefix DP are now
implemented.  Old v1/v2 checkpoints select the legacy engine; native searches
select the implicit engine.  The full regression suite passes, as do the
independent exhaustive/random relation and payload-compaction tests described
above.

Fresh native checkpoints:

```text
/tmp/rlife-bench/case1_implicit_66
/tmp/rlife-bench/case2_implicit_43
```

The fresh case-1 run reached Row 66 in about 24 s end-to-end and exactly
matched the legacy Row-66 state.  Initial implicit Row 67 was 2.121 s.  A
factored `2 * 256` byte clause table, unchecked proven child-block payload
access, and bulk all-one relation emission reduced it to:

```text
Row 67  1.867796 s, 174 MiB RSS, exact
right sweep                         0.687086 s
left sweep + prefix/index           0.955038 s
reification                         0.156598 s
```

The vanilla target remains 1.423118082 s, so case 1 is not solved.

Fresh case 2 reproduced every requested row state.  Before hot-loop cleanup,
Row 44 was 28.302 s at 785 MiB internal RSS.  Current result:

```text
Row 44  24.600809 s, 779 MiB internal RSS / 797708 KiB GNU maxRSS, exact
right sweep                         8.801800 s
left sweep + prefix/index          15.120684 s
reification                         0.374521 s
```

This is 0.8483 s (3.57%) behind the 23.752492488 s vanilla target while using
far less than vanilla's memory and staying below `1.5 * 839 MiB`.  Disabling
completion detection measured 24.026790 s, with the left phase 1.12 s lower
but enough run-to-run variation in the right phase that this is only an upper
bound on the optimization opportunity.

Bulk implicit emission is exact because an eight-edge terminal batch crosses
at most one 16K restart quantum.  On case 1 it removes 105,801,711 per-edge
`push_back`/modulo iterations; on case 2 it avoids billions.  Sparse path
packing and final segment merge themselves cost only about 10 ms on case 1.

Malformed checkpoint audit also added overflow-safe packed/tree sizes,
strictly increasing sparse restart validation, and a lower bound on the first
BCAF clause depth.  Genuine v1, v2, explicit-v3, and native implicit-v3
round-trips/continuations remain exact.

### Compact completion-path state

Completion-prefix propagation no longer carries the full
`PairPathSummary` (two `size_t` depths) through the dominant indexed left
sweep.  For this use, the exact last-nonzero depth factors into three monotone
states: valid/uninteresting, valid/interesting, and invalid.  The indexed
walker stores the two-bit state in existing padding in its 24-byte compact
restart frame, so this adds no frame or RSS cost; legacy summary-tracking
walkers remain unchanged.

The full regression suite passes.  A separate serial-summary versus
four-thread compact-state completion search produced byte-identical RLE, and
a debug build recomputing the old classification from the full history at
every callback found no divergence.  Case 1 reduced the left phase by about
1.7%, with whole-row time inside noise.  On an adjacent case-2 A/B under the
same thermal state, the old/new left phases were 15.197396/14.914291 s (about
0.28 s saved); unrelated right-sweep variance masked that improvement in the
whole-row totals.  Exact Row-44 output and roughly 797 MiB maxRSS were
unchanged.

The out-of-line geometry dispatcher was also split into narrow one-phase,
two-phase `c6d`, and generic leaves.  This removes the unified kernel's stack
frame from the dominant one-phase case and adds only 296 bytes of text.  Three
alternating case-1 pairs reduced combined relation phases from 1.61477 to
1.60301 s (0.73%); output/RSS and the full regression suite are unchanged.
Whole-walker specialization and forced call-site inlining grew text by about
20/36 KiB respectively and were timing-neutral, so they were rejected.

### Current case-1 scaling

A single foreground worker-count sweep after the compact completion and split
geometry changes measured:

| workers | Row 67 (s) | right (s) | left (s) | reify (s) |
|---:|---:|---:|---:|---:|
| 1 | 8.086 | 3.119 | 3.828 | 1.072 |
| 2 | 4.278 | 1.614 | 2.055 | 0.541 |
| 4 | 2.769 | 1.043 | 1.382 | 0.276 |
| 6 | 2.266 | 0.864 | 1.131 | 0.201 |
| 8 | 2.152 | 0.808 | 1.092 | 0.185 |
| 10 | 2.037 | 0.780 | 1.037 | 0.151 |
| 12 | 1.948 | 0.722 | 0.997 | 0.161 |
| 16 | 1.876 | 0.689 | 0.964 | 0.155 |
| 20 | 1.854 | 0.676 | 0.954 | 0.157 |

All outputs were exact and maxRSS stayed 174--181 MiB.  The relation kernel's
one-to-twenty speedup is only about 4.26x, and GNU `time` reported 11.08 user
seconds over 1.97 wall seconds for the 20-worker process.  The extra logical
workers still help slightly, but most of the remaining gap is parallel
utilization/granularity rather than a bad choice of 12/16/20 as the requested
worker count.  Per-task/per-adjacency instrumentation is in progress.

### Shared local-interest seeding and first target wins

Instrumentation then showed that dynamic scheduling was *not* the dominant
case-1 loss.  Across 14,289 indexed tasks in each direction, `dynamic,1`
kept relation workers 93.1/93.7% busy; `static,1` made Row 67 7.3% slower.
The indexed callbacks occupied only 0.514 s of the 1.614 s combined relation
phases.  The major serial remainder was `seed_suffix`/`seed_prefix`: each
walked every trie node to recompute the identical local predicate
`first_nonzero < long_window`.

The native engine now builds that leaf-local predicate once per slice, then
seeds both witness directions with streaming four-bit
`normal & local_interest` intersections.  This replaces two full recursive
trie scans by one and adds only one transient bit per leaf.  The same local
cleanup also removes freshly-zeroed destination clears, skips dead terminal
batches, and replaces completion's per-edge loop by exact `0x03` (zero left
label) / `0xfc` (nonzero left label) state masks.  The full regression suite
and all target-row columns remain exact.

Case 1 now beats vanilla decisively:

```text
rewrite Row 67: 1.231542 / 1.233097 s, GNU maxRSS 188340/188792 KiB
vanilla target: 1.423118082 s, reported about 1000.59 MB
```

Representative new phases are 0.313 s local-interest construction, 0.236 s
right, 0.454 s left, and 0.159 s reification.  This is about 13.4% faster than
vanilla while using essentially the original rewrite's RSS, far below the
1.5x allowance.

Case 2 also crosses its target on two consecutive exact runs:

```text
rewrite Row 44: 23.471596 / 23.224935 s, GNU maxRSS 838852/838908 KiB
vanilla target: 23.752492488 s
```

The second run split into 1.210 s local-interest construction, 8.547 s right,
12.795 s left, 0.361 s reification, and 0.312 s other row work.  Current RSS
is essentially the user's original 839 MiB rewrite measurement and well below
the 1258.5 MiB ceiling.

The reification part of the scheduler profile still has one genuine
granularity defect: 19 whole-slice tasks use only 47.4% of the team and take
139 ms on case 1, versus a 66 ms busy-time lower bound.  A global
`(slice, range)` worklist could recover about 70 ms, but it is no longer
required to beat the first two targets.

A direct 32-bit rank-before-word directory was rejected.  It was neutral on
case 1 (+14 MiB RSS) and made case 2 14.2% slower (25.184 vs 22.052 s in that
agent's adjacent pair) by doubling the random-access rank footprint.

The one required local-interest scan is now dispatched across independent
slices as well.  On case 1 its time fell from about 0.32 s to 0.051 s and Row
67 reached **0.966268 s** (188512 KiB maxRSS), 32.1% faster than vanilla.  On
case 2 the mask phase was 0.912 s and exact Row 44 reached **22.740487 s** at
839136 KiB maxRSS, 4.26% faster than vanilla.  Slice-level mask construction
is still skew-limited on width seven, but is strictly outside the relation
recurrences and safe to parallelize.

Native implicit walkers are now compile-time specialized for the invariant
that their sparse gates have no dense payload and every enumerated parent is
ancestry-allowed.  GCC had not unswitched the runtime test: it still executed
the gate-mode branch and bit extraction in every terminal batch.  The
specialized hot completion walker shrank by 193 bytes; alternating case-1
medians were 0.980591 -> 0.938785 s (4.26%), with right/left phases improving
5.45/2.14%, exact output, unchanged RSS, and a passing full regression.

Independent slice-tail expansion now uses the same dynamic executor.  Case-1
expansion fell from about 46 to 8 ms and Row 67 measured 0.941687 s.  Its
overlapped allocation high-water increased GNU maxRSS to about 210 MiB, still
only 1.12x the original rewrite and below the 282 MiB limit.

### First case-3 measurement

The native Row-43 checkpoint is `/tmp/rlife-bench/case3_implicit_43`; fresh
generation took 1m37s and peaked at 5573332 KiB RSS.  Before the global
reification fix, exact Row 44 measured:

```text
Row 44 37.106078 s, GNU maxRSS 5781276 KiB
local-interest 2.526, right 7.554, left 15.738, reify 8.990,
expand 1.568, pre-sweep/support 0.730 s
```

Vanilla's target is 30.331295883 s.  The 8.99-second reifier is the clear
defect: fourteen large tries are dispatched as fourteen indivisible serial
tasks, so the largest trie is the critical path.  A grouped cross-tree range
reifier is under exact differential testing.  Parallel tail expansion should
also remove most of the separately measured 1.57-second expansion phase.

### Grouped cross-tree reification and fused clause writes

The indivisible-tree scheduler was replaced by one global worklist for each
reification phase.  Leaf keep, reverse ancestry closure, live counting,
stable emission, and final rank rebuild are all exposed as range/tree tasks
from every slice to one worker team.  Reverse levels remain barriers, output
offsets are prefix sums in original node order, and the existing atomic
boundary-word OR preserves exact BFS and persistent-payload ordering.  Rank
directories are released after child starts are resolved and before output
tries allocate, keeping the transient memory high-water unchanged.

Validation includes the full search regression and serial-vs-grouped
differentials on eight fixed payload cases plus sixteen randomized unequal-
depth, irregular-density multi-tree cases.  Exact target-row results were:

```text
case 1 Row 67: 0.674516 s, reify 0.068375 s, maxRSS 185844 KiB
case 3 Row 44: 23.223293 s, reify 2.485356 s, maxRSS 5781548 KiB
```

For case 3 this reduces reification from 8.990 s and total row time from
37.106 s, beating vanilla's 30.331295883 s by 23.4% with only 272 KiB over
the pre-change maxRSS measurement.

The current-row BCAF prefix/suffix writes were then fused into the already
required global leaf-keep ranges.  Range boundaries are packed-word aligned,
so even a dominant slice is safely divided among workers; this removes a
separate team launch, full leaf scan, and duplicate witness reads.  The full
regression still passes.  An adjacent five-run case-1 comparison improved the
median from 0.684566 to 0.663710 s (3.05%) at unchanged RSS.  The much larger
exact case-3 Row 44 reached:

```text
22.831884 s, maxRSS 5781492 KiB
mask 2.712, right 6.877, left 9.394, reify 2.813,
expand 0.391, support/accounting 0.645 s
```

The write moved from the reported left phase into the reification phase, so
only total-row comparisons represent the fused work.  The final case-3 result
is 24.7% faster than vanilla with effectively identical RSS to the original
grouped-run baseline.

### First case-4 measurement

The native checkpoint `/tmp/rlife-bench/case4_implicit_29` was generated from
scratch in 4.59 s wall time at 419992 KiB maxRSS.  Row 29 itself took 2.901 s.
Exact Row 30 from that checkpoint measured:

```text
Row 30, 432382340 nodes, 8.015294 s, GNU maxRSS 1008448 KiB
mask 0.290, right 3.090, left 4.079, reify 0.386,
expand 0.075, support/accounting 0.095 s
cols: [1/1894\1894/106064\106064/4639411\2289490/51189626\19034095/243940604\47652980/523653085\67322599/661289702\67322599/523653085\47652980/243940604\19034095/51189626\2289490/4639411\106064/106064\1894/1894\1]
```

This beats vanilla's 20.702648451 s target by 61.3%.  The alternative rewrite
Row 29 target also trivially beats vanilla's 9.257578539 s at 2.901 s.

The final accepted raw-clause/range/P7/PDEP binary improved the same exact
Row 30 to:

```text
Row 30, 432382340 nodes, 6.379407 s, GNU maxRSS 1041108 KiB
ranges 0.000020, left 3.358247, right 2.459628, reify 0.384304,
expand 0.082701, support/accounting 0.094504 s
cols: [1/1894\1894/106064\106064/4639411\2289490/51189626\19034095/243940604\47652980/523653085\67322599/661289702\67322599/523653085\47652980/243940604\19034095/51189626\2289490/4639411\106064/106064\1894/1894\1]
```

This is 69.2% faster than vanilla (3.25x throughput) and adds only 3.2% to
the first accepted case-4 peak above.

### Constant-size local-interest ranges

A path is locally uninteresting exactly when its first `long_window` labels
are all zero.  There is at most one such prefix node, and breadth-first child
ordering makes all of its descendants a single contiguous interval at every
later level.  The leaf-sized interest bit plane and its full-tree DFS were
therefore replaced by two node IDs per slice.  Witness seeding OR-copies the
two complementary leaf intervals from the normal plane in shifted 64-bit
chunks, replacing the previous four-bit `normal & mask` loop as well.

Correctness was checked on 512 randomized irregular tries for every window
through `depth + 2`, against real lineage membership and random prefilled
witness planes.  A further 8192 randomized full-tree-to-leaf-local shifted
range copies, with arbitrary endpoints/alignments, matched a per-bit oracle.
The full search regression passes.

Adjacent exact measurements:

```text
case 1 control 0.667775 / 0.667479 s, maxRSS 184404/184732 KiB
case 1 range   0.578313 / 0.580454 s, maxRSS 175800/174932 KiB

case 3 control 23.032576 s, mask 2.624150 s, maxRSS 5781784 KiB
case 3 range   19.445980 s, range 0.000044 s, maxRSS 5474224 KiB
```

Case 1 improves about 13.2%.  Case 3 improves 15.6%: eliminating the scan
saves 2.624 s, wider witness seeding saves another roughly 0.97 s across the
relation phases, and the removed transient leaf plane lowers maxRSS by about
300 MiB.  This optimization improves both axes of the memory-time frontier.

### Raw-label persistent BCAF clauses (checkpoint v4)

The two node-indexed packed P/S planes were replaced by one byte per queried
internal parent.  Its low/high nibbles store P/S at raw child-label positions.
Historical pair filtering is now two byte loads plus the existing left/right
edge expansion, removing two random packed-word gathers and compact-ordinal
mapping-table lookups from every historical DFS state.  Parents above the
first clause depth and current leaves have no bytes.  Expansion appends one
zero byte per former leaf; serial, standalone-parallel, and grouped reifiers
stably compact and raw-child-mask the payload.

Checkpoint version 4 writes the byte vectors.  Versions 1/2 retain the legacy
engine, and a v3 load validates and converts its two packed planes through the
actual child masks.  A genuine v3 case-1 checkpoint was saved as v4, reloaded,
and advanced one row with exactly identical nodes/columns.  ASan/UBSan
exhaustive/random payload tests cover all child masks/tag patterns, expansion,
serial/parallel/grouped compaction, unequal trees, v3 conversion, v4 restore,
and malformed absent-child bits.  Full and smoke regressions pass.

Combined with constant-size local-interest ranges:

```text
case 1 median: 0.587138 -> 0.539650 s, maxRSS about 184 MiB
case 2 Row 44: 18.285964 s, maxRSS 927552 KiB
case 3 Row 44: 16.227112 s, maxRSS 5819752 KiB
```

Case 1 gains another 8.1%.  Case 2 is 23.0% faster than vanilla and uses
1.11x the original rewrite's 839 MiB RSS.  Case 3 is 46.5% faster than
vanilla; range-plane savings nearly pay for raw bytes, leaving maxRSS only
0.7% above the old packed-payload grouped baseline and far below 1.5x.

### First case-5 measurement

The v3 checkpoint
`/home/dandan/Documents/rlife/build/bench/case5_implicit_40` was generated
from scratch in 76.14 s wall at 6567760 KiB maxRSS.  Row 40 itself was
18.050311 s and the checkpoint is 1.3 GiB.  Loading it with the v4 binary
exercised on-load payload conversion on 1.75 billion nodes.  Exact Row 41:

```text
Row 41, 1978006482 nodes, 24.195092 s, GNU maxRSS 7638916 KiB
ranges 0.000041, right 7.655, left 11.596, reify 3.539,
expand 0.638, support/accounting 0.768 s
cols: [748154/8422317\5392554/40450317\22232410/131449300\66275307/284054025\132271559/470115048\202137578/615663199\223828745/584183610\160912614/374570789\64047486/138756713\17530060/35912786\4274055/7203153\468264/468264\23489/23489\1]
```

This beats vanilla's 32.368755528 s target by 25.3%.  Earlier rewrite rows
36--40 also beat their corresponding vanilla alternatives (for example the
fresh Row 40 is 18.05 s versus vanilla Row 39 at 29.19 s).

### Rejected: frame-local child-block runs

A bounded experiment cached topology/rank results for consecutive compact
children in each DFS frame.  It was correct under exhaustive/random
`child_block_run` differentials, debug scalar cache checks, and the full
regression; it added only about 14 KiB of worker state.  Nevertheless, both
eager and lazy (only when at least two later siblings remain) forms were
strongly slower.  The best lazy results were:

```text
case 1 control 0.554962 s -> run 0.671654 s (+21.0%)
case 3 control 16.289145 s -> run 18.972016 s (+16.5%)
case 3 right 5.088 -> 6.305 s; left 7.433 -> 8.854 s
```

RSS was neutral.  Constructing/gathering runs and expanding the hot frame
cost more cache/store traffic than the rank-directory reads saved, even on
the large skewed case.  The experiment remains isolated and is not merged.

### Rejected: static left-expansion lookup table

GCC materialized the function-local automatic `constexpr` 16-byte
left-expansion table on the stack inside the implicit walker.  Giving it
static storage removed both vector load/store copies, shrank a representative
walker from 2595 to 2444 bytes, and passed exhaustive equivalence/full
regression.  Timing nevertheless regressed:

```text
case 1: +2.31% row, +2.78% combined relation median
case 3: 16.703373 -> 16.867243 s (+0.98%)
```

RSS was identical.  The likely code/data placement or indexed-load cost
outweighs the cleaner instruction sequence; the change is not merged.

### Rejected: indexed implicit runtime hot view

An immutable per-task view hoisted worker-history and raw-clause bases,
origins, and start depth; the indexed loop structurally proves the generic
upper-depth guard redundant.  Random specialized-vs-generic ASan/UBSan
differentials and the full regression passed, and assembly confirmed the
intended metadata/guard removal.  GCC, however, versioned the nullable-clause
loop and grew a representative walker from 2595 to 3831 bytes (+47.6%).

```text
case 1: +1.80% row, +2.31% combined relation
case 3: 16.160163 -> 16.337112 s (+1.10%)
```

RSS was unchanged.  This runtime-view form is not merged; a final bounded
compile-time clause-present/absent dispatch is being tested to avoid loop
versioning.

The compile-time refinement removed loop versioning (BCAF kernel 2699 bytes,
no-clause kernel 2134 bytes versus 2595-byte control) and retained exactness,
but case 1 still regressed 1.90% total / 2.02% relation with unchanged RSS.
It was rejected without spending a case-3 run.

### P=7 packed history gather

For canonical one-projection orthogonal P=7 geometries with K=1..3 on
little-endian BMI2 hosts, the five history triples are now assembled from one
byte at `row-14` plus one memcpy-safe unaligned qword beginning at
`row-(7+K)`.  PEXT gathers the qword's `{b,c,e,d}` bytes and a fixed bit
permutation forms the exact existing 15-bit lookup key.  The qword ends at
`row-K`, so the checked interior entry point's `row>=14` invariant proves all
bounds.  Exact period/displacement/offset guards retain the scalar path for
all other geometries and ISAs.

The gather kernel is 79 bytes versus the 93-byte scalar and replaces five
dynamic byte loads/subtractions with one qword, one byte, and PEXT.  A 240k-
case randomized differential covered K=1/2/3, four rules, minimum buffers,
and negative geometry guards; the full regression passes.  Adjacent results:

```text
case 1 median row -1.43%, combined relation -2.25%
case 3 two-pair mean row -1.78%, combined relation -2.23%
```

Nodes/columns were exact and RSS neutral.  A separate full-history terminal
entry point was rejected: despite halving the dispatcher from 69 to 37 bytes,
it regressed case 1 by 4.21% total / 4.64% relation and was not merged.

### Final parallel scaling and rejected runtime tuning

On the final raw-clause/range/P7 binary, exact case-1 scaling is:

| workers | Row 67 (s) | right (s) | left (s) | reify (s) |
|---:|---:|---:|---:|---:|
| 1 | 5.533 | 1.852 | 2.548 | 1.057 |
| 2 | 2.640 | 1.019 | 1.198 | 0.375 |
| 4 | 1.308 | 0.502 | 0.575 | 0.194 |
| 6 | 0.903 | 0.343 | 0.392 | 0.134 |
| 8 | 0.755 | 0.300 | 0.310 | 0.107 |
| 10 | 0.658 | 0.245 | 0.287 | 0.095 |
| 12 | 0.623 | 0.229 | 0.273 | 0.088 |
| 16 | 0.548 | 0.196 | 0.247 | 0.074 |
| 20 | 0.513 | 0.187 | 0.223 | 0.072 |

This is a 10.8x one-to-twenty row speedup, versus 4.26x relation scaling in
the earlier implementation.  Twenty workers remain 6.5% faster than sixteen;
maxRSS is flat around 180 MiB.  Explicit OpenMP thread/core binding was 8--30%
slower on the hybrid 8P+4E host, and `OMP_WAIT_POLICY=active` was much slower;
the unbound default policy is retained.  Hardware perf counters were not
available (`perf_event_paranoid=4`), so no privileged system change was made.

### Rejected: coarser restart index quantum

Checkpoint indexes persist their explicit restart paths, so the experiment
coarsened a genuine 16K checkpoint on load rather than merely recompiling a
constant.  Exact case-1 means were 0.541938 s (16K), 0.533565 s (32K), and
0.547173 s (64K).  The 32K relation signal was only -0.74%, within control
variation; 64K relations regressed 1.96%.  RSS was neutral, and synthetic
path/cross-quantum checkpoint regressions passed.  The well-validated 16K
quantum remains.

### BMI2 algebraic left-edge expansion

On BMI2 hosts, duplicating a four-bit leaf mask into the eight synchronized
left-edge positions now uses `PDEP(mask, 0x55)` followed by `spread |
(spread << 1)`.  Non-BMI2 builds retain the original automatic constexpr
table, whose static-storage variant was slower.  This removes both the stack
table materialization and indexed load; the hot BCAF walker shrinks from 2595
to 2485 bytes.

Exhaustive intrinsic and `-mno-bmi2` fallback tests, ASan/UBSan, and the full
regression pass.  Exact adjacent measurements:

```text
case 1: -3.83% row, -4.65% combined relation, RSS unchanged
case 3: 16.387296 -> 15.990350 s (-2.42%)
        combined relation 12.647408 -> 12.206668 s (-3.49%)
        maxRSS +260 KiB (noise)
```

## Late-row continuation: quadratic merge and partial reporting

The supplied `save_217` and `save_239` checkpoints are for the `c5d-f2b`,
`B35678/S4678`, width-12 BCAF search.  They are flattened depths 217/239,
corresponding to `w_pos 108[1]` / `119[1]`.

### One-core valleys: repeated restart-index validation

Instrumentation separated each ordered output merge from its preceding
indexed walk.  The walks themselves used about 18--19 effective cores, but
`PairGate::append()` called `index_ready()` on the entire accumulated output
after every restart-range segment.  The destination therefore underwent an
O(number_of_ranges^2) monotonicity scan on one core.  At Row 218, left walks
summed about 11.16 s while this validation merge alone cost about 10.35 s;
central relations each spent roughly 3.97 s at exactly one core.

Every independently produced segment is still fully validated.  The
accumulated destination now gets an O(1) structural check on each append,
using the inductive invariant that a validated segment begins at zero and is
offset beyond the prior valid destination.  The existing one full
`index_ready(height_)` check remains after the complete ordered merge.

Exact adjacent `save_217 -> Row 218` results:

```text
before: 46.793778 s solver row, 861% process CPU, 5033004 KiB maxRSS
after:  23.685073 s solver row, 1533% process CPU, 5031576 KiB maxRSS
left phase: 33.118305 -> 11.222047 s
```

A post-cleanup confirmation measured 23.508662 s with identical columns.

`save_239` has 988,965 restart tasks; the sum of squared per-relation task
counts is 18.954x Row 217's.  The old validation loop therefore predicted
roughly 196--215 seconds of serial work, explaining the observed/anticipated
seven-minute scale.  The fixed exact continuation instead measured:

```text
Row 240, 6760494684 nodes, 106.821271 s solver row
right 42.669881, left 48.851045, grouped reify 10.842873 s
1518% process CPU, 24719012 KiB maxRSS
2:04.48 process elapsed including loading the 7.0 GiB checkpoint
```

No coarser restart quantum is needed for this failure; dynamic 16K tasks were
well utilized once the quadratic serial epilogue was removed.

### Partials without serial reconstruction

The current implicit engine did not actually disable its parallel pruning
when `--partials` was set.  It finished/printed the timed row first, then
`reconstruct_interesting()` allocated two full-node tag planes and made two
serial full relation passes: one backward suffix propagation and one forward
path selection.  On a row that was both scheduled and halted, it repeated the
entire reconstruction and emitted the same board as both `partial` and
`halt`.  The legacy v1/v2 engine additionally disables grouped reification
while caching a partial; that compatibility path is unchanged.

Native v4 rows now capture the deterministic witness path inside the already
required indexed left sweep.  Each restart range records only its first
suitable successor, and reducing candidates in restart order preserves the
old serial DFS choice byte-for-byte.  The path is materialized before stable
reification changes node IDs.  This adds no relation pass; rows with partials
disabled compile to a capture-free walker specialization.  A scheduled halt
emits only its already-produced partial.

Exact `save_217 -> Row 218` partial results:

```text
old: printed sec 51.472708 before reporting; first reconstruction 179.570503 s
     then reconstructed/emitted it again; 6:54.16 elapsed, 189% process CPU
new: sec 23.868316 includes reporting; reconstruction/output 0.000109 s
     27.48 s elapsed including load, 1541% process CPU, one emitted board
```

An explicit partial override on an already-halted loaded checkpoint is now a
read-only inspection operation.  For example:

```sh
./rlife_llsss llsss --load save_217 --save none --halts w_pos:108 \
  --partials every:1 --partial-output partial.rle --threads 20
```

Ordinary loads without an explicit partial override still suppress duplicate
inherited output.  The post-hoc fallback now uses one leaf-local suffix plane
and indexed current-edge walks.  `save_217` inspection took 14.930155 s for
reconstruction (18.53 s including load), used 1523% process CPU, and peaked
at 1663112 KiB RSS instead of performing the old multi-minute serial scans.

The per-row `sec` field is emitted after scheduled partial reporting and end
propagation, so it measures the complete search/reporting row; checkpoint
serialization remains intentionally outside it.  Phase timing state is now
per solver and flushes the final phase.  `every:N` continues to count
flattened depths, so on a two-subtile geometry `every:10` means every five
logical `w_pos` positions.  Completion detection/output remains independent
of partial mode and is still emitted with `--partials none`.

Validation for this continuation includes byte-identical serial/indexed
partials, exact deep-row columns, the full and smoke regressions, Release
CMake/CTest, and non-native ASan/UBSan smoke tests.

## Byte-budgeted external sweep residency

Expanded slice trees and their completed reverse-sweep suffix tags can now be
owned by a dedicated residency manager instead of remaining live for the
whole row. The manager preallocates a native temporary record file, uses
aligned `O_DIRECT` positional I/O when available, verifies word-oriented
checksums, retains a byte-budgeted left prefix at the turnaround, and
prefetches record `i+2` while forward relation `i` runs. The search loop only
submits complete records and receives loaded leases, so its inner relation
walks have no loaded-state branches.

Rolling each slice through reification initially exposed a separate executor
problem: opening thousands of short OpenMP regions for dependent trie levels
made `save_217` exceed three minutes even with disk disabled. A
process-lifetime indexed worker team restored the intended granularity. Exact
`save_217 -> Row 218` measurements with identical columns were:

```text
no spill: 25.440191 s row, 4609340 KiB maxRSS
4 GiB policy: 24.921469 s row, 4162204 KiB maxRSS
policy I/O: 2371 MiB written/read, 0.000 s read/write wait
```

The final bounded `save_239 -> Row 240` comparison used the default 4 GiB
activation and resident budgets on the SATA-backed `scratch/` mount:

```text
baseline: 106.821271 s row, 24719012 KiB maxRSS
spill:    136.478927 s row, 16805628 KiB maxRSS
phases:   right 51.590405 s, left/reification 84.884202 s
I/O:      17 GiB written, 15 GiB read, direct=yes
wait:     1.123 s producer, 20.179 s demand read
service:  15.146 s write, 19.057 s read
records:  9005 MiB manager-resident peak
```

This is a 32.0% max-RSS reduction for a 27.8% row-time cost. The result has
the exact expected 6,760,494,684 nodes and per-slice columns. The 4 GiB budget
is deliberately soft: the central adjacent pair required by one relation is
about 8.8 GiB and cannot be evicted while that relation runs. Compact output
tries, gates, the rolling prefix, checkpoint-load memory, and allocator
high-water behavior are also outside the manager budget. Late in the forward
sweep the observed live RSS fell much farther than the lifetime maxRSS once
those central records were consumed.
