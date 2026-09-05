# Hot-loop reference and CPU tradeoffs

This describes the native implicit-relation engine after the September 2026
structural experiments. Legacy v1/v2 checkpoints can still select the explicit
gate engine. No CUDA implementation or GPU measurements are included here.

## Where the logic lives

- `GeometryAcceptance` generates CA acceptance tables from the lattice and rule.
- `Solver::pair_step` is the common read-only interior transition: fetch two
  child blocks, intersect geometry acceptance with overlapping child pairs,
  then apply the historical BCAF clause `P(left) | S(right)`.
- `walk_indexed_gate_range` schedules those transitions as a restartable DFS.
  It reconstructs a saved path, visits terminal parent pairs, and backtracks
  until its half-open gate ordinal interval is exhausted.
- The batch callback performs the sweep-specific support/witness propagation,
  completion handling, and accepted-path index emission.
- `SuccinctSliceTree` provides packed topology/rank and compacts surviving nodes.

The terminal parent level is uniform after expansion. Its eight potential
three-column edges are handled together; child IDs are computed as
`leaf_begin + 4 * (parent - parent_begin)`. It bypasses `pair_step` because it
needs neither rank nor child-mask reads. Current-leaf walks used for
reconstruction have a different terminal callback and do not expand children.

`PairStep` returns two scalar child starts, the transition key, and the active
edge mask. An initial version returned two whole `ChildBlock` aggregates;
despite inlining, that version enlarged the generated walker and regressed
case 4 by about 20%. Passing scalar starts/key into branch selection recovered
the pre-refactor timing. Keep aggregate materialization in mind when changing
this interface, and inspect generated code rather than assuming inlining alone
eliminates its cost. The result is temporary, not additional per-depth state.

## Per-depth DFS state: an explicit CPU tradeoff

All four CPU worker scratch buffers use 64-byte-aligned heap allocations
(or stronger alignment if their element type requires it). This prevents
false sharing between otherwise private histories/frames; aligning only the
containing worker struct would not isolate its vector allocations. It adds
bounded allocation padding, **no per-depth state**, and no per-node walker work.
CUDA should choose its own coalescing/bank-aware layout rather than treating
this CPU allocator as an algorithmic requirement. See `CASE2_FALSE_SHARING.md`
for the counter evidence and the still-unexplained small fresh/resumed gap.

On the current 64-bit host, the ordinary indexed frame is 24 bytes: two child
block starts, remaining branches, the transition key, and an optional two-bit
completion class. Exact-summary walks use a 40-byte frame containing the first
and last nonzero left-label depths. The byte history consumes one further byte
per depth and remains necessary for restart-path output and reconstruction.

The accepted packed-history experiment adds **eight bytes per depth per worker**
in a separate `GateRangeScratch::packed_history` vector. It also carries a
current 64-bit window. Thus active compact-walk storage grows from approximately
25 to 33 bytes per depth; summary-walk storage grows from 41 to 49. Vector
rounding, retained scratch capacity, and scalar temporaries are additional.
These are CPU heap-backed worker arrays, not recursive C++ stack frames, but a
GPU implementation assigning one DFS to each thread must still budget them.

The window contains the most recent 21 triples, newest in bits 0..2. Descending
stores the old window at that depth and shifts in the selected triple.
Backtracking restores the parent's window before selecting a sibling. A
geometry-generated mask gathers the five required triples directly from the
current window. The CPU uses BMI2 PEXT and a separate 32 KiB table ordered for
that gather. The original table and byte-history path remain available.

Only a single five-triple projection with positive offsets at most 21 uses
this path, and only in the indexed walker on BMI2 builds. Larger windows,
multi-phase geometries (including case 2), non-BMI2 builds, and recursive serial
walks retain the previous lookup. The extra table is counted in reported
persistent payload. Dispatch occurs once per indexed task, not per node.

The CPU timing win does **not** establish a GPU win. Candidate CUDA variants
include retaining this window, extracting five triples from byte history, or
reconstructing the window at selected backtracking points. Assemble the same
15-bit key with device-appropriate shifts/extracts; the table key permutation
is part of the contract. Compare local-memory traffic and occupancy as well as
arithmetic work. Preserve the byte-history path as the correctness reference.

## Virtual expanded leaves

Expansion changes the former leaves to full child masks and advances logical
node count and level boundaries. It omits the new zero-mask leaf bitstream and
its rank entries. Logical leaf IDs, tag-plane sizes, parent rank/select,
historical clause bytes, and terminal arithmetic retain their old meanings.
`child_mask` and `child_block` must not read virtual leaf records. All current
walkers stop before doing so; this invariant avoids a new hot-path branch.

Reification counts retained leaves during ancestry closure, compacts only
internal records, and synthesizes the retained zero tail in the output.
Completed rows therefore retain the existing checkpoint representation.
Checkpoint serialization is valid at those compact row boundaries, as before.
The single unused parallel reifier and its duplicate closure/count machinery
were removed; the solver uses serial reification or grouped parallel reification.

This change adds no per-depth state. It saves expanded topology and rank
storage, not leaf tags or persistent leaf records. On the initial case-4
comparison it reduced peak RSS by about 29%, while the row was about 2% slower
than packed history alone. Expansion and compaction became faster; sweeps were
slower. Treat it as a measured memory/time tradeoff, not an unconditional speedup.

## Scheduling and output constraints still to solve for CUDA

The sparse index splits accepted parent-pair ordinals into dynamically scheduled
16K ranges. A restart stores a raw triple path; it is not a list of endpoint
IDs. Each task must visit exactly its interval. The new index is assembled in
range order so subsequent traversals and deterministic partial selection agree.
This scheduling is independent of the immutable `pair_step` transition.

Support and witness tags use monotone atomic OR because different DFS ranges
can reach the same packed destination word. A kernel launch/barrier separates
dependent sweeps. The right sweep computes R/S; the left sweep computes L/P
and the next restart index. Retention is `L & R & (P | S)` and the retained
historical edge clause is `P(left) | S(right)`.

Destination-owned ranges might remove write sharing, but a port must establish
both complete edge enumeration and balanced work. Arbitrary traversal pruning
can repeatedly scan ancestors or traverse a large paired subtree for little
output. Removing the index is a separate scheduling experiment, not a
consequence of making `pair_step` read-only. Changed traversal order must also
address deterministic partial/completion selection and checkpoint indexing.

CPU one-worker timings use recursive walkers, while multi-worker timings use
the indexed kernel. They do not directly measure scaling of one identical
kernel. Use indexed task timings when estimating a CUDA kernel's work.

## Validation anchors

`tests/structural_regression.cpp` exhausts all 32,768 packed keys for supported
coprime geometries through period 10 under three rules, checks fallback guards,
and compares materialized/virtual expansion plus serial/grouped reification.
The tree checks include irregular tries, root-only trees, multiple compaction
tasks, lineage/ancestry recovery, clause bytes, empty retention, and checkpoint
restoration. Existing search regressions also check exact column traces,
partial/completion RLEs, legacy/runtime behavior, and partition continuation.
The smoke suite explicitly compares one-worker and four-worker P=7 row traces
and scheduled partial RLEs across multiple sparse restart ranges.
Allocator tests also verify cross-worker cache-line isolation before and after
vector growth, including 24/40-byte frames, stronger element alignment, copying,
moving, and overflow rejection.

`OPTIMIZATION_LOG.md` records the benchmark rows and measured variants. Preserve
node and column output, not just elapsed time, when evaluating a new kernel.
