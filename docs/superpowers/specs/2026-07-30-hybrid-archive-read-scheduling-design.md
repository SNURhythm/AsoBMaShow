# Hybrid Archive Read Scheduling Design

## Problem

The discovery-priority archive read queue fixed the identified priority
inversion, but it did not reduce representative iOS manual-rebuild time. The
latest trace completed in 14.682 seconds, compared with 13.756 seconds for the
preceding trace and 13.353 seconds for the trace before that.

The new trace confirms that reads are selected in discovery order, but ordered
database application still stalls behind expensive archives that occur later
in that order. Three visible stalls before `c.s.q.n`, `NightTheater`, and
`-終天-` were roughly 1.4 seconds, 1.0 second, and 1.6 seconds. During those
stalls, later archive capacity had previously been spent on cheaper work even
though the expensive archive indexes had long been available.

Archive size alone is not a useful scheduling estimate for this workload. In
the latest trace, archive extraction duration correlated only 0.13 with
estimated unpacked size. The highest requested archive-entry ordinal correlated
0.58 with extraction duration. For example, an archive whose requested charts
end near entry 1,222 can be much more expensive than a larger archive whose
requested charts are near its beginning.

The primary success metric is total manual-rebuild completion time. Starting
the first archive database insertion slightly later is acceptable when total
completion improves.

## Goals

- Always keep an earliest-ready archive moving toward ordered application.
- Use otherwise available archive capacity to start expensive future reads
  early enough to hide their latency.
- Retain the current contraction to one archive reader when ordinary CPU work
  is queued.
- Preserve ordered database, cache, progress, and checkpoint mutation.
- Preserve non-preemptive, work-conserving execution.
- Preserve destructive manual rebuild behavior, including clearing metadata,
  solid-archive state, archive scan cache, and scan checkpoints.
- Add enough information to the existing performance log to verify scheduling
  decisions on the representative device without adding new per-entry noise.

## Non-goals

- Changing archive decompression backends or parser behavior.
- Making database application concurrent or out of order.
- Persisting scan indexes or archive read-cost estimates across a manual
  rebuild.
- Tuning a device-specific fixed thread count.
- Benchmarking archive throughput on the development machine, which has no
  representative archive library.

## Alternatives Considered

### Bounded lookahead

The scheduler could reorder only the next fixed number of archives. This limits
how far speculative work can move, but the current stalls include expensive
archives far from the frontier. A fixed window would either miss them or need a
large, workload-specific size.

### Separate archive-reader and parser pools

Dedicated pools would isolate blocking archive producers from CPU parsing, but
they would also introduce two independently tuned thread budgets. Because
decompression itself consumes CPU, this risks oversubscription and requires
device-specific tuning. The existing shared worker pool already enforces one
global budget.

### Restore the `develop` outer-worker pipeline

The earlier implementation achieved better throughput on the representative
library, but it used nested worker groups, materialized whole-archive results,
and lacked the current ordered streaming, checkpoint, and bounded-memory
behavior. Restoring it would discard correctness and memory improvements and
would be a much larger regression surface.

## Selected Model

Use one logical frontier slot plus cost-prioritized speculative slots inside the
existing fixed worker pool.

The frontier is not assigned to a particular thread. Instead, the scheduler
tracks the discovery ranks of active archive reads and compares them with the
earliest queued rank whenever a reader can be admitted:

1. If no active archive read has a rank earlier than or equal to the earliest
   queued rank, select the earliest queued archive. It becomes the frontier.
2. If an active read already covers the frontier, select the queued archive
   with the highest predicted read cost for the newly available speculative
   slot.
3. Break equal-cost ties by discovery rank and then enqueue sequence.
4. Never preempt a running read. If an earlier archive becomes ready while all
   slots are occupied, it wins the next eligible dispatch.
5. When queued CPU work contracts archive admission to one, do not admit
   speculative reads. Existing reads finish non-preemptively under the current
   behavior.

This keeps ordered progress live while applying longest-predicted-processing
first to capacity that would otherwise duplicate frontier ordering.

## Read-Cost Signal

Archive indexing already visits entries in archive order and identifies chart
entries. `ArchiveScanResult` will record a read-cost estimate equal to one plus
the highest entry index containing an accepted chart candidate. An archive
with no chart candidates has cost zero.

