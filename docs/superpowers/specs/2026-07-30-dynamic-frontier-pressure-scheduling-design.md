# Dynamic Frontier-Pressure Archive Scheduling Design

## Problem

The unrestricted hybrid scheduler improved the latest representative manual
rebuild only from 14.682 seconds to 14.500 seconds, which is below the observed
run-to-run variation and remains slower than the earlier 13.756-second and
13.353-second runs.

The new trace shows that the read-cost signal is useful but the dispatch policy
overuses it. Archive-read work fell from 45.097 seconds to 43.259 seconds and
database insert work fell from 2.305 seconds to 1.913 seconds, yet ordered idle
time after the first insert grew from 8.382 seconds to 9.354 seconds. The last
archive read finished 805 milliseconds earlier, but the final insertion
improved by only 182 milliseconds.

Every archive slot after the logical frontier currently chooses the largest
read cost. That moved `BMSFILES.zip` and `4K U_E FULL PACK 2.1.zip` 8.587 and
10.006 seconds earlier, while delaying nearer `Love _ Justice`,
`Between Sanity and Insanity`, `Absurd Gaff`, and `eden` by 2.360 to 4.413
seconds. The scheduler removed the old late stalls but created larger stalls
closer to the ordered application frontier.

The failure is not a particular worker split. It is that one active frontier
read is treated as enough ordered coverage regardless of the number of workers
or the amount of speculative work already in flight.

## Goals

- Balance discovery-order progress and speculative latency hiding from live
  work pressure rather than a fixed number of lanes.
- Prevent one or more expensive far-future reads from consuming every spare
  archive reader.
- Allow multiple inexpensive speculative reads when their combined predicted
  work is small relative to active discovery-ordered work.
- Keep a newly ready archive ahead of every active archive on the next eligible
  reader.
- Preserve the existing shared worker budget, CPU-aware archive admission,
  archive-index isolation, non-preemption, and work-conserving behavior.
- Preserve ordered database, cache, progress, and checkpoint mutation.
- Preserve destructive manual rebuild cleanup of `chart_meta`,
  `solid_archives`, `archive_scan_cache`, and `chart_scan_checkpoint`.
- Keep the current read-cost estimator and log fields so this change tests one
  scheduling hypothesis at a time.

## Non-goals

- Hard-coding a three-to-one, one-to-one, or other worker allocation.
- Changing the total worker count or archive I/O ceiling.
- Changing archive backend selection or decompression behavior.
- Retuning the read-cost estimator from this single trace.
- Making database application concurrent or out of discovery order.
- Running a local archive throughput benchmark without the representative
  device library.

## Alternatives Considered

### Fixed ordered and speculative lane counts

A fixed split would directly prevent unrestricted speculation, but it would
encode one example worker count into policy. It also wastes capacity when the
available worker count, CPU demand, or mix of predicted archive costs changes.

### One composite score from cost and order distance

The scheduler could rank every archive by a formula such as cost divided by
distance from the frontier. This has no fixed lane count, but it introduces a
workload-specific weighting or exponent. The current cost is a useful ordering
hint, not a calibrated duration, so a scalar formula would hide an unvalidated
tuning constant inside correctness-sensitive scheduling.

### Dynamic in-flight work balance

Track the predicted work currently running for two dispatch purposes:

- **ordered pressure** for reads chosen from the discovery-order index;
- **speculative pressure** for reads chosen from the cost index.

Always close a true frontier gap first. Otherwise select expensive speculative
work only while speculative pressure is below ordered pressure; when it is not,
select the earliest queued archive and add ordered runway. This is the selected
approach because it responds to worker count and task costs without assigning
threads to permanent roles or adding a tuned order-distance constant.

## Selected Dispatch Policy

Each queued archive read retains its discovery rank, read cost, and enqueue
sequence in the existing dual indexes. Normalize a zero or missing read cost to
one only for pressure accounting; zero-cost jobs retain discovery/FIFO
selection behavior.

When an archive reader can be admitted:

1. Find the earliest queued read and the highest-cost queued read.
2. If no archive read is active, select the earliest queued read as ordered.
3. If the earliest queued rank is earlier than every active rank, select it as
   ordered. This closes a newly exposed frontier gap.
4. Otherwise, cost-based speculation is eligible only when:
   - the highest-cost candidate has a nonzero estimate;
   - it is not the same record as the earliest candidate; and
   - active speculative pressure is lower than active ordered pressure.
5. If speculation is eligible, select the highest-cost candidate and classify
   it as speculative. Otherwise select the earliest candidate and classify it
   as ordered.
6. On dispatch, add the selected normalized cost to its active pressure. On
   completion, remove the exact cost from the same pressure before waking
   workers.

This policy has no fixed speculative count. For example, a large speculative
read can make speculative pressure exceed ordered pressure by itself, causing
subsequent readers to extend the ordered runway. Several small speculative
reads may remain below the ordered pressure and use multiple available
workers. The actual mix follows current work estimates and available capacity.

When ordinary CPU work reduces archive admission, the same selection policy
runs over the smaller available capacity. No task is preempted and no worker is
reserved for a permanent role.

## Data Model

Extend the scheduler's active `WorkItem` with:

- the normalized archive read cost;
- whether the read was selected speculatively.

Maintain two unsigned active pressure totals. Archive discovery ranks remain in
the existing multiset because frontier-gap detection considers all active
reads, regardless of why they were selected. Queued records and the order/cost
indexes are unchanged.

Pressure addition is saturating and completion subtraction is defensive. The
worker that adds an active rank and pressure also owns their removal, including
when the callback throws. Cancellation clears queued indexes while active
workers finish the existing cleanup path.

## Data Flow and Correctness

`ChartLibraryScanner` continues deriving read cost during its one mandatory
archive index pass and passes it to `WorkScheduler::enqueue`. No scanner API or
database ordering changes are required.

The scheduling classification affects only which queued callback starts next.
Archive results still publish into per-archive state, the scanner still drains
them in discovery order, and SQLite/cache/progress/checkpoint changes remain on
the scanner thread. Missing costs safely fall back to ordered behavior.

## Test Strategy

Add a deterministic scheduler regression with enough blocked workers to queue
two expensive far-future reads and two cheap near-frontier reads before any
capacity is released. With a cheap active frontier, releasing three workers
must start the most expensive speculative read plus both near-frontier reads;
the second expensive read must remain queued. The unrestricted implementation
instead starts both expensive reads and leaves one near-frontier read queued.

Add a complementary test with an expensive active ordered read and several
cheap speculative reads. Their combined pressure remains below the ordered
pressure, so multiple speculative reads must use the available workers. This
guards against replacing unrestricted speculation with a hidden fixed
one-reader quota.

Keep all existing tests for newly ready earlier work, cross-class ordering,
CPU-aware admission, cancellation, exception handling, and finish semantics.

Run the focused scheduler tests repeatedly, scanner and archive concurrency
tests, the complete desktop build and CTest suite, and the iOS build-only
workflow.

## Representative Validation

No local performance claim is possible without the device archive library.
The next exported parsing log must be compared with the 14.500-second and
14.682-second traces. Success requires:

- far-future expensive reads no longer occupying all spare capacity;
- nearer archives starting materially earlier than in the unrestricted trace;
- old late expensive archives still receiving some speculative lead time;
- lower ordered idle time after the first insertion; and
- lower total manual-rebuild completion time across repeated runs.

Image-load timing remains outside parsing analysis.
