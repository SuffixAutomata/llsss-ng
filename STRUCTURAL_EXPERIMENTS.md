# Remaining-direction experiments, September 2026

Baseline: committed `67fa84f` (cache-aligned indexed scratch). This pass examines
the five directions formerly listed under “Remaining directions” in
`OPTIMIZATION_LOG.md`. The production changes are word-wide BCAF retention/clause
emission and leaf-local R. Scheduling and rank representation are unchanged.
Final paired five-case measurements and validation are recorded in that log.

Large checkpoints, frozen binaries, experimental source trees, and raw logs are
preserved locally under `benchmark-artifacts/remaining-directions/`, not `/tmp`.
Benchmark drivers and their small results are in `benchmarks/`. Both directories
are currently ignored by Git; this document and the optimization log are the
versioned handoff. Back up the local artifacts separately if preserving them
across clones matters. All timings below used 20 workers with no concurrent
builds or other audit benchmarks. Small differences are not meaningful.

## Completed state versus traversal order

Yes: changing traversal order should preserve the completed slice trees if the
same CA-compatible, historical-clause-compatible edges are enumerated and the
same support/witness recurrences are applied. These recurrences combine bits by
OR, followed by `L & R & (P | S)` retention. Their result does not depend on the
order of visits. Stable reification should then preserve the same BFS topology
and the same raw-label historical clause bytes.

The first chosen partial may change. The ordered restart index may also change,
even when the represented relation does not. Therefore a genuine ordering
rewrite must compare topology, level boundaries, clause bytes, and accepted-edge
counts, rather than treating changed partials or changed index order as proof of
a changed search tree. It must also check completion behavior and continuation.

The production changes here do not change traversal order; existing partial
expectations remain valid. Complete checkpoint comparisons are consequently
stronger than aggregate node/column comparisons and can include the index too.

## Destination-owned left sweep: isolated, not retained

### Ownership is more than reversing labels

The paired DFS interleaves decisions from both trees at every depth. Although
its local triple order favors left labels, its terminal left-node sequence is
not globally monotone: choosing another right branch higher up can revisit a
left subtree. The symmetric issue holds for the right-node sequence. Merely
reversing the local order therefore does not give a task exclusive destination
words.

The first prototype partitions right-tree parents into disjoint BFS intervals,
lifts the endpoints to ancestor intervals, and prunes the paired DFS to each
owned interval. Geometry acceptance, historical clauses, support/witness writes,
and completion classification remain the existing scalar operations. The
prototype writes the same support results, then replays the original indexed
walk **without support writes** to produce the original ordered index and
partials. That second pass exists for validation, not as a proposed production
design. Comparing only the owned-sweep phase is an optimistic screen: it gives
the ownership proposal credit for entirely free index replacement.

L uses absolute tree-node coordinates, while P/completion planes use leaf-local
coordinates. A parent range aligned to the local words can still cut an L word.
Most writes are ordinary ORs, but every writer to a shared boundary word remains
atomic. The subtree variant checks both absolute and leaf-local boundary words.
Mixing an atomic writer with another task's ordinary write to that same word
would be a race, even if their intended bits differ.

### Variants and observations

| Owned left-sweep variant | Case 4 phase | Case 2 phase | Case 5 phase |
|---|---:|---:|---:|
| BFS parent intervals, 4096 parents/task | 1.861s | 9.061s | — |
| 1024 parents/task | 1.969s | — | — |
| 65536 parents/task | 1.940s | — | — |
| skip bounds inside wholly owned subtrees | 2.019s | — | — |
| above plus right-label-first branch order | 1.999s | 9.489s | 7.443s |
| whole destination-prefix subtrees | 2.008s | — | 7.340s |
| prefix subtrees with smaller tasks on narrow columns | 2.276s | — | 7.879s |
| original indexed left sweep, **including index emission** | 1.872s | 6.767s | 6.503s |

These are screening runs, not a statistically paired comparison of every
variant. Later runs also show general machine/timing drift. Nevertheless, no
variant establishes a CPU win even under the optimistic phase-only comparison.
Whole-row prototype times additionally include the separate index replay and
are deliberately not presented as a candidate production speedup. The initial
case-4 whole-row comparison was 5.328s versus 3.739s for the original.

The normal and reordered prototypes pass the existing smoke/partial tests, and
the large tested rows match full node/column output. The whole-prefix prototype
also produced a byte-identical case-4 row-30 checkpoint against the baseline,
including topology, clause bytes, and the replayed original index. Bounds replay, prefix
restriction, task construction, per-task ancestry recovery, and different load
balance eat the anticipated savings. This does not prove that every possible
destination-owned implementation must be slower; it rejects these concrete
CPU proposals as a basis for removing the current scheduler now.

