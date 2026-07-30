# Archive Read FIFO Design

## Problem

The chart scan scheduler stores `ArchiveRead` and `ArchiveReadHeavy` jobs in
separate queues, then always checks the ordinary archive-read queue first.
Both classes use the same active-reader counter and admission limit, so the
priority difference does not protect a resource. It only lets later small
archive reads overtake earlier heavy reads.

The 2026-07-30 device trace demonstrates the resulting head-of-line delay.
`LAST PROPOZE.7z` completed extraction at 6.188 seconds, but ordered database
application could not reach it until 8.280 seconds because the earlier
`Kaleidoscope.7z` and `Shooting Silver Bullet.7z` heavy reads were overtaken
and did not start until 7.529 and 7.682 seconds.

## Requirements

- Preserve enqueue order across `ArchiveRead` and `ArchiveReadHeavy` jobs.
- Preserve the existing dynamic archive admission limit and CPU priority.
- Preserve the public `WorkClass` values so callers do not need to change.
- Preserve cancellation, exception collection, and finish semantics.
- Do not change manual rebuild cleanup; it must continue clearing chart
  metadata, solid-archive metadata, archive scan cache, and scan checkpoint.

## Design

Route both archive-read work classes into the existing archive-read FIFO.
Internally, they can be handled as `ArchiveRead` because their lifecycle is
already identical: both increment and decrement `activeArchiveReads_` and use
the same eligibility test. Remove the redundant heavy-read queue and its
priority branches.

Archive indexing remains a separate queue because it has its own active count
and is deliberately admitted alongside parsing work. CPU work remains a
separate FIFO and continues to contract new archive admission to one reader
while CPU work is queued.

## Testing

Add a deterministic one-worker regression test. Hold the worker with an active
CPU task, enqueue a heavy archive read followed by a regular archive read,
release the worker, and assert the two reads execute in enqueue order. The test
must fail against the dual-queue scheduler by observing the regular read first.

Run the focused scheduler test, the chart scanner and archive concurrency
tests, the full desktop test suite, and the iOS build-only verification. No
local throughput claim will be made because this machine lacks the device's
archive corpus.
