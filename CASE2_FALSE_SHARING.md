# Case 2: allocation-sensitive scratch false sharing

September 5, 2026. Diagnostic baseline: commit b107825. The findings below
precede the production fix; its five-case validation is in OPTIMIZATION_LOG.md.
20 workers on the i7-12700, GCC 15.2, Make native Release flags. All timing
runs were sequential, with no builds or other audit benchmarks in parallel.
Per-row `sec` excludes checkpoint loading: the clock starts inside Solver::run
after constructor/load/validation. Partials and checkpoint saving were disabled;
normal completion detection remained enabled. Row 44 is w_pos 22[0], row 45
is w_pos 22[1].

## Direct answer: continuing through row 45 does not resolve the discrepancy

Unmodified committed binary, stopping at w_pos:23 (therefore row 46):

| startup | row 44 | row 45 | row 46 |
|---|---:|---:|---:|
| fresh from @bg(7) | 17.089386 | 24.153191 | 32.132952 |
| load case2_43 | 20.937757 | 18.994232 | 26.838379 |

The startup discrepancy reverses on row 45. Node and per-column counts match
exactly. Logs: current-{fresh,resume}46.err/out.

## Scratch allocation experiment

The per-worker GateRangeScratch history and frame vectors use ordinary malloc.
On resume, initial depth 43 allocates history capacity 43 and compact-frame
capacity 43. Logging the live byte address ranges showed 23 cache lines shared
between distinct workers: 10 history/history and 13 frame/frame lines. On the
next row both capacities grow to 86 and the live regions no longer share lines.

Fresh traversal instead grows those capacities to 48. In the runtime-toggle
control it had 13 cross-worker shared lines at both depths 43 and 44: 12
history/history and one frame/history. Thus restart and fresh traversal can
give precisely the same logical work different cache-coherence costs.

An isolated header supplies an allocator for all four scratch vectors, aligning
their heap buffers to 64 bytes. Aligning just the containing struct would not
align those separate allocations. The final diagnostic binary chooses normal
operator new/delete or aligned new/delete with RLIFE_AUDIT_ALIGN; the compiled
walker is exactly the same in both modes. This flag is only consulted during
allocation/deallocation, not at each visited node.

| allocation, startup (same binary) | row 44 | row 45 |
|---|---:|---:|
| normal, resumed | 22.387640 | 20.195098 |
| aligned, resumed | 14.915212 | 19.088400 |
| aligned, fresh | 13.472523 | 18.901412 |
| normal, fresh | 18.073562 | 25.356218 |

Aligned active scratch buffers have zero cross-worker shared cache lines.
All reported node and column counts match. The ~33% row-44 resumed gain cannot
be attributed to different hot-loop code generation in this experiment. The
large startup reversal is removed, although row 44 retains a ~1.44s fresh/load
difference that this audit has not separately explained.

Earlier separate binaries corroborate it: diagnostic ordinary allocation
21.011953/19.907572s for rows 44/45, fixed-aligned 14.251069/18.866622s.

Case 2 still uses the unchanged two-phase three-projection acceptance lookup,
not the new packed single-projection path. Its nine byte-history loads provide
a plausible reason it is particularly exposed to scratch false sharing. This
audit did not separately measure history-only versus frame-only alignment.

### Hardware-counter confirmation

Final same-binary resumed row-44 pair, using perf stat:

| scratch allocation | row sec | P-core xsnp_hitm loads |
|---|---:|---:|
| ordinary | 21.702003 | 877,267,251 |
| 64-byte aligned | 14.810526 | 21,551,487 |

Event: cpu_core/mem_load_l3_hit_retired.xsnp_hitm/ (retired loads receiving
HitM responses from shared L3). This approximately 40.7x / 97.5% reduction
supports actual cache-coherence false sharing, not just a theoretical address
overlap. Remaining sharing can include intentionally atomic destination tags.
P-core instruction counts were 2.773e12 versus 2.707e12; these are not whole-CPU
instruction counts because work is dynamically distributed across P/E cores.
Perf scales counts for approximately 79% event running coverage on these runs.
The counters include process loading/teardown, whereas row `sec` does not.