### Best next CPU design if revisiting ownership

- Preserve the common read-only `pair_step` and separate the planner from the
  walker. Do not add a new independent copy of acceptance/clause logic.
- Partition whole destination subtrees or explicit aligned tiles, but estimate
  **paired-edge work**, not just destination leaf count. The current index's
  16K-edge ranges are valuable for balancing dense and sparse joins.
- Keep an indexed fallback where a narrow destination has many compatible
  source paths. Destination ownership limits task count there; splitting the
  source work again needs a reduction or shared destination writes.
- Treat replacement of the next-row index as a separate invariant. Concatenating
  destination-ordered output into the old PairGate format does not make the
  original indexed DFS consume that order correctly. Either define a matching
  new scheduler/index, or consistently remove the restart index from native
  walking, reconstruction, loading, and continuation. Accepted-edge counts and
  historical clauses must remain correct in either design.
- Do not retain the diagnostic second full emission walk. Any proposed win must
  include its real scheduling/output costs and completed-tree verification.

The interval prototype carries two O(depth) node-boundary arrays, approximately
16 extra bytes/depth/task; the prefix prototype needs one ancestor path. The
“contained” flag fits existing frame padding, so the compact frame remains 24
bytes, but the boundary arrays are **still additional state**. None of this
experimental state or duplicated walker code was added to production.

### CUDA implications

CPU timings do not settle the GPU design. One cooperative block owning a
destination tile can reduce contributions locally and assign exclusive global
output words, while several threads explore its source branches. Per-thread
destination ownership would risk poor parallelism on narrow joins and duplicate
prefix work. A GPU design should measure that parallelism and its local
reduction cost rather than assuming that eliminating global atomic OR is free.

The O(depth) boundary paths could be shared by the cooperative task instead of
replicated in every thread. This matters alongside the existing 24-byte frames,
byte history, and optional 8-byte packed-history prefix. A frontier of joint
prefix states and weighted tasks may be a better GPU starting point than porting
the CPU's entire indexed restart/backtrack mechanism literally. Those are design
candidates, not GPU performance claims; no CUDA code was benchmarked here.

## Word-wide keep and clause emission: retained

`PackedTags::window` provides a zero-padded unaligned 64-bit view. One shared
`execute_bcaf_keep_range` now evaluates retention by destination word, translating
leaf-local R/P/S into the full-tree L/keep coordinate system and preserving bits
outside the owned range. Both serial and parallel reification use it.

Clause emission packs eight parents at a time by spreading 32 P bits and 32 S
bits into eight raw-label clause bytes. A parent's byte belongs to the task
containing its first child, even if a keep-word boundary splits the four child
bits. Prefix/suffix tails and arbitrary word offsets remain supported. The
portable byte-order path preserves the same logical byte sequence.

An unused standalone BCAF reifier callback was removed instead of maintaining a
third copy of the old bit-at-a-time loop. The changes add no DFS state. GPU
retention can similarly assign independent output words / clause groups without
changing the paired walker.

The initial isolated case-5 comparison was 15.850s -> 15.091s, with unchanged
peak RSS. Case 4 changed 3.723s -> 3.663s; that smaller difference alone would
not establish a reliable whole-row speedup.

### A rejected helper extraction

The first full five-case comparison caught a case-2 regression after a later
cleanup extracted a separate `filter_bcaf_keep_range` helper from the callback:
13.258s -> 14.355s, despite improvements in the other four cases. The added time
was in the **left sweep**, not retention. An isolation sequence measured baseline
13.615s, word-wide only 14.629s, initial word-wide + leaf-R 13.502s, and the latter
with helper extraction 14.663s. Thus neither the leaf offset nor the reduction
alone explains the pattern.

Restoring the shared callback body regenerated a binary byte-identical to the
initial leaf-R build (SHA-256
`0a53e2d426c020e68164813a3702bc84a3c8321ed0b088c74f6a70f2ab60d36d`).
The recovery comparison was 13.571s -> 13.591s; the final five-case sequence
measured case 2 at 13.471s -> 13.505s. Both are neutral. The separate helper was
not retained. The serial path invokes that callback
through a one-element range vector; this small host-side allocation adds no
per-depth state. Code generation/layout is a plausible explanation for the
nonlocal timing effect, not an established instruction-level diagnosis. Future
refactors should keep case 2 in the benchmark set instead of adding a diagonal-
specific algorithm exception on this evidence.

## Leaf-local R: retained

Only L is consumed as a full-tree ancestry-closure/keep plane. R is queried only
at leaves, so `SupportTags` now names a full-tree `left` plane and leaf-local
`right` plane. Boundary traversal omits internal R marks; partition restriction,
suffix seeding, retention, and counts use the correct local offset. The old
numbered `normal[i][0/1]` accesses become explicit `.left/.right` accesses.

