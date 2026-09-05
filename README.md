# Succinct-tree C++ LLSSS

This is a C++ rendition of the fixed-width `rlife llsss` search for orthogonal
and diagonal velocities. It is a new implementation, not an architectural
port of either Rust storage backend or `cpp-old`.

The persistent state has one quaternary trie for each pair of adjacent
logical `U` columns. (`U` is the physical x-axis orthogonally and the
`(1,-1)` diagonal for diagonal velocities.) Each trie node is only a four-bit
child mask and nodes are stored in breadth-first order. Native searches retain
historical BCAF clauses on trie parents and sparse restart paths for parallel
pair traversal. The compatible relation is implicit: it stores neither endpoint
IDs nor join objects, and there is no join DAG or separate column-history store.
Legacy v1/v2 checkpoints can still use explicit packed relation gates.

## Build

```sh
make -j
```

or:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The default Make and CMake builds use `-march=native`, primarily so succinct
rank uses the host's hardware `popcount`. Set `NATIVE_FLAGS=` with Make, or
configure CMake with `-DRLIFE_NATIVE=OFF`, to build a portable binary.

`make test` runs the full orthogonal regression and the requested 114-step
diagonal parity trace. CTest uses a shorter diagonal trace so an unoptimized
Debug build remains quick.

## Run

```sh
./rlife llsss [options] <geometry> <start>
```

For example:

```sh
./rlife llsss \
  --rule 'B34ar5in/S2i3-i4-nwz5ceny6cei7e8' \
  --left-edge odd --filters bcaf --halts w_pos:20 \
  2c5-f2b '@bg(15)'
```

`<geometry>` is an orthogonal `cP-f2b` / `KcP-f2b` geometry or a diagonal
`cPd-f2b` / `KcPd-f2b` geometry. Non-coprime geometries retain their unreduced
period rather than silently searching only a primitive-period subset. The
physical velocity-period basis is

```text
orthogonal: U = (1,0,0),  V = (0,-K,P)
diagonal:   U = (1,-1,0), V = (-K,-K,P)
```

If `g = gcd(K,P)`, each logical `W` tile contains `g` orthogonal subtiles or
`2g` diagonal subtiles. One flattened subtile is one trie level, so a node and
child label remain exactly the same size in every geometry. For example,
`c5d-f2b` alternates two subtiles and starts with a 20-level CA lookback:

```sh
./rlife llsss --rule 'B35678/S4678' \
  --left-edge bg --filters bcaf c5d-f2b '@bg(11)'
```

Compatible geometries support both glide-odd (`gso`) and glide-even (`gse`)
edges. For example, this bounded invocation exercises both `c6d` subtiles
without running the much larger width-seven search:

```sh
./rlife llsss --rule 'B3578/S24678' \
  --left-edge gse --filters bcaf --ends none --halts w_pos:14 \
  --save none c6d-f2b '@bg(4)'
```

The start can be `@bg(W)` (also accepting the older `@bg:W` spelling), a
standard Life RLE file, or an ASCII grid containing `.`, `*`, and independent
`?` cells. File rows are flattened logical-`U` lattice subtiles. A file must
contain at least `2P` rows orthogonally or `4P` rows diagonally, and must end at
a complete lattice tile. `@bg(W)` constructs this lookback automatically.

Run `./rlife llsss --help` for all supported options. Zero is currently
the only background agar. Background, odd, even, glide-odd (`gso`), and
glide-even (`gse`) edges can be selected independently. Their compatibility is:

| lattice | odd | even | GSO | GSE |
| --- | --- | --- | --- | --- |
| orthogonal | yes | yes | `K,P` even | `K,P` even |
| diagonal | yes | no | `K,P` even | `K` odd, `P` even |

The default end detector finds a nontrivial transition into a full zero
lookback, prints the recovered configurations as RLE, and halts. `--symmetry`
sets only the left edge; explicit `--left-edge` and `--right-edge` options can
override it independently.

RLE output maps quotient-lattice cells back to physical `(x,y,t)` and places
all `P` time phases from left to right with 16 dead cells between them. Every
reflected edge uses the same coordinate transform as its boundary checker,
including the half-period spatial and temporal shift for glides. If both edges
are symmetric, the left-expanded configuration is reflected once more at the
right.

## Checkpoints

