# Recycling lane-map nodes without changing parsing results

The parser's 16 ordered lane maps now share a per-parse node pool. Erasing an
old slot returns its storage to a free list, and a later insertion reuses it.
The maps, their comparisons, source-order collision rules, long-note handling,
statistics, encoding, hashes and random behavior are unchanged. Event compaction
and timeline structures are unchanged.

Parser implementation: `5689702f234e8e7e5ecc1953abeed9c556526673` in
`SNURhythm/bms-parser-cpp`. The app amalgamation matches its generated output.

The pool is declared before the maps so it outlives them, and releases all its
blocks at the end of the parse. It retains approximately the maximum simultaneous
live-node storage, not one node allocation for every note encountered. Rebound
allocators with a different size/alignment and multi-element allocations use
matching ordinary allocation/deallocation. The pool is private to one parse.

## Evidence and acceptance criteria

The accepted condition was improved full-library scan time with identical
parsing results. Allocation reduction by itself was insufficient.

A 15-second macOS `sample` profile of the previous parser on the private real
chart corpus showed allocation/free activity on `ParserNotes::normal` and
`ParserNotes::advance`. `collect()` was a much smaller sampled cost. This trial
therefore changes only the lane-map allocator.

Baseline: app `a851ed2e1d0779aba2e1e5f942f668491aeeb9e0`, parser
`7d26b6dcec9d19a477738fe2e46c0889fb621a48`. Both sides use worker workspaces;
`--baseline-workspace` in the benchmark runner makes that choice explicit and
records it in build provenance. Benchmark byte parsing now supplies the original
source name, so PMS mapping is exercised in full/metadata/ready/reference modes.

The throughput corpus and machine match the [workspace experiment](2026-10-11-bulk-scan-workspace.md):
563 BMS/BME/BML/PMS charts, 76,740,789 bytes, Apple M1 Pro with eight logical CPUs,
Apple Clang 21.0.0, optimized benchmark objects (`-O2 -g0`) and identical dependency
libraries. Source files are read only. Full-library runs scan chart-only copies
into fresh databases, with normal eight-worker scheduling. No archives or audio
assets are included. File caches are warmed; this is not a mobile-device result.

Seven samples alternate baseline/current order. Parser allocation counters cover
ordinary per-job C++ allocations, excluding one workspace construction per worker
and non-C++ allocations. Full-library timers include discovery through final
commit, excluding database initialization. Builds, tests and sampling are stopped
before throughput measurement. A separate initial seven-sample trial is retained
as a repeatability check, not mixed into the final medians.

## Throughput results

Final medians of seven samples:

| Case | Workers | Baseline ms | Node pool ms | Elapsed-time reduction |
| --- | ---: | ---: | ---: | ---: |
| Preloaded Scan | 1 | 999.662 | 966.217 | 3.3% |
| Preloaded Scan | 4 | 308.582 | 298.310 | 3.3% |
| File Scan | 1 | 1033.650 | 998.098 | 3.4% |
| File Scan | 4 | 327.825 | 308.069 | 6.0% |
| Fresh library and database | 8 | 204.246 | 192.856 | 5.6% |

The candidate was faster in all seven paired parser samples for each case and
six of seven full-library pairs. Full-library ranges overlap: baseline
195.635–233.237 ms, candidate 184.737–239.561 ms. The independent first trial also
improved its full-library median, 202.470 → 192.902 ms (4.7%). This supports a
modest local improvement, not a guaranteed device-independent percentage.

Single-worker file-scan allocations fell from 2,732,699 to 1,262,783 (**53.8%**),
and requested allocation bytes from 566,315,242 to 505,024,706 (**10.8%**).
Preloaded single-worker Scan fell from 2,731,007 to 1,261,091 allocations, and
533,610,964 to 472,320,428 requested bytes. Retaining tree lookup means allocation
reduction is much larger than elapsed-time reduction.

Every fresh-library run stored all 563 charts. All `chart_meta` columns matched
across both versions and samples, excluding only wall-clock insertion time.

## Result equivalence

The comparison covers 563 real charts and 300 generated timing/random/resource/
channel edge cases, using seeds 12345 and 20261001. Full, metadata, ready-measure,
and file modes compare the previous parser directly with the candidate; scan mode
also compares candidate metadata/features with previous full parsing. Separate
byte/file Scan comparisons and fresh-database comparisons use the benchmark runner.

Snapshots compare field values and graph links rather than memory addresses.
While checking coverage, detached-note data and resolved stop durations were
found missing from the existing helper. Both are now serialized, with mutation
regressions that first failed with the old helper. The expanded comparison
includes metadata, resources, measures, timelines, normal/invisible/background/
landmine notes, detached notes, long-note partner links and resolved stop duration.
All 8,630 chart/mode/seed comparisons matched after that expansion.
The previously rejected missing-initial-tempo PMS also remains rejected by both
versions in full, file, byte-Scan and file-Scan modes.

## Allocation and safety regressions

The 256,000-note scan previously used 279,609 general-heap allocations. It now uses
23,615, with measured peak live bytes changing from 1,139,320 to 1,144,656.
The regression ceiling is tightened to 0.5 allocations per note while retaining
the 8 MiB peak bound and zero retained-allocation requirement. The old allocator
fails this test. Scan and metadata modes pass it with the pool.

Pool tests interleave two live maps over repeated erase/reinsert cycles, exercise
size-rebind and multi-element fallback, overflow rejection, and recovery after a
mapped-value constructor throws. Existing cancellation tests still interrupt work
without publishing a partial result. The parser's regular and amalgamated suites
pass, as does the allocator implementation under AddressSanitizer and
UndefinedBehaviorSanitizer.

## Reproduction

```sh
python3 scripts/benchmark_chart_scan.py --baseline a851ed2e \
  --baseline-workspace --files-from /private/chart-paths.txt \
  --output /tmp/parser-note-pool-benchmark
```

Full semantic comparison uses the sibling parser repository:

```sh
python3 scripts/check_parser_equivalence.py BASELINE_BINARY CANDIDATE_BINARY \
  --manifest /private/chart-paths.txt --output /tmp/parser-note-pool-equivalence \
  --workers 4 --scan
```

Both binaries must use the current `performance.cpp` and `ChartSnapshot.h`, linked
against their respective parser snapshots. Private paths, inputs, samples and raw
fingerprints remain outside git.

## Final integration validation

The full desktop app build passed. All 481 CTest tests passed in parallel with
`ctest --test-dir cmake-build-debug --output-on-failure -j 6` (126.44 seconds).
The final expanded parser suite also passed under AddressSanitizer and
UndefinedBehaviorSanitizer. Regular and amalgamated parser tests passed after the
snapshot-helper changes. The app's generated parser files match the committed
parser output byte for byte. Independent review found no remaining issues in the
allocator or comparison/test changes.
