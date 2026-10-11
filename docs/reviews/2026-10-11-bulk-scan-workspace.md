# Bounded workspaces for bulk chart scans

The scanner reuses input bytes, decoded text, timing-position storage and timeline
arena blocks on each parsing thread. A new parser is still constructed for each
chart, preserving independent random state. No result borrows workspace memory.
Each buffer category retains at most 1 MiB after a call; large charts may use more
while parsing, then release oversized storage. Cancellation and exceptions also
reset storage. Existing Scan/Parse overloads retain their behavior.

Parser source: `SNURhythm/bms-parser-cpp` commit
`7d26b6dcec9d19a477738fe2e46c0889fb621a48`; the app's amalgamated sources match
that repository's generated output byte for byte.

## Measurement method

- App baseline: `f21a900a` (before this workspace integration).
- Machine: Apple M1 Pro, eight logical CPUs, macOS arm64; Apple Clang 21.0.0.
- Both parser versions and scanner benchmark objects use `-O2 -g0` and the same
  existing dependency libraries. Standalone parser allocation counting uses
  per-job ordinary C++ `new`/`new[]`, not all `malloc` calls or resident memory.
  One workspace construction per worker is outside allocation counters but
  inside wall time.
- 563 private local charts, 76,740,789 bytes: 268 BME, 168 BMS, 69 BML, 58 PMS.
  Sizes: minimum 14,560 bytes, median 79,257, p95 218,600, maximum 8,441,623.
- One additional PMS chart lacks both an initial BPM header and a tempo event at
  the origin; both baseline and candidate reject it. It is excluded from timing.
- Inputs, source snapshots, commands and raw results remain outside git. Source
  charts are read only. Fresh-library scans use copies containing only chart
  files, no archives or audio/image assets.
- Both benchmark-only parser constructors use seed 12345. Production random
  behavior is unchanged. Parser fingerprints include all metadata and feature
  flags. Database comparisons include every `chart_meta` column except the
  wall-clock `add_date`.
- Seven samples alternate baseline/current order. Each parser sample creates
  its worker pool and processes the corpus once, including chart destruction.
  Buffers are reused within the sample. Inputs are preloaded even in file mode;
  file scans reread them during timing, so filesystem caches are warmed.
- Each fresh-library sample starts a new process and a fresh SQLite database.
  Database initialization is outside the timer; discovery, parsing, upserts and
  final commit are inside `ScanWithResult`. The scanner uses its normal hardware
  worker budget (eight here). These are local ordinary-file scans, not mobile or
  archive-extraction measurements.

Reproduce with a private newline-delimited chart manifest and a populated
`cmake-build-debug` build of `chart_library_scanner_tests`:

```sh
python3 scripts/benchmark_chart_scan.py --baseline f21a900a \
  --files-from /private/chart-paths.txt --output /tmp/chart-scan-benchmark
```

The sibling parser repository supplies `test/performance.cpp` and
`test/ChartSnapshot.h`; override its location with `--parser-repo`. `--build-only`
and `--skip-build` separate compilation from timing. Reused binaries require
matching saved baseline/source/runner provenance. Do not run timing alongside
compilation or other benchmarks.

## Results

Median of seven samples, in milliseconds. There is no demonstrated throughput
improvement: candidate medians are 0.2–3.0% slower, with overlapping sample ranges.
The concrete benefit is reduced requested allocation volume, not faster scanning.

| Case | Workers | Baseline ms | Workspace ms | Requested bytes, baseline → workspace | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| Preloaded Scan | 1 | 1000.390 | 1002.420 | 617,407,588 → 533,610,964 | 13.6% |
| Preloaded Scan | 4 | 309.174 | 318.543 | 617,407,588 → 538,490,236 | 12.8% |
| File Scan | 1 | 1035.500 | 1037.220 | 696,512,438 → 566,315,242 | 18.7% |
| File Scan | 4 | 316.921 | 319.590 | 696,512,438 → 572,967,351 | 17.7% |
| Fresh library and database | 8 | 196.786 | 201.380 | Not counted | — |

Fresh-library ranges were 191.267–219.416 ms for baseline and 194.166–239.518 ms
for the workspace version. Allocation **counts** decreased only about 0.23–0.25%:
for single-worker file scans, 2,739,579 → 2,732,699. Reuse primarily removes large
buffer allocations, while existing per-chart containers still allocate heavily.
Parallel requested-byte totals vary slightly with work distribution and each
worker's high-water buffer capacity.

All 563 chart fingerprints match for both byte and file overloads. Every fresh
scan stores all 563 charts, and stored metadata matches across all 14 runs after
excluding insertion time. Both versions also reject the separately checked
missing-initial-tempo PMS chart. This does not claim equivalence for untested
inputs or cold storage performance.

Median stage totals in the workspace fresh-library runs (worker milliseconds):

| Stage | Summed ms |
| --- | ---: |
| Input reads | 52.514 |
| MD5 and SHA-256 | 199.090 |
| Decode and newline normalization | 92.399 |
| Remaining parsing | 1042.625 |
| Serial chart upserts | 6.311 |

Parsing accounts for about 75% of the four measured worker stages. These totals
support investigating parser containers/token processing next; they do not
support prioritizing ordinary input mapping on this corpus. No span/mmap input
or encoding fast path is introduced in this change.

## Stage diagnostics

`Chart scan worker stages` adds summed input-read, hash, decode and remaining
parse durations plus serial chart-upsert time. Worker sums can exceed wall time.
Archive extraction/prefetch, discovery, parser construction, result wrapping,
checkpoint writes and final commits are outside these selected stages. Existing
phase logs and an outer scan timer remain necessary for elapsed-time analysis.

## Regression coverage

The parser suite compares reused-workspace results with the existing API across
mixed encodings and PMS fixtures, retains results across later calls, exercises
cancellation and reuse, and runs eight independent workspaces concurrently.
After warming a 256 KiB chart, four scans make zero allocations of at least
128 KiB (the non-reusing implementation made eight). A 4 MiB chart releases
oversized storage, and workspace destruction releases all tracked live storage.

Validation completed: parser regular and amalgamated suites, allocation/retention
regressions and benchmark harness build; full app build and all 481 CTest tests
passed. The focused scanner suite also passed. Review found no production
correctness issues; benchmark output containment and reuse-provenance findings
were fixed and rechecked.
