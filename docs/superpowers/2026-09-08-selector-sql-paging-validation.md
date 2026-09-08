# Selector SQL paging validation

## Outcome

Normal recursive-folder opening no longer builds a whole-folder C++ song index.
SQLite resolves the count and filters, returns ordered pages, and locates chart
identities. Each provider retains six 128-row pages; opening primes at most two
pages. Explicit autoplay and the detected legacy DURATION overflow exception
retain their previous whole-folder behavior.

## Reproducible measurement

Run the production repository/provider benchmark:

```sh
cmake --build cmake-build-debug --target chart_repository_tests -j 6
cmake-build-debug/chart_repository_tests --benchmark-first-page all 100000
```

Measured on 2026-09-08, Apple M1 Pro, macOS 26.5.1, existing desktop debug build.
All three modes use the same real SQLite database containing 100,000 unique
SHA256 values, nested folders and pseudorandom titles, with empty immutable score
caches. Every pass opens a new SQLite connection. Seeding warms the OS cache;
these are **not cold-filesystem or device/UI latency measurements**. No build or
test processes were running during this measurement.

Milliseconds, median of five subsequent passes:

| Mode | Count/preparation | First 128 rows | Wrapped last page | Total preparation |
| --- | ---: | ---: | ---: | ---: |
| MainMenuScene-style QueryChartMeta | 203.300 | 224.557 | 1458.455 | 1888.589 |
| Previous C++ selector index | 2207.319 | 3.050 | 0.682 | 2211.051 |
| SQL selector provider | 78.267 | 3.197 | 0.647 | 82.112 |

The first pass totals were 1827.818, 2253.926 and 80.652 ms respectively. SQL
subsequent totals ranged from 79.392 to 169.090 ms. Each column has its own
median, so the displayed component medians need not sum to the total median.

Selector page phases include SQL execution, rich-record decoding, projection
and replay-file existence checks. Previous-index preparation includes the
complete narrow visitor, indexing and sorting. The MainMenu comparator performs
only repository count/page reads, without selector projection. Its forced
wrapped-page request and this synthetic data distribution do not represent the
user's observed few-millisecond MainMenuScene opening. The supported conclusion
is the measured selector improvement from 2.21 seconds to 82 ms on this fixture,
not a universal latency guarantee or an equivalent end-to-end UI comparison.

## Query-plan evidence

An independent harness captured actual production resolve/page statements and
ran EXPLAIN QUERY PLAN on 1,028 rows, including a four-row narrow folder:

- Broad TITLE pages scan `idx_chart_meta_selector_title` without a temporary
  sorting B-tree. The wrapped page reverses the order and uses SQL offset zero.
- Raw SHA representative lookup searches the covering
  `idx_chart_meta_selector_representative` index.
- Broad distinct-SHA count scans that covering index without temporary grouping.
- Narrow folders use folder-index searches; temporary grouping/sorting is
  confined to their matching rows rather than forcing the global TITLE index.

Count resolution still performs database-wide work. Non-title ordering and
identity ranking may also scan/sort all matching rows in SQLite. Score sorts
use immutable-cache UDF lookups, not per-chart score database queries. The
DURATION overflow compatibility exception deliberately retains the old index.

## Verification

- Desktop `main` and all test targets compile successfully.
- Repository, SQL differential and provider tests: 3/3 pass. Coverage includes
  all filters/sorts, duplicates, hidden fallback, score modes, cancellation,
  stale snapshots, normalized identities, clone ownership, bounded decoding,
  error retries and DURATION compatibility.
- The same-path/different-SHA legacy fallback regression fails before the
  identity guard and passes afterward; independent scoped review finds no
  remaining blocker.
- Full `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: 328/329
  pass on both runs. The unchanged `builtin_renderer_characterization_tests`
  timing/trace assertions fail under suite concurrency, but pass on an isolated
  retry and five further consecutive isolated runs. No renderer source or
  golden fixtures were changed; this full-parallel limitation remains open.
- Unsigned iOS `scripts/ios_firebase_deploy.sh --build-only` succeeds, followed
  by a successful `--build-only --skip-init` recheck after the final fix.
- `git diff --check` passes. No deployment or mobile-device smoke test performed.
