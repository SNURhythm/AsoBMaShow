# Dynamic Chart Scan Scheduling Design

## Context

`ChartLibraryScanner` already parses ordinary chart batches concurrently and
uses inner and outer parallelism when reading chart entries from non-solid
archives. Archive discovery is still serialized: recursive directory traversal
calls `scanArchivePath()`, which opens and indexes each uncached archive before
the traversal can continue. Repeated archive-open and index-building overhead
therefore dominates libraries containing many small archives.

The scheduler must share one worker budget across the mixed top-level workload.
For an input order such as one archive followed by three ordinary charts on a
four-worker machine, one worker should index the archive while the other three
workers parse the ordinary charts. It must also retain the existing behavior in
which a single large non-solid archive can use the available worker budget to
parse many chart entries quickly.

## Goals

- Stop one archive index operation from blocking discovery and preparation of
  later filesystem entities.
- Dynamically share the scanner's worker budget between archive indexing and
  ordinary chart parsing.
- Preserve the optimized inner and outer archive-entry parsing paths.
- Keep SQLite writes, scan checkpoint updates, progress reporting, and result
  ordering on the scanner thread.
- Preserve cancellation, pause, resume, archive-cache, and solid-archive
  behavior.
- Avoid timing-based production behavior and fixed worker reservations by file
  type.

## Non-goals

- Parallel SQLite writes.
- Direct browsing of archives reached through Android Storage Access Framework
  tree paths; those remain unsupported by the current scanner.
- Parsing solid archive chart entries in place.
- Changing archive formats, archive backend selection, or BMS parser behavior.
- Removing the existing checkpoint model or changing its persistent schema.

## Scheduling Model

The scan will have two worker-backed preparation stages and one scanner-thread
application stage.

### 1. Dynamic entity preparation

Recursive traversal remains on the scanner thread and becomes a producer. It
assigns a monotonically increasing discovery sequence to each new ordinary
chart or archive, then immediately enqueues eligible work without waiting for
the preceding entity.

All workers consume one FIFO queue:

- An ordinary-chart task parses metadata for one new chart path.
- An archive-index task validates file state and, when its persistent cache is
  absent or stale, lists and classifies one archive. This task discovers chart
  entry paths but does not start nested archive-entry parser threads.
- Persistent archive-cache hits are inexpensive and may be resolved by the
  producer without occupying a worker.

Each top-level task consumes one worker. No worker slots are permanently
reserved for either task kind. With four workers and the discovery order
`archive, ordinary, ordinary, ordinary`, the archive task occupies one worker
and the ordinary tasks can occupy the remaining three.

The existing `parallel_worker_count()` policy supplies the total budget, so the
scanner continues to reserve capacity for render, audio, and main threads. A
single-worker environment executes the same queue serially.

Workers return immutable result records keyed by discovery sequence. They do
not mutate scan-diff collections, known-path collections, database state,
progress state, or checkpoint state. The scanner thread merges result records
in discovery order after the producer closes the queue and all workers finish.
This keeps archive batch ordering and scan signatures deterministic even when
work completes out of order.

For a fresh scan, parsed ordinary metadata may be retained until the database
application stage. If a checkpoint is already present, eager ordinary parsing
is disabled until the checkpoint is validated, so resuming a scan does not
redo the entire ordinary-file prefix. Archive inspection can still use the
shared queue because the scanner must reconstruct archive batches to validate
the checkpoint signature.

### 2. Archive-entry preparation

After entity results have been merged, non-solid archive chart entries use the
existing archive batch pipeline. The current pipeline already:

- runs multiple archives concurrently when there are many archive batches;
- gives multiple entry workers to a large non-solid archive when capacity is
  available;
- bounds in-flight file count and byte usage; and
- falls back to streaming reads when a random-access backend is unavailable.

Ordinary charts prepared during the first stage are not parsed again. Because
the first stage has finished, the archive-entry pipeline can use the full
scanner worker budget without competing with nested ordinary-chart threads.
The current speculative archive prefetch pool will be removed or folded out of
the first stage so there is only one worker budget active at a time.

