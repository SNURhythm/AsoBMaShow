# Find BMS and downloads

## Intent and user flow

Find BMS searches supported external sources, presents candidates, downloads a
selection, validates/stages its archive, and hands the resulting content to the
library. The flow avoids exposing partially written or unverified content as a
chart folder.

## Code map

- `src/bms_search/` contains source drivers, candidate normalization, download
  storage identity, staging, archive classification, and workflow helpers.
- `src/scene/FindBmsTask.*` owns the worker, cancellation, bounded progress,
  and result handoff for lookup, candidate download, and artifact resolution.
- `src/scene/FindBmsDialogPolicy.*` and progress presentation code connect
  search/download state to the main-menu UI.
- Archive support and repository/library services validate the downloaded result
  before library discovery consumes it.

## Boundaries and invariants

Download and extraction work use staging locations and a stable storage identity
before publishing a visible folder. Archive classification distinguishes solid
archives and unsupported entry behavior instead of assuming random access.
Cancellation and replacement handoff must leave no stale UI request or unsafe
partial artifact behind.

The task remains busy through completed-result handoff, so a fast artifact
resolution cannot re-enable actions against the previous pending artifact.
Cancelling a request is nonblocking and still delivers the service result,
including artifacts requiring a keep/delete decision. Shutdown and replacement
join all work, including uncancellable artifact resolution, and discard queued
updates. Pending progress retains the latest 160 events in order. Selection
generation and downloaded-path indexing remain application-thread scene policy.

## Verification

Use `find_bms_download_tests`, archive-file concurrency tests, chart scanning
tests, and `scripts/check_find_bms_archive_flow.py` when changing the complete
workflow.
`find_bms_task_tests` covers task admission, progress bounds, cancellation,
replacement, destruction, and complete production scene launch/delivery methods.
It includes deterministic immediate-completion artifact gating and exactly-once
indexing. `main_menu_records_lifecycle` checks the production destructor with
the real task to ensure joining before scene dependencies are destroyed.

## Related pages

- [Library and chart scanning](library-and-chart-scanning.md)
- [Mobile and platform integration](mobile-and-platform-integration.md)
