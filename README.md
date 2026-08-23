# Succinct-tree C++ LLSSS

This is a C++ rendition of the fixed-width `rlife llsss` search for orthogonal
and diagonal velocities. It is a new implementation, not an architectural
port of either Rust storage backend or `cpp-old`.

The persistent state has one quaternary trie for each pair of adjacent
logical `U` columns. (`U` is the physical x-axis orthogonally and the
`(1,-1)` diagonal for diagonal velocities.) Each trie node is only a four-bit
child mask and nodes are stored in breadth-first order. A packed relation gate
retains one bit for each currently compatible neighboring leaf pair when
filtering cannot be expressed as a unary slice projection. An all-one gate is
implicit and uses no payload. It stores neither endpoint IDs nor join objects,
and there is no join DAG or separate column-history store.

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
./rlife_llsss llsss [options] <geometry> <start>
```

For example:

```sh
./rlife_llsss llsss \
  --rule 'B34ar5in/S2i3-i4-nwz5ceny6cei7e8' \
  --left-edge odd --filters bcaf --halts w_pos:20 \
  2c5-f2b '@bg(15)'
```

`<geometry>` is an orthogonal `cP-f2b` / `KcP-f2b` geometry or a diagonal
`cPd-f2b` / `KcPd-f2b` geometry. Orthogonal non-coprime periods are not yet
supported. Reducing the fraction searches only its primitive-period subset,
not every solution of the unreduced geometry. Orthogonal geometry directly
selects the row relation

```text
evolve(row[n-2P], row[n-P], row[n]) = row[n-P+K].
```

Diagonal geometry uses lattice basis

```text
U = (1,-1,0), V = (-K,-K,P).
```

If `g = gcd(K,P)`, each logical `W` tile contains `2g` subtiles. One flattened
subtile is one trie level, so a node and child label remain exactly the same
size as in an orthogonal search. For example, `c5d-f2b` alternates two subtiles
and starts with a 20-level CA lookback:

```sh
./rlife_llsss llsss --rule 'B35678/S4678' \
  --left-edge bg --filters bcaf c5d-f2b '@bg(11)'
```

The start can be `@bg(W)` (also accepting the older `@bg:W` spelling), a
standard Life RLE file, or an ASCII grid containing `.`, `*`, and independent
`?` cells. File rows are internal search rows: ordinary rows orthogonally and
flattened logical-`U` subtiles diagonally. A file must contain at least `2P`
rows orthogonally or `4P` rows diagonally, and must end at a complete lattice
tile. `@bg(W)` constructs this lookback automatically.

Run `./rlife_llsss llsss --help` for all supported options. Zero is currently
the only background agar. Background, odd, and even edges can be selected
independently; `--symmetry asymmetric|odd|even` is a convenience spelling.
Odd and even reflection are currently orthogonal-only; diagonal searches must
use background edges. (Diagonal odd reflection is phase-staggered, while
diagonal even reflection is not compatible with the vanilla lattice.) The
default end detector finds a nontrivial transition into a full zero lookback,
prints the recovered configurations as RLE, and halts. `--symmetry odd|even`
sets only the left edge; explicit `--left-edge` and `--right-edge` options can
override it independently.

RLE output places all `P` phases from left to right with 16 dead cells between
them. Orthogonally, phase `i` contains sequence rows `i`, `P+i`, `2P+i`, and so
on. Diagonal output maps the logical lattice cells back to physical `(x,y,t)`
and renders physical time phases. Orthogonal odd and even edge conditions are
reflected into the displayed pattern: odd reflection shares its boundary cell,
while even reflection duplicates it. If both edges are symmetric, the
left-expanded row is reflected once more at the right.

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
four zero-mask children are appended. Synchronized DFS over neighboring tries
rechecks overlap at every flattened row. Each new three-cell row is tested
against the same static 1024-entry CA table for every complete or partially
known local equation it touches. Orthogonal geometry uses one `15 + 3`-bit row
projection, stored as a 32 KiB read-only lookup shared by every traversal.
Diagonal projections are generated from the lattice for each subtile phase.
Checks are greedily grouped so a lookup reads at most five historical triples;
for `c5d-f2b`, the three projections use 32,768 + 4,096 + 512 bytes per phase,
or 73 KiB total. This avoids a monolithic `2^27` lookup without adding anything
to persistent search state. Orthogonal odd and even edges use two additional
1 KiB projections. A compact transition table maps the two four-bit child
masks directly to the possible overlapping child pairs, so a pair state
performs one rank operation per trie and iterates only present, CA-accepted
branches.

Without BCAF, one sweep starts at the left boundary and the opposite sweep
intersects reachability from the right boundary. With BCAF, dependency order
combines normal right reachability with suffix witnesses, then combines normal
left reachability, prefix witnesses, and the first global-cleanup direction.
The reverse cleanup and final gate emission bring the total to four pair-tree
traversals instead of six independent traversals. Normal reachability, witness
reachability, and global cleanup use six simultaneous packed tag bits per
expanded node. This deliberately avoids a much larger temporary bit tape over
every compatible neighboring leaf pair.

Reification performs one DFS to tag live ancestry, then stably compacts the
four-bit records in place. Whole-tree walks exploit BFS ordering with one child
cursor per depth; children remain contiguous, so these walks do not need a
rank lookup for each child. This deletes empty nodes and leaves the new current
leaves as the zero-mask tail for the next extension.

The optional `bcaf` filter uses the fixed first complete lookback plus one
logical `W` tile of each lineage as its zero-background witness, matching the
Rust implementation and `cpp-old`. Its window is `2P+1` flattened levels
orthogonally and `4P+2g` diagonally. It propagates witnesses in both directions,
admits individual compatible leaf pairs only when they lie on an interesting
prefix or suffix, and then performs global reachability cleanup. The BCAF
predicate is recomputed during those existing DFS passes instead of being
materialized as a temporary gate. While the surviving relation gate is emitted
in deterministic synchronized-DFS order, each finished slice is immediately
reified and its old gate and tags are released. The persistent gate is the
minimum correlation state needed to prevent a rejected pair from reappearing
merely because both unary slice nodes survive; it has no endpoint records.

End detection uses two temporary leaf suffix tags (valid and interesting), and
BCAF partial reconstruction uses one.  Under BCAF these tags are propagated by
the existing reverse cleanup walk, then the existing forward gate-emission walk
chooses and saves the corresponding lineages before reification changes node
IDs.  Thus partial and completion reconstruction add no pair-tree traversals.
The non-BCAF fallback still rescans the compact relation gate and needs neither
parent IDs nor cached join endpoints.

End detection likewise uses one complete lookback (`2P` orthogonal or `4P`
diagonal) as its zero suffix and the preceding logical tile as its interesting
witness. It runs only at complete tile boundaries.

This version is serial. Pair-tree traversal is isolated from mutation of the
persistent tries, leaving its top-level branch work suitable for later
parallelization. Autochoke is intentionally absent.