Raw files: normal-hitm.stat/.err and aligned-hitm.stat/.err in the preserved
`benchmark-artifacts/case2-audit/` directory. Run this command there:

```sh
DEBUGINFOD_URLS='' perf stat \
  -e cpu_core/mem_load_l3_hit_retired.xsnp_hitm/ \
  -e cpu_core/instructions/ -o OUTPUT.stat -- env \
  RLIFE_AUDIT_LAYOUT=1 RLIFE_AUDIT_ROWS=1 ./aligned/rlife llsss \
  --load ../checkpoints/case2_43 --halts w_pos:22 \
  --threads 20 --partials none --save none --phase-timings
```

Add RLIFE_AUDIT_ALIGN=1 immediately after env for the aligned measurement.

## Commit 1149620 is not the direct regression

Built exact git archives of 6c347ae (parent/August) and 1149620, and used the
same v4 checkpoint benchmark-artifacts/august-audit/august_43 for both. Ran in
parent, child, child, parent order. Exact node and column counts match.

| commit | first row-44 run | second row-44 run |
|---|---:|---:|
| 6c347ae | 19.305027 | 19.301458 |
| 1149620 | 15.105584 | 14.945340 |

This does not establish that 1149620 intrinsically accelerates the walker:
heap layout itself is a demonstrated confounder. It does establish that the
suspected commit alone does not reproduce the later ~21s behavior, and reverting
it is not the demonstrated remedy. Changes outside the hot loop can alter
allocation ordering/layout. No full later-commit bisect was performed.

## Reproduction / isolated files

`aligned/` is a git archive of HEAD with only scratch allocation plus diagnostic
layout logging and an internal row-limit environment override. `diagnostic/`
has the latter two changes but normal std::allocator. `after114/` is an exact
archive of the suspected commit. `summarize.py` reads all logs, checks node and
column strings, and counts cross-worker cache-line overlaps in live buffers.

The full original audit (including diagnostic binaries and raw logs) is now
preserved locally in ignored `benchmark-artifacts/case2-audit/`, not `/tmp`.
Run these commands from that directory, serially:

```sh
RLIFE_AUDIT_LAYOUT=1 RLIFE_AUDIT_ROWS=2 ./aligned/rlife llsss \
  --load ../checkpoints/case2_43 --halts w_pos:23 \
  --threads 20 --partials none --save none --phase-timings

RLIFE_AUDIT_ALIGN=1 RLIFE_AUDIT_LAYOUT=1 RLIFE_AUDIT_ROWS=2 ./aligned/rlife llsss \
  --load ../checkpoints/case2_43 --halts w_pos:23 \
  --threads 20 --partials none --save none --phase-timings

RLIFE_AUDIT_ALIGN=1 RLIFE_AUDIT_LAYOUT=1 RLIFE_AUDIT_ROWS=21 ./aligned/rlife llsss \
  --rule B3578/S24678 --left-edge gse --filters bcaf --halts w_pos:23 \
  --threads 20 --partials none --save none --phase-timings c6d-f2b '@bg(7)'
```

Omit RLIFE_AUDIT_ALIGN for the fresh normal-allocation control. These diagnostic
environment variables do not exist in the repository version.

## Production implementation and CUDA implications

The production fix uses `CacheAlignedAllocator` for all four worker scratch
vectors, with no diagnostic environment switches or layout logging. Allocation
and deallocation use matching C++ aligned new/delete, with overflow checking
and preservation of element alignments stronger than 64 bytes. See
`OPTIMIZATION_LOG.md` for the resumed five-case benchmarks and validation.

The remaining smaller fresh/resumed timing gap is **unexplained and deferred**:
row 44 was 13.472523s fresh versus 14.915212s resumed; row 45 was 18.901412s
versus 19.088400s. Do not conflate it with checkpoint loading time or claim
alignment eliminates every startup effect. A future investigation can isolate
this residual; the production benchmark comparison keeps the original resumed
checkpoint protocol for all five cases.

Alignment changes neither the DFS frame size nor the per-depth history size,
and adds no per-node work to pair_step or the walker. It adds bounded allocation padding,
not another per-depth field. CUDA should choose its own scratch layout for
coalescing/bank behavior; this CPU allocator should not be transplanted as an
algorithmic requirement.