Checkpointing happens only after a row has finished and the tries and relation
gates are back in their compact persistent form. By default a final checkpoint
is written as `saves/save_{row}` whenever the search exits normally. The policy,
directory, and search name can be changed with:

```sh
--save none|final|every:N
--savedir DIRECTORY
--search-name NAME
```

Checkpoint files are named `DIRECTORY/SEARCH_NAME_{row}`. The directory is
created recursively when needed, and must not be an existing file. The search
name is checkpoint metadata and survives a reload, while the save directory
remains a runtime policy. Existing checkpoint files are atomically replaced.
Resume without geometry or a start grid using:

```sh
./rlife llsss --load FILE [runtime options]
```

The checkpoint's rule, geometry, start description, edges, and BCAF setting
define the stored search tree and cannot be changed. Other saved configuration
is used as a baseline and can be explicitly overridden, including `--halts`,
`--threads`, end/partial controls, and the save policy. Thus reloading a
checkpoint made at its configured halt exits immediately unless a later halt
is supplied. Ctrl-C is noticed at the next completed row; when saving is
enabled that row is checkpointed before the program exits.

### Soft memory cap and structured status

`--max-memory SIZE` requests a stop when the process's peak resident set size
reaches `SIZE`. Integer byte counts and binary suffixes such as `500MiB`,
`8GiB`, and `1TiB` are accepted; `none` disables an inherited cap.

```sh
./rlife llsss --load saves/save_217 \
  --max-memory 32GiB --halts w_pos:300 \
  --save final --status-output run-status.json
```

This is deliberately a soft cap, not an allocation limit. The expansion and
support/reification outer loops latch the peak-RSS crossing, but finish the
current flattened row before emitting a final partial and checkpoint. Memory
can therefore overshoot the cap by the transient cost of one row (and by
checkpoint output buffers). Exhaustion, a completion configured to halt, and
`--halts` take precedence when they occur on the same row, since none of those
states needs another partition.

The cap is mutable checkpoint configuration, like `--halts`, so a reload uses
the saved value unless it is explicitly changed. `--status-output FILE` is a
runtime-only path and atomically receives a JSON result after the checkpoint
has been committed. Its `reason` is one of `memory_cap`, `exhausted`,
`completion`, `halt`, `row_limit`, or `interrupted`; it also names the exact
checkpoint, row, logical W position, configured halt, completion flag, and
observed peak RSS. A missing status file or a nonzero process exit without an
`interrupted` result is an operational failure rather than a search outcome.

## Partitioning checkpoints

The dedicated `partition` command divides one selected slice into contiguous
leaf intervals.  Since final-level node order is lexicographic, every subtree
occupies a contiguous leaf interval.  A boundary can therefore duplicate at
most one ancestor at each depth, avoiding the much larger ancestry overlap of
arbitrarily grouped cut nodes.

By default the command writes small partition specifiers:

```sh
./rlife partition --load saves/save_217 --parts 4 \
  --search-name c5-search --output parts
```

This creates `parts/c5-search-1.rlp` through
`parts/c5-search-4.rlp`. A spec records the exact source checkpoint size and
checksum, saved height, selected slice, source leaf count, and half-open leaf
range. It cannot silently be applied to another checkpoint. Resume a spec as
if it were a checkpoint, supplying a later halt when the source checkpoint was
itself saved at its halt:

```sh
./rlife llsss --load parts/c5-search-1.rlp --halts w_pos:120
```

The first completed row applies the restriction in both support directions;
normal BCAF, completion, partial, reporting, reification, and checkpoint logic
then continues unchanged. Child checkpoints inherit their distinct search
names and save into the spec directory unless explicitly overridden.

Use `--materialize` to perform that ordinary first row immediately and write
self-contained child checkpoints instead of specs:

```sh
./rlife partition --load saves/save_217 --parts 4 \
  --search-name c5-search --output parts --materialize -- \
  --threads 20 --partials none
```

`--part INDEX` (one-based) materializes only that child of the full plan. This
is an optional low-level operation for targeted inspection or recovery; the
depth-first manager materializes a complete split in one command.

Arguments after `--` are ordinary mutable `llsss` runtime options. The
partition command controls load/save paths, search names, and the one-row
stop. A per-child `--partial-output` or `--dump-slice-stats` path must contain
`{name}`, which is replaced by the child search name. The same rule applies to
`--status-output`, allowing an orchestrator to classify every materialized
child independently.

