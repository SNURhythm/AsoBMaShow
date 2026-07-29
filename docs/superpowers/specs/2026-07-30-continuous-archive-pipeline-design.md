# Continuous Archive Pipeline Design

## Problem

The codec-registration trace proves that `.7z` archives now use the 7-Zip SDK, but queue-to-final-insert time remains effectively unchanged: 10,011 ms before the codec fix and 9,952 ms after it. The first continuous-pipeline implementation also kept archive indexing and long archive reads in one scheduler class. Once a prefetch began, its worker remained classified as archive I/O while it streamed entries and queued CPU continuations. The scheduler therefore treated later short index jobs as additional readers and throttled them together. In the representative trace, the last archive index moved from 7,630 ms to 14,980 ms while database setup after the last index changed by only 162 ms. Prefetch was delaying the metadata needed to leave preparation.

## Goals

- Start multi-chart archive reading as soon as enough discovered work proves that the scan is not the single-large-archive case.
- Keep the dynamic reader contract: one admitted archive reader while CPU work is queued, expanding to at most four readers when CPU work is absent.
- Give short archive-index jobs independent admission so an active reader cannot delay discovery metadata.
- Publish small-archive preparation after indexing and parse those entries through the same independent reader queue.
- Preserve the full-budget concurrent backend for a scan containing one large random-access archive.
- Preserve discovery-ordered SQLite, cache, progress, and checkpoint mutation.
- Preserve bounded entry memory, incremental chunks, cancellation, and archive error behavior.

## Non-goals

- Do not change archive codecs, decompression algorithms, database schema, chart parsing, or image loading.
- Do not reorder database application or add per-archive thread pools.
- Do not add a local throughput benchmark because this machine lacks the representative archive library.

## Chosen Architecture

Keep one `chart_scan::WorkScheduler` alive from entity discovery through ordered database application. It has separate FIFO queues and active counters for CPU work, short archive-index work, ordinary archive reads, and heavy archive reads. Heavy reads are serialized so several multi-chart decompressors cannot occupy the worker budget before their CPU continuations appear. When CPU work is queued, the scheduler admits at most one index and one total reader, prioritizing a missing index lane and then a missing reader lane before additional CPU work. When CPU work is absent, ordinary reads and indexes can expand to the configured archive admission limit. Archive indexing can therefore continue alongside one heavy decompressor and the remaining parse workers instead of being counted as another decompressor.

Archive indexing publishes its `PreparedArchive` immediately after the scan result is available; archive entry parsing continues independently through a shared `ArchiveParseJobState`. The scanner waits only until every discovered entity has published preparation metadata, then builds the ordered diff and archive-batch structures while archive readers and CPU continuations remain active.

The first archive with at least 16 charts is held as a speculative single-archive candidate. If no second large archive is discovered, it reaches the existing `readArchiveEntriesConcurrently()` fast path after preparation. When a second large archive is discovered, both the candidate and the new archive enter the continuous shared pipeline; every later large archive enters immediately. This retains the single-large-archive behavior without deferring every large archive.

Each prefetched archive owns a shared result state containing ordered chunks, a terminal result, a mutex, and a condition-variable notification path. After preparation, `archiveBatchOrder` maps each archive to that state. Archives without a prefetched state are queued on the same still-running scheduler. The scanner consumes chunks in discovery order exactly as it does today and calls `finish()` only after ordered application is complete.

## Data Flow

1. Directory enumeration reserves a discovery sequence and submits ordinary or archive-index work.
2. Ordinary work publishes its prepared metadata normally.
3. Archive-index work publishes its `PreparedArchive` as soon as indexing completes.
4. Small archives immediately attach to a shared parse state and enter the archive-read queue. A large archive is either held as the sole candidate or attached to a shared parse state and queued the same way.
5. Once enumeration ends, the scanner waits for all preparation slots, not for all archive parsing.
6. The scanner builds diffs and ordered archive batches, associates any early parse states, and queues only genuinely unstarted batches.
7. The scanner drains contiguous parse chunks and performs SQLite/cache/checkpoint changes in discovery order while the scheduler continues reading and parsing later archives.
8. Cancellation cancels the shared scheduler; normal completion finishes it after archive application and reports worker exceptions once.

## Alternatives Considered

### Restore `develop`'s nested async workers

This overlaps parsing early but recreates independent producer/parser thread groups per archive and the oversubscription that motivated the bounded scheduler. It is not selected.

### Reorder the post-index archive FIFO

Largest-first ordering could shorten some tails, but it leaves the measured phase boundary intact and can delay the first discovery-ordered result. It is not selected before eliminating the barrier.

### Separate fixed I/O and CPU pools

Dedicated pools provide stronger isolation but count LZMA decompression ambiguously because archive-reader threads perform substantial CPU work. Independent work classes in the shared scheduler provide index isolation while retaining dynamic reader admission and one total worker budget.

## Error Handling and Lifecycle

- A failed early read publishes the same incomplete terminal result used by the post-index path; database cleanup and cache suppression remain unchanged.
- If the speculative candidate is never released, it has no prefetched state and follows the existing single-archive path.
- Preparation tasks must always resolve their discovery slot, including exceptions, so the preparation barrier cannot hang.
- Early results remain owned by their per-archive state until the ordered writer consumes them.
- The shared scheduler is cancelled on stop and finished once on normal completion.

## Testing

- Add a real two-large-ZIP integration test that requires both archives to use indexed prefetch and rejects post-index bounded queuing for those paths.
- Require small archives to use indexed prefetch and reject post-index bounded queuing for those paths.
- Add a scheduler regression proving a queued archive index starts before additional CPU work while one archive reader is active.
- Strengthen the single-large-ZIP test to require the existing concurrent fast-path log and reject indexed prefetch.
- Keep the existing same-archive and later-archive overlap assertions green; they prove ordered chunk application still overlaps active streaming.
- Run `chart_library_scanner_tests`, `chart_scan_work_scheduler_tests`, the desktop `main` build, iOS build-setup tests, and the iOS build-only link check.

## Expected Trace Change

Representative logs should show archive indexing continuing while `Prefetching indexed archive chart batch` work is active. The final archive-index timestamp should return near the pre-prefetch baseline instead of moving with archive extraction completion. Fresh non-solid small archives and multi-large-archive scans should have no `Queued bounded DB archive chart batch parse` lines for successfully prefetched paths, while a single large archive should continue to log `Finished single archive concurrent chart parse`.