### 3. Ordered database application

The scanner thread performs all deletions, source-preference updates, chart
upserts, archive-cache writes, checkpoint commits, and final transaction
commit. Ordinary metadata is applied in discovery order. Archive batches keep
their discovery order, and chart entries keep archive entry order.

Progress callbacks also remain on the scanner thread. Worker completion wakes
the coordinator but does not invoke UI-facing progress callbacks directly.

## Components

### Shared work scheduler

A small scanner-specific scheduler will own:

- the fixed total worker count chosen at scan start;
- a mutex-protected FIFO work queue;
- condition variables for available work and completion;
- task exception capture;
- queue closure and worker joining; and
- cancellation-aware rejection of new work.

The scheduler accepts callable tasks rather than knowledge of charts or
archives. This makes its dynamic occupancy and shutdown behavior testable
without filesystem timing hooks. It will live in focused source files rather
than adding more queue mechanics to the already large scanner implementation.

### Entity result records

`ChartLibraryScanner` will define result records for ordinary charts and
archives. An archive result contains readability, solid status, file count,
uncompressed size, discovered virtual chart paths, and any diagnostic text.
The result contains all data needed for scanner-thread integration, so worker
code never writes shared scanner collections.

The existing archive scan helper will stop accepting `knownChartPaths` by
mutable reference. It will deduplicate entries locally and return its paths in
the result.

### Archive backend concurrency

The 7-Zip open-cache mutex currently covers both cache-map access and the
potentially slow archive open on a cache miss. That would serialize independent
RAR5 and 7-Zip index tasks even after the scanner begins scheduling them on
different workers.

The cache mutex will be narrowed to cache lookup, insertion, use-counter
updates, and eviction. Opening an uncached archive will happen outside the
global cache mutex, followed by a locked second lookup before insertion. The
existing per-archive state mutex will continue to serialize access to one
cached archive handle. Consequently, independent archive paths can open in
parallel without permitting concurrent operations on the same handle.

## Error Handling and Shutdown

- A malformed or unreadable archive produces an unsuccessful archive result,
  is logged once by the scanner thread, and does not prevent unrelated work
  from completing.
- A chart parser exception produces an empty ordinary-chart result using the
  existing parse diagnostic behavior.
- An unexpected task exception is captured by the scheduler and converted into
  a failed entity result; it does not terminate a worker thread or the process.
- A stop request closes the producer side, prevents new work, wakes every
  worker, joins all workers, and leaves database/checkpoint handling to the
  existing scanner-thread cancellation path.
- Pause callbacks may block worker work as they do in the current concurrent
  parse paths. Resuming wakes work without changing queue order.
- Destruction joins workers; no task may outlive the scanner stack data it
  references.

## Testing

Tests will avoid elapsed-time thresholds.

1. A scheduler test enqueues blocking tasks and proves that up to the configured
   worker budget can be occupied concurrently. With one archive-shaped task
   followed by ordinary-shaped tasks, it verifies that later work begins while
   the first task remains active.
2. A scheduler shutdown test verifies that queue closure and cancellation wake
   workers and join cleanly.
3. A scanner integration test creates real stored ZIP fixtures and ordinary BMS
   files in mixed discovery order. It verifies that ordinary charts and archive
   charts are all indexed and that archive cache records are written.
4. A scanner integration test uses several small archives to verify every
   archive is inspected and applied exactly once despite out-of-order worker
   completion.
5. An archive backend test opens two uncached 7-Zip-backed fixtures from
   separate threads and uses a synchronization barrier, not a duration
   threshold, to prove that independent cache misses are not covered by the
   global cache lock.
6. Existing checkpoint-resume, pause/stop, archive cache, and storage-failure
   tests remain green.
7. The focused scanner tests, relevant archive tests, and the repository's
   desktop compile check are run before completion.

## Expected Outcome

Many small archives can be opened and indexed concurrently, and mixed ordinary
files no longer wait behind an archive encountered earlier in traversal. A
single large non-solid archive continues to use the established parallel entry
pipeline, while database and checkpoint behavior remains deterministic.