Automatic slice selection uses the center for asymmetric searches and shifts
one slice toward a lone symmetry edge. `--slice INDEX` selects a one-based
slice explicitly. Ideal equal-leaf boundaries are, by default, allowed to move
within one percent of a part's leaf count in order to minimize the depth of
`LCA(R-1,R)`. Set `--boundary-slack 0` for exact equal intervals, use
`--dry-run` to inspect the ranges and spanning-node overhead, and use `--force`
to replace existing partition outputs.

## Depth-first search manager

`scripts/rlife_manager.py` automates the memory-cap cycle in fresh subprocesses
(peak RSS is process-lifetime state, so this separation matters). On a
`memory_cap` result it partitions and materializes the checkpoint, then visits
the first child to completion before the next sibling. A child that reaches
the cap is recursively partitioned; an exhausted child is retired. The
original `--halts` target is reapplied to every materialized descendant.

Start a new managed search with a new work directory:

```sh
scripts/rlife_manager.py start runs/c5-search \
  --binary ./rlife --max-memory 32GiB --disk-reserve 32GiB --parts 4 -- \
  --rule 'B34ar5in/S2i3-i4-nwz5ceny6cei7e8' \
  --left-edge odd --filters bcaf --halts w_pos:300 \
  2c5-f2b '@bg(15)'
```

The arguments after `--` are ordinary `llsss` arguments and may instead begin
with `--load CHECKPOINT`. The manager controls `--save`, `--savedir`,
`--search-name`, `--max-memory`, `--status-output`, and `--partial-output` so
those options must not be supplied there. Other runtime choices, including
threads, partial frequency, end detection, and halt-on-end behavior, are saved
normally.

The manager also treats free disk space as a soft launch constraint. By
default the minimum free-space reserve equals `--max-memory`; override it with
`--disk-reserve SIZE` (or use `none`). It checks before every extension and
before and after each complete split-materialization subprocess. Below the
reserve, the durable frontier is marked `paused` with
`pause_reason.kind = "disk_space"`, no new solver is launched, and the manager
exits with status 75. This is distinct from Ctrl-C's status 130.

For automatic reclamation, configure an archive directory on another
filesystem:

```sh
scripts/rlife_manager.py configure runs/c5-search \
  --disk-reserve 32GiB --archive-dir /mnt/archive/c5-search
scripts/rlife_manager.py resume runs/c5-search
```

When the reserve is crossed, retired checkpoint payloads are copied and
fsynced to a mirrored path under the archive before the local copy is removed.
This covers payloads from ordinary extensions and completed partition
materializations. Live or queued checkpoints and the active partition are
never moved. Small status and RLE index files stay in the work directory, and
each relocation is recorded in `state.json`. An archive on the same filesystem
is not used for automatic reclamation because moving files there cannot
increase free space. To archive every currently retired checkpoint
proactively, use:

```sh
scripts/rlife_manager.py archive runs/c5-search \
  --archive-dir /mnt/archive/c5-search
```

Press Ctrl-C once to pause. The manager forwards it to the active solver,
waits for the current row and checkpoint, commits that branch to `state.json`,
and exits with status 130. Continue or inspect it with:

```sh
scripts/rlife_manager.py resume runs/c5-search
scripts/rlife_manager.py status runs/c5-search
```

All per-run and per-materialization RLE files remain under the work directory.
The manager rebuilds `results.rle` from its artifact list, so partials and
completions are transmitted back without duplicate appends across a resume.
`events.jsonl` is the chronological machine-readable event stream. Completion
detection is active in both ordinary extension and partition materialization:
with the default halt-on-end behavior, the first such result completes the
managed run while retaining any unvisited branches in the manifest; with
`--no-halt-on-ends`, completion RLEs are collected and DFS continues. A failed
subprocess leaves the active operation recorded, and `resume` retries it;
each split is materialized by one `rlife partition` subprocess, and an
incomplete split is retried in its manager-owned output directory with
`--force`.

## Representation and sweeps

For node `n`, child label `i` exists exactly when bit `4*n+i` is one. Since
nodes are BFS ordered, its ID is:

```text
1 + rank1(4*n+i)
```

Random rank uses a 64-bit absolute count per 8192 nodes, a 16-bit count per 16
nodes relative to that absolute count, and one `popcount` for the remainder.
The persistent cost is therefore four child bits plus
`1 + 64/8192` rank bits per node, apart from word rounding and the `O(height)`
level boundaries.

