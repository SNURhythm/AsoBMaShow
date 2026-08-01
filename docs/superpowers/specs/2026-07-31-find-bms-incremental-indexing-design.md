# Find BMS Incremental Indexing Design

**Date:** July 31, 2026
**Status:** Self-reviewed and approved for implementation

## Objective

After Find BMS commits a downloaded archive or extracted directory, index only
that committed output. Do not start the ordinary full-library refresh path.
The downloaded charts must become visible without traversing every configured
library root or reconciling unrelated chart files.

## Verified Root Cause

`MainMenuScene::applyFindBmsUpdates()` currently calls
`startLibraryRefresh(findBmsDownloadRoot)` after a successful download or after
the user keeps mismatched files. The refresh request treats that path as an
additional root: `runLibraryRefreshTask()` still imports difficulty tables,
loads every effective library entry, appends the Find BMS root, and passes all
of those entries to `ChartLibraryScanner::Scan()`.

`BmsSearchResult::outputPath` already identifies the exact committed artifact.
It is the extracted destination directory when Find BMS unarchives a package
and the archive file itself when the archive is kept. The scanner already
supports both directory and regular archive roots, but its normal mode also
loads and reconciles all existing chart metadata.

## Considered Approaches

### Dedicated incremental task and scanner mode (selected)

Add a library task specifically for indexing a committed path. It opens the
chart repository, sends only `BmsSearchResult::outputPath` to an additions-only
scanner entry point, and reloads the library after the transaction commits.
This separates download ingestion from maintenance work and preserves the
existing full refresh and rebuild behavior.

### Reuse refresh with a narrowed root list

The ordinary refresh worker could special-case its root list to contain only
the result path. This would avoid directory traversal of other roots, but it
would keep unrelated difficulty-table work and the scanner's global metadata
reconciliation. A boolean special case inside the refresh task would also make
its name and behavior misleading.

### Parse charts in the Find BMS downloader

The download thread could parse and insert charts directly after committing
the artifact. That would duplicate archive discovery, parsing, source
preference, cache, checkpoint, cancellation, and transaction behavior already
owned by `ChartLibraryScanner`, while coupling network/storage work to the
chart database.

## Architecture

Add an `IndexDownloadedPath` library task kind carrying one
`downloadedPath`. The Find BMS result handler enqueues this task only when the
result is a completed download or a completed Keep Files resolution and
`outputPath` is nonempty. The task remains on the existing serialized library
worker, so it cannot race another chart database scan and retains the task
pause/cancellation UI.

The incremental task does not:

- import default or local difficulty tables;
- read or modify configured library folder entries;
- bootstrap an empty library;
- refresh security-scoped folder entry access; or
- clear chart metadata for a rebuild.

It opens a repository session, ensures the schema, and invokes
`ChartLibraryScanner::ScanAdded()` with the exact committed path. Progress is
reported through the existing task progress mapping. On completion it requests
the same folder/chart UI reload used by a normal successful scan.

## Additions-Only Scanner Semantics

`ChartLibraryScanner::Scan()` remains the full reconciliation API. A new
`ScanAdded()` entry point shares the discovery, archive, parsing, batching,
checkpoint, and transaction pipeline but changes how pre-existing state is
loaded and handled:

- chart metadata outside the supplied roots is not loaded, checked on disk,
  updated, or deleted;
- stale solid-archive state outside the supplied roots is not reconciled;
- every BMS chart discovered under the supplied directory is parsed and
  upserted, allowing an existing committed destination to receive updated
  files;
- a supplied archive is always reindexed through the normal archive pipeline,
  including replacing its own prior chart rows and updating archive/solid
  caches; and
- the global chart-metadata-rebuild-required marker is cleared only by a full
  reconciliation scan, never by additions-only ingestion.

Checkpoint state is still loaded for resumability. Global chart metadata,
solid-archive rows, and archive-cache rows are not loaded for additions-only
scans; the exact supplied archive is deliberately reindexed instead of being
accepted from a pre-download cache. The repository snapshot loader therefore
accepts a checkpoint-only mode.

## Data Flow

1. Find BMS downloads and transactionally commits an extracted directory or
   archive.
2. The service returns that committed path in `BmsSearchResult::outputPath`.
3. The main-thread result handler enqueues `IndexDownloadedPath` with that
   exact path.
4. The serialized library worker calls `ChartLibraryScanner::ScanAdded()` with
   one root.
5. The scanner parses and upserts charts found under that root and commits its
   batch.
6. The main menu reloads folder and chart projections from the updated
   repository.

The prior `findBmsDownloadRoot` and `additionalFolderToScan` plumbing becomes
unnecessary and is removed. Manual Refresh Library and Rebuild Library continue
using every configured entry and `ChartLibraryScanner::Scan()`.

## Concurrency with a Full Scan

The downloaded-path task shares the existing FIFO library worker with full
refresh and rebuild tasks. If one of those tasks is running or paused, database
ingestion for the download waits behind it and runs after its scan transaction
finishes.

The filesystem commit itself remains on the Find BMS download thread. Its
existing system-temp staging root cannot be used as the final swap source
because a configured library can live on another filesystem, where rename is
not atomic and may fail as a cross-device operation. Commit and backup payloads
therefore live under the reserved same-volume private namespace
`BMSSEARCH/.asobmashow-transactions/<uuid>/` until the final rename succeeds or
rolls back. A full scan that is already traversing the library disables
recursion at that exact reserved directory. Similarly named user directories
remain scannable. The queued exact-path scan then indexes the installed
destination after the full scan completes.

## Error Handling and Cancellation

An empty Find BMS `outputPath` does not enqueue an indexing task. A repository
open failure, schema failure, inaccessible committed path, parser failure, or
transaction failure is reported through the existing library task failure or
scanner diagnostics; it does not turn into a full refresh fallback.

The incremental scan uses the existing stop token, pause callback, progress
callback, and checkpoint pipeline. Find BMS result presentation remains
complete even if later indexing fails, because the downloaded files were
already committed successfully.

## Testing

Scanner regression tests will prove that additions-only scanning:

- imports charts from a newly supplied extracted directory;
- preserves an unrelated database chart whose file is now missing, proving no
  global reconciliation occurred; and
- imports charts from an exact archive path and updates its archive cache.

Full-scan and commit regressions also prove that the private transaction
namespace stays out of chart metadata, a similarly named normal directory
remains eligible, swap rollback restores the old destination, and the private
transaction directory is cleaned after rollback succeeds.

Main-menu library policy tests will prove that a downloaded-path request
produces exactly one scan entry for either a directory or archive path. The
Find BMS flow audit will require the successful result handler to enqueue the
incremental task with `findBmsResult.outputPath` and will reject the removed
download-root/full-refresh wiring.

Verification includes the focused scanner, main-menu library, and Find BMS
flow tests, the desktop `main` build, and the full CTest suite. No mobile build
is deployed.

## Scope

This change affects Find BMS post-download ingestion and the reusable scanner
mode needed to make that ingestion additive. It does not alter download or
archive commit rules, manual library refresh/rebuild semantics, difficulty
table importing, general Android document imports, or library folder
selection.
