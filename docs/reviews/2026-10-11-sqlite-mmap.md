# Chart database mmap experiment — 2026-10-11

Enable `PRAGMA main.mmap_size=268435456` on each chart repository session,
alongside the existing `journal_mode=WAL` and `synchronous=NORMAL` settings.
This is a 256 MiB mapping limit, not a resident-memory allocation. SQLite keeps
its normal read fallback when mapping is unavailable or a page is beyond the
limit. Score/replay databases, parser input, archives, and audio are unchanged.

## Method

The optional `chart_selector_query_tests --benchmark-mmap` runner compares zero
mapping against 256 MiB using the real `ResolveChartSelectorQuery` and
`SelectChartSelectorPage` methods. Each sample resolves a query, then reads the
first, middle, and last 128-row pages. It compares result counts, paths, hashes,
and titles across every sample and checks that SQLite accepted the requested
mapping limit.

Two synthetic libraries contain 5,000 and 100,000 chart rows, occupying
11,173,888 and 216,866,816 bytes including indexes. Queries cover a recursive
folder and keyword `artist3`. Each combination has eight paired trials, with
mapping order alternating each trial. The first query sequence uses a reopened
connection; the second reuses it. Connection creation is outside the timing.

Both modes use WAL and NORMAL. Fixture writes are checkpointed before timing,
so this measures a read-mostly database with a warm OS file cache. Reopening a
connection does **not** simulate cold storage. Imports, WAL-heavy workloads,
power-loss behavior, and mobile-device performance are not measured.

The recorded run used macOS 27.0.1 arm64, Apple clang 21.0.0, and the repository's
SQLite 3.53.4. All 40 selector-test translation units, including SQLite, were
compiled with `-O2`, with assertions retained. Dependency libraries were reused
from the existing debug build; this is an optimized query experiment, not a
release-app startup benchmark.

## Results

Median milliseconds for the entire query sequence; positive reduction is faster.
Raw samples: [CSV](evidence/2026-10-11-sqlite-mmap.csv).

| Chart rows | Query | Connection cache | mmap off | 256 MiB | Time reduction |
| ---: | --- | --- | ---: | ---: | ---: |
| 5,000 | Folder | Fresh | 4.066 | 3.871 | 4.8% |
| 5,000 | Folder | Warm | 3.869 | 3.552 | 8.2% |
| 5,000 | Keyword | Fresh | 6.472 | 5.670 | 12.4% |
| 5,000 | Keyword | Warm | 6.378 | 4.953 | 22.3% |
| 100,000 | Folder | Fresh | 52.129 | 50.614 | 2.9% |
| 100,000 | Folder | Warm | 51.533 | 44.718 | 13.2% |
| 100,000 | Keyword | Fresh | 88.242 | 87.703 | 0.6% |
| 100,000 | Keyword | Warm | 86.706 | 79.553 | 8.3% |

SQLite's per-connection `SQLITE_DBSTATUS_CACHE_USED` fell from 2,103,040 to
22,784 bytes in every measured case. This counter excludes mapped resident
pages and the OS cache; it does not establish a reduction in total RSS.

A preliminary optimized run also improved warm queries (4–18%), while the
large keyword query on a fresh connection was effectively unchanged. The result
supports retaining the small setting change, especially for reused connections,
without claiming a consistent cold-query speedup.

## Reproduce

```sh
cmake --build cmake-build-debug --target chart_selector_query_tests -j 6
python3 scripts/benchmark_sqlite_mmap.py --output /tmp/asobmashow-mmap-run
```

Run without other builds/tests active. The script rebuilds optimized objects in
the output directory, preserves the normal CMake objects, and saves
`benchmark.log` and `commands.json`. It requires the existing Unix Ninja build
and its compiled dependency libraries. The output directory is disposable.

For a quick comparison using the existing build configuration:

```sh
cmake-build-debug/chart_selector_query_tests --benchmark-mmap
```

## Verification

- All nine targets that directly compile `ChartRepository.cpp`, including
  desktop `main`, built successfully.
- Full CTest suite: **481/481 passed**, using `-j 6`.
- Optimized benchmark: all 128 samples completed with matching query results.
- Benchmark runner rejects an output directory equal to its dependency build.

SQLite references: [memory-mapped I/O](https://www.sqlite.org/mmap.html),
[synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous).
WAL with NORMAL preserves consistency but may lose recent commits after power
loss or an OS crash; this existing durability policy is unchanged.