The initial case-5 word-wide-versus-leaf-R comparison was 15.091s versus 15.106s,
with maxRSS falling from 5,637,520 to 5,423,484 KiB (about 209 MiB). This is a
memory saving, not a claim that every case gets another speedup. A different
phase can still determine process peak RSS, as in the initial case-4 pair.

R and S now share leaf-local coordinates, which also makes a future right-sweep
ownership experiment simpler. L deliberately stays full-tree: converting a
leaf-local L into a new closure plane would need another storage/lifetime tradeoff.

## Persistent tag-buffer reuse: not retained

An isolated pool keeps L/R, witnesses, and completion-prefix buffers across rows.
It was tested both with exact-size growth and optional 2x capacity headroom.
Case 4 resumed at row 29 and continued through rows 30 and 31:

| Variant | Row 30 | Row 31 | Process peak KiB |
|---|---:|---:|---:|
| word-wide baseline | 3.977s | 9.040s | 1,964,660 |
| retain buffers, exact growth | 4.007s | 9.150s | 2,642,964 |
| retain buffers, 2x headroom | 3.931s | 8.936s | 2,637,456 |

All rows match exact node/column output. The approximately 34% peak-RSS increase
is material; the timing differences are small. The old code frees R/witness/
completion planes **before** reification allocates its output, so keeping those
planes live changes the peak overlap. Furthermore, plain `reset_size/assign`
does not avoid allocation when every row exceeds the previous capacity. Case 4
grows rapidly enough that even 2x headroom does not guarantee reuse everywhere.

This rejects the claim that whole-plane pooling is a no-cost peak-memory win.
It does not rule out selective L-only reuse, stable/shrinking searches, or an
explicit memory-budgeted pool. Such policies would need their own multi-row
measurements; no persistent pool was put into the production solver.

## Interleaved rank: screened, not integrated

`benchmarks/rank_layout.cpp` compares the exact current rank formula with a
64-byte-aligned block holding seven data words and one cumulative rank. It also
tests a branchless variant that executes all six preceding-word popcounts with
masks. This is a **synthetic uniform-node lookup benchmark**, not a full solver
or a recorded walker trace. Both layouts are built from the same deterministic
bitstream; sampled ranks and timed checksums agree.

One billion lookups, 20 workers, forward then reverse variant order:

| Data words | Current layout | Interleaved loop | Interleaved branchless |
|---|---:|---:|---:|
| 32 MiB, run 1 | 1.006s | 1.426s | 1.709s |
| 32 MiB, run 2 | 1.002s | 1.463s | 1.661s |
| 256 MiB, run 1 | 2.085s | 2.310s | 3.016s |
| 256 MiB, run 2 | 2.097s | 2.321s | 2.973s |

The combined bitstream/rank storage drops by 8.7%, but neither implementation
wins CPU lookup time even at the larger footprint. Consequently the additional
reification/serialization/layout complexity was not integrated into the solver.
This screening result does not assert an end-to-end regression or rule out a
different access distribution / GPU-cooperative rank scheme.

## Reproduction anchors

- `remaining-directions/before/rlife`: frozen `67fa84f` baseline.
- `remaining-directions/wordwide/`: isolated word-wide binary/source snapshot.
- `remaining-directions/leaf-r/rlife`: initial word-wide + leaf-R binary.
- `remaining-directions/after/rlife`: rejected later helper extraction.
- `remaining-directions/restored/rlife`: restored shared callback, identical to
  the initial leaf-R binary.
- `remaining-directions/final-state-equality/`: all five final checkpoints
  compared byte for byte against the baseline; binary fingerprints and logs.
- `remaining-directions/owned-first/rlife`: first interval prototype.
- `remaining-directions/owned-probe/`: final prototype, including range,
  right-major, prefix, and adaptive variants.
- `remaining-directions/pool/`: exact/headroom reuse experiment.
- `remaining-directions/rank-layout`: isolated rank executable; source in
  `benchmarks/rank_layout.cpp`.

All paths in the list above are relative to `benchmark-artifacts/` except the
explicit benchmark source path. Prototype flags are intentionally absent from
production: `RLIFE_OWNED_PROBE`, `RLIFE_OWNED_QUANTUM`,
`RLIFE_OWNED_RIGHT_MAJOR`, `RLIFE_OWNED_PREFIX`, `RLIFE_OWNED_ADAPTIVE`, and
`RLIFE_POOL_HEADROOM`. The usual CLI input remains `--load` the preserved
pre-target checkpoint, 20 threads, `--partials none --save none --phase-timings`.
Never add the prototype index-replay phase to a claimed production speedup.
