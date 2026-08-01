# Ordered Archive Result Drain Design

## Problem

The bounded archive scheduler finishes every archive read and chart parse before
the scanner begins applying any archive result to SQLite. A representative iOS
trace queued the remaining archive batches at 12.953 seconds, completed the last
stream at 22.248 seconds, then spent until 37.663 seconds applying database,
cache, and checkpoint work. Only 2.293 seconds of that 15.4-second tail was
reported as chart insertion.

`develop` overlaps these phases: the scanner applies the next result in
discovery order while other archive jobs continue. The fixed worker-pool change
accidentally replaced that overlap with a full `WorkScheduler::finish()` barrier.

## Design

Keep the existing fixed worker pool, CPU/archive work classes, archive-reader
limit, and deterministic database ordering. Add a condition variable beside the
indexed archive-result slots. A worker publishes its completed result and wakes
the scanner. The scanner walks archive discovery order and:

1. uses an already prepared result immediately;
2. otherwise waits only for the current archive's result;
3. applies that result to SQLite, the archive cache, and the scan checkpoint;
4. advances to the next archive while workers continue future archive reads.

The scanner calls `finish()` only after all ordered results have been applied.
If cancellation occurs it cancels the scheduler instead. Each queued root job
must publish either a successful result or an error result so the ordered waiter
cannot deadlock on an exception.

## Correctness and lifecycle

- SQLite/cache/checkpoint mutation stays exclusively on the scanner thread.
- Results remain deterministic even when workers finish out of order.
- The worker pool still admits at most one archive while CPU work is queued and
  expands to the configured archive-reader limit otherwise.
- Cancellation polling remains nonblocking and wakes at short intervals.
- Worker exceptions are converted into an archive error result and are also
  retained by the scheduler's existing exception reporting.

## Regression coverage

Add two deferred ZIP fixtures: a small first archive and a substantially larger
second archive. The test asserts that insertion of the first archive begins
before streaming of the second archive finishes. Removing ordered draining or
moving `finish()` back before the database loop makes this assertion fail.
Existing order, cache, checkpoint-resume, scheduler, and archive concurrency
tests remain required.

## Scope

This change restores phase overlap only. It does not alter archive admission,
checkpoint frequency, parser behavior, archive backends, or database schema.
