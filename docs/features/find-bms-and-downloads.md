# Find BMS and downloads

## Intent and user flow

Find BMS searches supported external sources, presents candidates, downloads a
selection, validates/stages its archive, and hands the resulting content to the
library. The flow avoids exposing partially written or unverified content as a
chart folder.

## Code map

- `src/bms_search/` contains source drivers, candidate normalization, download
  storage identity, staging, archive classification, and workflow helpers.
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

## Verification

Use `find_bms_download_tests`, archive-file concurrency tests, chart scanning
tests, and `scripts/check_find_bms_archive_flow.py` when changing the complete
workflow.

## Related pages

- [Library and chart scanning](library-and-chart-scanning.md)
- [Mobile and platform integration](mobile-and-platform-integration.md)
