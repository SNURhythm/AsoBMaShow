# Library and chart scanning

## Intent and user flow

The library turns chart folders, archives, difficulty tables, and imported
paths into a searchable local catalogue. Users choose library folders in the
main menu, browse/filter/sort discovered charts, and can request a rescan. A
scan must remain responsive: discovery and parsing run in workers while the
application thread owns visible progress and database updates.

## Code map

- `src/repositories/ChartRepository*` owns chart metadata queries and schema.
- `src/repositories/ChartScanStore*` owns scan checkpoints and archive-cache
  persistence.
- `src/ChartLibraryScanner*`, `src/ChartScanWorkScheduler*`, and archive
  helpers implement bounded discovery, parsing, ordering, and cancellation.
- `src/archive/TemporaryCache.*` owns materialized archive-media storage and
  protected cleanup; `ArchiveFile.*` supplies the current root and identities.
- `src/scene/SettingsCacheMaintenance.*` owns asynchronous Settings cleanup and
  measurement; the scene consumes typed results and formats their presentation.
- `src/archive/UnzipOutput.*` owns extraction budgets, bounded output buffering,
  and writer lifetime independently of backend decoding.
- `src/archive/IndexBuildCoordinator.*` owns per-key build admission and waiter
  outcomes; the archive facade retains cache validation and retry policy.
- `src/scene/MainMenuLibrary.*`, `MainMenuScene.*`, and chart-list views
  present the catalogue.
- `MainMenuPreviewController.*` owns preview scheduling and deferred release;
  `ChartPreloadWorker.*` supplies the shared debounced, latest-request worker.
- `SettingsLibraryTask.*` owns the exclusive table/folder job and typed updates.
  Difficulty-table import and URL completion retain their existing operation
  and repository boundaries.

## Boundaries and invariants

Workers may parse files and prepare batches but do not mutate the scan
database. Ordered scanner application owns SQLite mutation, progress, and
checkpoint updates. Archive caches are performance hints, never substitutes
for validated chart metadata. A completed or cancelled scan must release all
queued work before the scene discards its lifecycle owner.

Chart metadata is shared across profiles; player settings, scores, and replay
data are not part of the library database contract.

Main Menu keeps the selected chart because normal Start can reuse it. Preview
cancellation returns promptly; deferred media release runs after loading on the
worker. A replacement preview withdraws pending release. A handoff joins preview
work without unconditionally releasing the selected chart. Cleanup and
destruction join replay/export preparation before preview work, while callback
dependencies remain alive.

Temporary media writes and cleanup share one mutation lock. Cleanup uses the
current platform path normalizer to protect active top-level cache entries;
usage measurement remains best-effort and does not block writes or cleanup.
Full extraction shares byte, entry, and free-space budgets across archive
writers. Output streams remain owned until queued writes finish; the pipeline
joins before its guard and cancellation dependencies are released. The archive
workflow retains path reservations, recovery markers, and output publication.

Index-build waiters retain the outcome of the build they joined even when a
later request starts another build for the same key. Cancelling one waiter does
not cancel the builder or other waiters. Builder abandonment publishes failure,
and cache publication precedes successful completion. Checkpoints run outside
the coordinator mutex; data-cache locks remain outside the coordinator.

## Archive chart read budgets

The scanner's 16 MiB in-flight chart budget is a scheduling target, not a
maximum chart size or archive size. Concurrent reads explicitly use
`ConcurrentReadMemoryPolicy::AllowSingleOversizedEntry`: one oversized entry
can be extracted and parsed while all other entry reservations wait. Empty
entries also hold a reservation until their callback finishes. The default
`Strict` policy remains a hard entry-buffer limit for audio consumers.

| Archive family (including extension aliases) | Chart extraction path |
| --- | --- |
| ZIP, CBZ | Direct miniz workers for supported methods; stored entries reserve output bytes, deflated entries reserve output plus compressed scratch. Unsupported methods use the scanner's serial fallback. |
| Non-solid RAR4, CBR | Independent unarr readers, when that backend is available. Each reserves its output bytes. |
| RAR5, CBR | Solid archives and batches up to 512 MiB use one SDK handle. Larger non-solid batches use independent SDK readers. Both respect the scanner's oversized-entry policy. |
| Solid RAR4 or unavailable random-access backend | Serial streaming fallback. |
| 7z, CB7, ZIPX, LHA/LZH | Serial SDK extraction, with libarchive fallback if unavailable; solid-block dependencies are preserved. |
| TAR and gzip/bzip2/xz/zstd variants | Serial libarchive fallback. |

Serial chart parsing already admits a single oversized chart. Its queue
budget and concurrent entry reservations exclude decoder dictionaries,
parsed metadata, and memory retained by consumers. These are not process-wide
memory caps. Full extraction and bounded asset reads retain their separate
limits.

Regression coverage includes oversized stored/deflated ZIP, RAR4, small and
solid RAR5, and a compact RAR5 fixture with more than 512 MiB of expanded data
to exercise parallel SDK extraction. Tests also check cancellation, strict
rejection, exact-budget stored ZIP reads, 7z/compressed TAR fallback, and an
end-to-end scanner batch containing a 17 MiB chart.

## Verification

Start with `chart_library_scanner_tests`, `chart_scan_work_scheduler_tests`,
`chart_repository_tests`, `chart_filter_sort_panel_view_tests`, and
`difficulty_table_*_tests`. Preview ownership is covered by
`chart_preload_worker_tests`, `main_menu_preview_controller_tests`, and the
Main Menu preview/Records lifecycle fixtures. For the scheduler's detailed operating model, see
[`src/ChartScanWorkScheduler.md`](../../src/ChartScanWorkScheduler.md).
Temporary-media storage is covered by `temporary_archive_cache_tests` and the
private-root integration case in `archive_file_concurrency_tests`.
`settings_cache_maintenance_tests` covers the Settings job lifecycle, stale
completion rejection, and application-thread handoff using the real cache.
`unzip_output_tests` directly covers output policy, cancellation, failures,
backpressure, and destructor joining; backend extraction/recovery cases remain
in the archive concurrency tests.

## Related pages

- [Find BMS and downloads](find-bms-and-downloads.md)
- [Results, records, and persistence](results-records-and-persistence.md)
- [Settings and user interface](settings-and-user-interface.md)

`archive_index_build_coordinator_tests` directly covers admission, per-flight
outcomes, cancellation, exceptions, retries, and builder ownership.