The estimate deliberately does not use archive byte size. It is inexpensive,
available during the mandatory fresh index, and is the strongest predictor
available in the device trace. It is a scheduling hint only; correctness never
depends on it.

The scanner propagates the estimate through both paths:

- Eager indexed prefetch carries discovery rank and read cost in
  `IndexedArchivePrefetch`.
- Deferred or checkpoint-compatible archive parsing looks up the same rank and
  cost by normalized archive key and passes both to the scheduler.

Callers that omit the cost receive zero. Equal zero-cost work retains discovery
order and enqueue-sequence FIFO behavior.

## Scheduler Data Model

Each queued archive read record contains:

- the work callback;
- discovery rank;
- predicted read cost;
- a monotonic enqueue sequence.

The scheduler maintains two indexes over the same queued records:

- an order index sorted by `(discovery rank, enqueue sequence)`;
- a cost index sorted by `(read cost descending, discovery rank, enqueue
  sequence)`.

Dispatch removes the selected record from both indexes. A sorted multiset of
active archive-read ranks identifies whether a frontier read is already
running. Completion removes the corresponding active rank before waking other
workers. The order index remains the authoritative pending-work count, so
finish and cancellation behavior do not depend on stale priority entries.

CPU and archive-index queues remain unchanged. Both archive read work classes
continue to share one admission counter and one pair of scheduling indexes.

## Data Flow

1. Folder discovery reserves a prepared-entity sequence for each archive.
2. Archive indexing enumerates entries, collects chart paths, and records the
   maximum accepted chart-entry ordinal as read cost.
3. The scanner queues an archive read with its prepared-entity sequence and
   read cost.
4. The scheduler admits an earliest-order frontier read first.
5. While that frontier is active and archive capacity remains available, the
   scheduler admits highest-cost speculative reads.
6. Archive workers continue publishing contiguous parsed chunks.
7. The scanner consumes chunks strictly in archive discovery order and performs
   all database, cache, progress, and checkpoint changes on the scanner thread.

## Cancellation and Failure Handling

Cancellation clears both archive queue indexes and the existing CPU/index
queues while running tasks observe the existing stop path. Failed archive reads
publish the same terminal error results as today. A rejected enqueue still
publishes a rejected terminal result so ordered application cannot wait
forever.

Read cost is advisory. A missing or zero estimate falls back to discovery/FIFO
ordering and cannot change error handling or result order.

## Observability

The existing `Prefetching indexed archive chart batch` line will include the
archive discovery rank and predicted read cost. No new per-entry lines are
added. Existing start, extraction-complete, stream-complete, insertion, and
final-insert lines remain sufficient to reconstruct whether expensive later
archives used speculative capacity and whether ordered application stalls
shrank.

Image-load timing remains outside parsing performance analysis.

## Test Strategy

Scheduler unit tests will verify:

- the earliest queued archive is selected when no frontier read is active;
- while a frontier read is active, a higher-cost later archive wins a newly
  available speculative slot;
- an earlier archive that becomes ready while later reads are active wins the
  next dispatch;
- equal-cost and default-cost archive reads preserve discovery/FIFO order;
- both archive read work classes use the same frontier and speculative policy;
- finish, cancellation, exceptions, and dynamic CPU contraction retain their
  existing behavior.

Scanner tests will verify that indexing derives the maximum chart-entry ordinal
and that eager and deferred archive read paths pass identical discovery-rank
and cost metadata. Existing real ZIP, checkpoint, ordered-streaming, cache-count,
and single-large-archive tests remain required.

## Validation

Local validation consists of the focused scheduler, archive concurrency, and
scanner tests, followed by the complete desktop build, all CTest tests, and the
iOS build-only workflow. No local performance benchmark will be claimed.

Representative validation uses exported iOS parsing logs. A successful result
must show expensive later archives starting in speculative slots, shorter
ordered-application idle gaps, and lower total manual-rebuild completion time
without a correctness regression. Because the latest traces show meaningful
run-to-run I/O variation, the scheduling decision should be judged from both
the reconstructed critical path and repeated total times rather than one
isolated archive duration.
