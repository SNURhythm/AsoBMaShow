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
- `src/scene/MainMenuLibrary.*`, `MainMenuScene.*`, and chart-list views
  present the catalogue.
- Difficulty-table import and URL completion live in `src/scene/` and the
  repository layer.

## Boundaries and invariants

Workers may parse files and prepare batches but do not mutate the scan
database. Ordered scanner application owns SQLite mutation, progress, and
checkpoint updates. Archive caches are performance hints, never substitutes
for validated chart metadata. A completed or cancelled scan must release all
queued work before the scene discards its lifecycle owner.

Chart metadata is shared across profiles; player settings, scores, and replay
data are not part of the library database contract.

## Verification

Start with `chart_library_scanner_tests`, `chart_scan_work_scheduler_tests`,
`chart_repository_tests`, `chart_filter_sort_panel_view_tests`, and
`difficulty_table_*_tests`. For the scheduler's detailed operating model, see
[`src/ChartScanWorkScheduler.md`](../../src/ChartScanWorkScheduler.md).

## Related pages

- [Find BMS and downloads](find-bms-and-downloads.md)
- [Results, records, and persistence](results-records-and-persistence.md)
- [Settings and user interface](settings-and-user-interface.md)
