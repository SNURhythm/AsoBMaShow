# Streaming Archive Result Chunks Design

## Problem

Ordered archive-result draining overlaps database work with later archives, but
the current archive itself is still an all-or-nothing parse result. Its reader
and parser workers fill a complete in-memory vector before the scanner thread
can apply any row. In the latest device trace, the scanner was idle for roughly
one second before the largest archive completed, then spent most of the
remaining 1.7-second tail inserting that archive and the two archives behind
it.

## Design

Extend the bounded archive pipeline with an optional ordered-chunk callback.
Parser tasks continue filling discovery-indexed slots out of order. Whenever a
contiguous prefix reaches a small chunk threshold, the pipeline moves that
prefix into a chunk and publishes it with its starting index. The terminal
callback still reports whether every requested entry was read and parsed.

The database scheduling path stores chunks per archive and per starting index,
then the scanner thread consumes only the next expected chunk of the current
archive. This preserves both archive discovery order and entry discovery order
while allowing SQLite work to overlap the current archive's remaining I/O and
parsing. Chunk callbacks only enqueue data; SQLite, progress, cache, and
checkpoint mutations remain on the scanner thread.

Prepared archives and the existing single-large-archive concurrent fast path
continue to publish one complete result. Entity preparation also keeps the
pipeline's complete-result behavior. Incremental publication is enabled only
for deferred bounded archive batches.

## Channel and ordering

- A chunk contains its absolute first inner-entry index and a vector of
  contiguous parsed chart attempts.
- Chunk callbacks may complete out of order, so the result channel indexes
  chunks by their first entry rather than callback arrival order.
- The consumer waits for its exact next index. A successful terminal result is
  not accepted until all earlier chunks have been consumed.
- The final short prefix is flushed before terminal completion.
- Publication does not wait for the database consumer, avoiding worker-pool
  deadlocks and preserving the existing archive reader's file/byte
  backpressure.

## Completion, cancellation, and failure

A successful terminal result means all requested entries produced an ordered
parse attempt, including entries whose chart metadata is invalid. Only then is
the archive scan cache written and the archive-complete checkpoint saved.

If streaming fails after one or more chunks were applied, the scanner deletes
the archive's partially inserted chart rows and skips its cache write. This
restores the previous all-or-nothing observable result for a failed archive.
Cancellation stops chunk consumption, leaves the normal resume checkpoint
behavior intact, and cancels the scheduler during shutdown.

## Logging and regression coverage

The existing `Inserting streamed DB chart batch` line is emitted when the first
chunk is applied, and the existing finished line summarizes all chunks after
terminal completion. A real-ZIP regression uses two unprepared archives so the
bounded scheduler is selected, then asserts insertion of the first large
archive begins before that same archive logs stream completion. It also checks
the final chart count and archive caches.

Image loading and image-load telemetry are explicitly outside this change.
Local performance benchmarking remains skipped because this machine has no
representative archive library.
