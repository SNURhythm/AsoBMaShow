# Archive Read Discovery Priority Design

## Problem

The archive-read FIFO fix preserves the order in which read jobs reach the
scheduler, but that is not the order in which scan results are consumed.
Archive indexes run concurrently, so a later small archive can finish indexing
and enqueue its read before an earlier large archive. Database updates and
checkpoints intentionally consume archive results in discovery order.

The follow-up device trace confirms the mismatch. Manual rebuild took 13.756
seconds, up from 13.353 seconds. Fresh indexing remained effectively flat at
2.366 versus 2.328 seconds, while post-index work grew to 11.390 seconds.
`BMSFILES.zip` is consumed before `2018.zip` and `4K 2.2.zip`, but its larger
central directory finishes indexing later, so its read did not start until
10.302 seconds after the rebuild began. The previous run started that read at
9.787 seconds.

## Requirements

- Prefer queued archive reads in archive discovery order, independent of index
  completion order and archive-read class.
- Remain work-conserving: later ready archives may run while an earlier archive
  is still indexing and no earlier read is queued.
- Do not preempt archive reads that have already started.
- Preserve the existing archive admission limit, CPU priority, indexing
  behavior, cancellation, exception, and finish semantics.
- Preserve FIFO behavior for archive-read callers that do not provide a
  discovery order.
- Preserve ordered database application and checkpoint writes.
- Manual rebuild must continue clearing chart metadata, solid-archive
  metadata, archive scan cache, and scan checkpoint.

## Design

Extend `WorkScheduler::enqueue` with an optional archive-read order. Store
queued archive reads in an ordered map keyed by `(archive order, enqueue
sequence)`. Lower archive order runs first. The enqueue sequence makes equal
orders stable and gives callers using the default maximum order the same FIFO
behavior they have today. CPU and archive-index queues remain unchanged.

`ChartLibraryScanner` already assigns each prepared entity a sequence at
discovery time. Carry that sequence in `IndexedArchivePrefetch` and pass it to
the scheduler when the archive read is enqueued. Record the same sequence by
archive key so any non-prefetched fallback read uses the identical priority
namespace.

If a later archive starts while the earlier archive is still indexing, it is
allowed to continue. Once the earlier archive becomes ready, its lower order
wins the next available archive-read slot ahead of later queued work. This
keeps workers occupied without allowing asynchronous index completion to
create a long ordered-application stall.

## Testing

Add a deterministic scheduler regression test with one blocked worker. Enqueue
a later archive read first with order 20, then an earlier archive read with
order 10. After releasing the worker, assert execution order `{10, 20}`. This
must fail against arrival-order FIFO and pass with the ordered queue.

Keep the scanner integration on the real scheduler API and run the existing
scanner overlap and ordered-application tests. Do not add a timing-sensitive
integration assertion: work-conserving execution deliberately permits a later
read to start while an earlier archive is still indexing, so start order alone
is not the contract. The deterministic scheduler test covers the queued-work
ordering rule directly.

Run focused scheduler, scanner, and archive tests, the complete desktop test
suite, and the iOS build-only command. Device throughput remains the acceptance
measurement because this machine does not contain the representative archive
corpus.
