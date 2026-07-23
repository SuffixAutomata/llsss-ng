# Succinct-tree C++ LLSSS

This is an orthogonal-only C++ rendition of the fixed-width `rlife llsss`
search. It is a new implementation, not an architectural port of either Rust
storage backend or `cpp-old`.

The persistent state has one quaternary trie for each pair of adjacent
physical columns. Each trie node is only a four-bit child mask and nodes are
stored in breadth-first order. A packed relation gate retains one bit for each
currently compatible neighboring leaf pair when filtering cannot be expressed
as a unary slice projection. An all-one gate is implicit and uses no payload.
It stores neither endpoint IDs nor join objects, and there is no join DAG or
separate column-history store.

## Build

```sh
make -C cpp -j
```

or:

```sh
cmake -S cpp -B cpp/build
cmake --build cpp/build -j
```

The default Make and CMake builds use `-march=native`, primarily so succinct
rank uses the host's hardware `popcount`. Set `NATIVE_FLAGS=` with Make, or
configure CMake with `-DRLIFE_NATIVE=OFF`, to build a portable binary.

## Run

```sh
cpp/rlife_llsss llsss [options] <geometry> <start>
```

For example:

```sh
cpp/rlife_llsss llsss \
  --rule 'B34ar5in/S2i3-i4-nwz5ceny6cei7e8' \
  --left-edge odd --filters bcaf --halts w_pos:20 \
  2c5-f2b '@bg(15)'
```

`<geometry>` is an orthogonal `cP-f2b` or `KcP-f2b` geometry. It directly
selects the row relation

```text
evolve(row[n-2P], row[n-P], row[n]) = row[n-P+K].
```

The start can be `@bg(W)` (also accepting the older `@bg:W` spelling), a
standard Life RLE file, or an ASCII grid containing `.`, `*`, and independent
`?` cells. A file must contain at least `2P` rows.

Run `cpp/rlife_llsss llsss --help` for all supported options. Zero is currently
the only background agar. Background, odd, and even edges can be selected
independently; `--symmetry asymmetric|odd|even` is a convenience spelling.
The default end detector finds a nontrivial transition into `2P` zero rows,
prints the recovered configurations as RLE, and halts. `--symmetry odd|even`
sets only the left edge; explicit `--left-edge` and `--right-edge` options can
override it independently.

RLE output places all `P` phases from left to right with 16 dead cells between
them. Phase `i` contains sequence rows `i`, `P+i`, `2P+i`, and so on. Odd and
even edge conditions are reflected into the displayed pattern: odd reflection
shares its boundary cell, while even reflection duplicates it. If both edges
are symmetric, the left-expanded row is reflected once more at the right.

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
rechecks overlap at every row. Each new three-cell row is tested against the
same static 1024-entry CA table for every complete or partially known local
equation it touches. The resulting `15 + 3`-bit row projection is stored as a
32 KiB read-only lookup shared by every slice traversal. Odd and even edges use
two additional 1 KiB projections. The even projection models its overlapping
reflected reads directly: it reads one physical history cell and requires the
two newly appended cells to agree. A compact transition table maps the two
four-bit child masks directly to the possible overlapping child pairs, so a
pair state performs one rank operation per trie and iterates only present,
CA-accepted branches.

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

The optional `bcaf` filter uses the fixed first `2P+1` rows of each lineage as
its zero-background witness, matching the Rust implementation and `cpp-old`.
It propagates witnesses in both directions, admits individual compatible leaf
pairs only when they lie on an interesting prefix or suffix, and then performs
global reachability cleanup. The BCAF predicate is recomputed during those
existing DFS passes instead of being materialized as a temporary gate. While
the surviving relation gate is emitted in deterministic synchronized-DFS
order, each finished slice is immediately reified and its old gate and tags
are released. The persistent gate is the minimum correlation state needed to
prevent a rejected pair from reappearing merely because both unary slice nodes
survive; it has no endpoint records.

End detection uses two temporary leaf suffix tags (valid and interesting), and
BCAF partial reconstruction uses one.  Under BCAF these tags are propagated by
the existing reverse cleanup walk, then the existing forward gate-emission walk
chooses and saves the corresponding lineages before reification changes node
IDs.  Thus partial and completion reconstruction add no pair-tree traversals.
The non-BCAF fallback still rescans the compact relation gate and needs neither
parent IDs nor cached join endpoints.

Pair-tree traversal is isolated from mutation of the persistent tries. With
`--threads N`, it first builds deterministic,
work-balanced ranges over the synchronized DFS stream and then executes those
ranges through a small OpenMP-backed indexed executor. The solver translation
unit itself is compiled without OpenMP, so `--threads 1` retains the original
recursive DFS. Parallel destination writes use dense private leaf planes and
word-owned reductions; relation-gate segments are concatenated in DFS range
order. Autochoke is intentionally absent.