At an extension, every current leaf mask changes from `0000` to `1111` and its
four children receive logical node IDs. These expanded leaves are virtual:
their zero masks and rank entries are not allocated. Synchronized DFS over
neighboring tries rechecks overlap at every flattened row. Each new three-cell row is tested
against the same static 1024-entry CA table for every complete or partially
known local equation it touches. Interior projections are generated from the
quotient lattice for each subtile phase. Checks are greedily grouped so a
lookup reads at most five historical triples;
for `c5d-f2b`, the three projections use 32,768 + 4,096 + 512 bytes per phase,
or 73 KiB total. This avoids a monolithic `2^27` lookup without adding anything
to persistent search state. Odd, even, GSO, and GSE boundaries use the same
coordinate-generated approach. They group by at most ten individual
historical bits, so each table contains at most 1024 four-bit masks. A compact
transition table maps the two four-bit child masks directly to the possible
overlapping child pairs, so a pair state performs one rank operation per trie
and iterates only present, CA-accepted branches.

Native searches use two support sweeps. Without BCAF, they compute reachability
from each boundary and retain their intersection. With BCAF, the right sweep
computes normal reachability `R` and suffix witnesses `S`; the left sweep computes
normal reachability `L`, prefix witnesses `P`, and the next sparse restart index.
A leaf survives exactly when `L & R & (P | S)`. R, witness, and completion tags
are leaf-local; L is full-tree because it becomes the reification keep plane.
Retention combines whole words, and expanded-parent clause bytes are emitted
in groups of eight. Serial and parallel reification share this reduction.

Reification tags live ancestry, then stably compacts internal four-bit records.
It counts retained leaves and synthesizes their zero-mask tail without scanning
expanded leaf records. Serial reification closes ancestry by DFS and compacts
in place; parallel reification distributes closure and compaction ranges from
all slices across one worker team. Both preserve BFS node order and restore
the ordinary compact checkpoint representation.

The optional `bcaf` filter uses the fixed first complete lookback plus one
logical `W` tile of each lineage as its zero-background witness, matching the
Rust implementation and `cpp-old`. Its window is `2P+1` flattened levels
orthogonally and `4P+2g` diagonally. It propagates witnesses in both directions,
admits individual compatible leaf pairs only when they lie on an interesting
prefix or suffix, and enforces global reachability through the retention formula
above. For retained endpoints, an edge survives exactly when `P(left) | S(right)`.
Each row stores these two historical witness bits by raw child label in a byte
on the parent. Future pair walks check the clause at its original depth,
preventing rejected pairs from reappearing merely because their endpoints
survive. No dense native pair gate is needed. Sparse restart paths preserve
deterministic traversal order.

Native BCAF completion detection propagates valid/interesting prefix tags in
the existing left sweep. Indexed traversal carries a three-state completion
class, while exact-summary walks retain first/last nonzero depths. Scheduled
partial and completion lineages are captured before reification changes node
IDs. Fallback reconstruction can walk the compact relation and needs neither
stored parent IDs nor cached join endpoints.

End detection likewise uses one complete lookback (`2P` orthogonal or `4P`
diagonal) as its zero suffix and the preceding logical tile as its interesting
witness. It runs only at complete tile boundaries.

The reported `persistent_payload_bytes` covers the allocated slice-tree child
bits, rank directories and level boundaries, historical BCAF clauses, restart
indexes, legacy relation gates, and rule/projection/transition lookup payloads.
At a row boundary those are the
large persistent state terms. It intentionally excludes small C++
object/container overhead, allocator metadata, stream buffers, configuration,
and a temporarily cached reconstructed board. `maxrss` is the process's
lifetime resident-set high-water mark, so it also retains peaks from the
expanded pre-compaction trees, the sweep tag planes, overlapping old/new gates,
reconstruction data, and allocator high-water behavior after a row compacts.

The default is one worker; `--threads N` enables dynamically scheduled indexed
pair traversal and grouped parallel reification. The common read-only interior
transition is `Solver::pair_step`; `walk_indexed_gate_range` supplies DFS
scheduling and sweep callbacks supply writes. On supported BMI2 geometries the
indexed walker carries a packed history window to avoid gathering recent byte
stores. Its extra per-depth storage and the remaining CUDA port constraints are
documented in [CUDA_PORT_NOTES.md](CUDA_PORT_NOTES.md). Autochoke is absent.
