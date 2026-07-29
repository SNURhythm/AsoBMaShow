# Dynamic Pipelined Chart Scan Scheduling Design

## Context

The first dynamic scheduler removed the serial archive-discovery bottleneck, but
it introduced two new costs on an eight-core machine:

- all six scan workers may open and index separate archives at once, creating
  filesystem and decompressor contention; and
- archive entries are parsed only after every archive has been indexed, using
  outer `std::async` jobs that each create their own producer and parser
  threads. Several one-chart archives can therefore create more runnable
  threads than the scanner's worker budget while still paying a global stage
  barrier.

The first bounded pipeline then capped the entire archive operation at two
workers. Follow-up testing found it significantly slower than `develop` for
many small archives. Source review identified two causes: `develop` admits up
to four outer archive jobs, and the bounded scheduler linearly scanned one
mixed queue past every ineligible archive on every worker wake-up. With many
queued archives, those scans could approach quadratic work under the scheduler
mutex.

The corrected model must pipeline archive I/O and chart parsing without nested
thread pools, leave workers available for ordinary files, and remain
work-conserving when only archives remain.

## Goals

- Use one fixed worker budget for archive preparation and chart parsing.
- Admit one archive-I/O task while CPU work is queued, then expand archive
  admission up to the proven `develop` ceiling of four when the queue contains
  only archives.
- Keep separate FIFO queues for CPU and archive work so eligibility checks and
  dispatch remain constant-time regardless of backlog size.
- Begin reading and parsing a non-solid archive immediately after its index is
  available instead of waiting for every archive index to finish.
- Fuse indexing, reading, and parsing for one-chart archives to avoid task and
  synchronization overhead.
- Use shared-pool chart tasks for larger archives, with bounded in-flight file
  data, while preserving the optimized concurrent reader for one large
  random-access archive.
- Preserve discovery order, SQLite/checkpoint ordering, pause/stop behavior,
  archive caches, and solid-archive handling.

## Non-goals

- Measuring speed on this machine; it has no representative archive library.
- Parallel SQLite writes or a checkpoint schema change.
- Parsing charts inside solid archives.
- Changing archive formats, backend selection, or amalgamated BMS parser code.
- Adding a lock-free queue. The mutex remains appropriate once CPU and archive
  queues remove linear eligibility scans from the critical section.

## Scheduling Model

### Resource-aware shared pool

`ChartScanWorkScheduler` supports ordinary CPU tasks and archive-I/O tasks in
separate FIFO queues. All tasks share the existing `parallel_worker_count()`
budget. If CPU work is queued, archive admission contracts to one active task
and the other workers take CPU tasks. If no CPU work is queued, admission
expands up to four archive tasks, capped at one fewer than the total worker
count so a pipeline parse task can always make progress. Queue eligibility and
selection are constant-time.

Finishing the scheduler means that the external producer is done, not that
active tasks may no longer create work. An active archive reader may enqueue
chart parsing tasks. Workers exit only when finishing has been requested, the
queue is empty, and no task remains active. Calls to `enqueue()` after the pool
has fully joined are rejected.

For four workers and input `archive, ordinary, ordinary, ordinary`, one worker
performs archive I/O and the other three parse ordinary files. For a run
containing only archives, up to three readers operate at once. On the current
eight-core machine the scanner budget is six workers, so archive-only admission
expands to the `develop` ceiling of four.

### Archive pipeline

An uncached archive task lists and classifies the archive while holding one
archive-I/O admission. Solid archives stop after classification.

For a non-solid archive on a fresh scan:

- A one-chart archive reads and parses its chart inline in the archive task.
  This is the common many-small-archive case and avoids two extra queue hops.
- A larger archive streams requested chart entries. Each file becomes a CPU
  task on the same scheduler; the archive task remains the sole reader.
- Per-archive backpressure limits queued/running file data to 12 files and
  16 MiB. Four active readers therefore bound active archive data to roughly
  64 MiB, excluding one oversized entry per reader that must be admitted to
  make progress.

After discovery, if exactly one unprepared archive remains with at least 16
chart entries, the entity pool is already stopped and the scanner gives the
existing random-access archive backend the full worker budget. Its extractor
workers also parse the callbacks, so it is the only active pool rather than a
nested pool. If the format cannot support confident parallel reads, processing
falls back to the dynamic shared-CPU pipeline. This retains the
established fast path for one large ZIP or non-solid RAR without reintroducing
oversubscription when several archives are present.

Each archive owns a fixed result vector in entry order. CPU tasks write only
their assigned slots. The final task publishes one immutable prepared-archive
record after the read has completed and every accepted chart task has
completed. Read failures leave the archive entries unprepared so the ordered
fallback stage can retry them through the same bounded model.

If a scan checkpoint exists, eager ordinary and archive-entry parsing remains
disabled until the checkpoint signature has been reconstructed and validated.
Only the uncompleted suffix is then submitted to a new resource-aware pool.
This preserves resume efficiency without returning to nested per-archive
thread groups.

### Ordered application

Workers never mutate scan diffs, database state, checkpoints, progress state,
or cache collections. Prepared entities are stored in discovery-sequence
slots. The scanner thread merges those slots in order, computes and validates
the scan signature, then applies chart results and cache updates in the
existing deterministic order.

Prepared archive metadata flows with its archive batch. The later archive
application stage consumes it directly and submits only unprepared entries to
the bounded fallback pool. All SQLite and checkpoint calls remain on the
scanner thread.

## Error Handling and Shutdown

- Scheduler task exceptions are captured and reported after joining.
- An unreadable archive produces no eager chart results and does not block
  unrelated work.
- A failed chart parse records an attempted result with empty metadata so it is
  not parsed twice in the same scan.
- A failed streaming read discards eager results for that archive and allows
  the ordered fallback stage to retry safely.
- Stop or failed pause callbacks prevent new tasks, wake backpressure waits,
  drain or cancel the scheduler as appropriate, and preserve the existing
  checkpoint transaction behavior.
- No worker or future may outlive stack state owned by `Scan()`.

## Testing

Tests use synchronization and synthetic stored ZIP fixtures, not performance
timings.

1. A four-worker scheduler test queues several blocking archive-I/O tasks and
   proves archive-only work expands to three active tasks while reserving one
   worker.
2. A scheduler test proves queued CPU work contracts archive admission to one
   even when older archive tasks are waiting.
3. A scheduler test proves work enqueued by an active task is drained after
   `finish()` has begun.
4. Existing scheduler cancellation, exception, FIFO, and idempotent shutdown
   tests remain green.
5. Scanner tests create one-chart and multi-chart ZIP fixtures and verify every
   chart and archive-cache row is applied exactly once in deterministic order.
6. Checkpoint-resume, mixed ordinary/archive, stop/pause, cache, and storage
   failure tests remain green.
7. The focused scheduler/scanner/archive tests and desktop `main` build are run
   before pushing. No benchmark result or speed claim is produced locally.

## Expected Outcome

Many small archives regain up to four-way archive throughput without the
mixed-queue scanning cost. Mixed scans contract to one archive worker and keep
the remaining workers available for ordinary charts. Large non-solid archives
stream entries into the same parsing pool, while one large random-access
archive receives the full budget as the sole active pool. Both paths avoid
exceeding the scanner's worker budget.
