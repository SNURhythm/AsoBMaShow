# Recursive folder statistics and opening latency follow-up

## Scope and observed failures

The approved follow-up addresses missing pre-open recursive-folder statistics
and remaining opening latency reported on iOS. After-open behavior is verified
independently rather than inferred from the user's current skin.

- The scene's rows-revision guard ignored selection-only changes. The worker
  processed directories in list order, so selected folders waited behind others.
- Entering provider-backed song rows replaced the statistics request with an
  empty list and canceled unfinished parent aggregation. Repeated navigation
  could continually restart the work.
- Late status installation updated canonical bars but not cached open-directory
  read views. Previously retained views must remain immutable when fixing this.
- Physical statistics decoded all rich metadata, sorted it, and queried favorite
  projections even though aggregation only needs score identity and mode fields.

## Implementation

- Prioritize selection initially and on selection-only changes. Cancel and
  requeue unrelated active work without publishing partial results; rapid
  reselection honors the latest priority. Generation/configuration checks reject
  obsolete callbacks. Stop callbacks execute outside the loader mutex.
- Include physical ancestors in scene requests so their aggregation survives
  opening/back navigation. Preserve intersecting completed results; reconcile
  truly empty membership and retain bounded failure retries.
- Refresh open-directory status with copy-on-write ownership, preserving old
  snapshots and song-list frames without changing list membership.
- Stream six physical-statistics columns, with no rich records, sorting,
  favorite/review projections, or replay checks. Share aggregation with the
  existing full-record path. Preserve raw duplicates, hidden rows, mode filters,
  score/clear independence, LN identity, rank buckets, and minimum lamp.
- Skip per-hash visibility lookups in broad ALL/ALL counts only when a
  statement-local check finds no hidden reviews. Narrow folders retain their
  previous indexed lookup rather than scanning unrelated reviews. No cache or
  snapshot semantics change.

The physical-statistics score callbacks receive only SHA256, key mode, LN mode,
long-note count, and backspin-note count. These are the fields consumed by the
production immutable best-score and clear-rank caches; richer metadata remains
available through the separate full-record API.

## Regression and rendering evidence

- Deterministic scheduling fixtures exercise initial priority, selection without
  row changes, preemption, rapid reselection, parent navigation, completed-result
  reuse, empty membership, cancellation, and bounded retries.
- Repository tests require one six-column stream over 4,096 records, zero rich
  rows and sorts, cancellation during score/clear callbacks, and raw aggregation
  parity across path aliases, mode/LN variants, duplicates, and hidden rows.
- Count fixtures cover hidden/favorite/NULL reviews, empty/NULL/duplicate hashes,
  and a tiny folder beside thousands of unhidden reviews. The tiny-folder test
  fails before the broad-only guard restriction and passes afterward.
- Actual property projection and Skin2DRenderer checks: unopened selected-folder
  total 0→3 and lamp/rank graph segments 0→2; selected child after category open
  total 0→7 and graph segments 0→2; late parent update reaches both fresh views
  while retained views stay unchanged; backing out restores child total/graphs.
- Selecting a song intentionally exposes no selected-directory property 300 or
  directory graph, while its enclosing folder retains aggregated data. No new
  enclosing-folder graph behavior is introduced.

## Desktop benchmark

Same 100,000-chart synthetic SQLite fixture before/after, pseudorandom titles,
empty immutable score caches, filesystem warm from seeding, fresh database
connection per pass; Debug build on this Mac. No simultaneous native builds.

| Measurement | Before | After |
| --- | ---: | ---: |
| Statistics aggregation, first pass | 1,397.87 ms | 339.35 ms |
| Statistics aggregation, second pass | 1,368.79 ms | 333.49 ms |
| Rich rows decoded for statistics | 100,000 | 0 |
| Initial SQL provider setup + first/wrapped pages | 88.85 ms | 53.50 ms |
| Same provider operation, median of five warm passes | 85.68 ms | 53.21 ms |

All 100,000 raw rows still contribute to statistics; only the unnecessary rich
projection is eliminated. These are desktop repository timings, not measured
iOS frame latency. Aggregation is still asynchronous and linear in matching
rows. Broadness is a chart-count heuristic, not a guaranteed crossover for
unusually large orphan-review tables.

## Final verification

- Focused repository/projection suites and post-review scheduling fixture passed.
- `cmake --build cmake-build-debug -j 6`: passed, including desktop `main`.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: all 329 tests
  passed in 89.28 seconds, including the full scene contract and renderer suites.
- `scripts/ios_firebase_deploy.sh --build-only --skip-init`: unsigned iOS Release
  build passed; archive, signing, and upload skipped.
- Independent reviews approved the repository and scheduling changes after
  rapid-reselection, empty-membership, and concurrency follow-ups. Diff check passed.
- No app distribution or device-runtime validation is included.
