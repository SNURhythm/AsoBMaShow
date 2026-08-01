# Fresh Archive Indexing Performance Design

## Problem

Manual rebuild must continue clearing chart metadata, solid-archive records,
archive scan cache, and checkpoints. In the representative iOS trace, root
discovery reaches its first archive 36 ms after the rebuild starts, while the
last fresh archive index finishes 2.351 s after the start. Three large ZIP
central directories dominate that window:

- 299,517 entries: 1.584 s in miniz indexing and classification;
- 172,056 entries: 1.128 s;
- 294,543 entries: 1.189 s.

The current miniz loop calls the library pause callback for every entry,
performs a separate filename lookup before reading the file stat that already
contains ordinary filenames, normalizes safe entry paths more than once, and
filters system paths both inside the ZIP reader and again in the common cache
builder. The common lookup builder then normalizes the already-normalized
stored path again. The scanner separately walks the copied entry vector twice.

## Decision

Keep destructive manual rebuild semantics unchanged and streamline the fresh
index path:

1. Check the pause hook at a bounded interval while walking ZIP central
   directory entries, plus before and after the loop. A 256-entry interval
   removes hundreds of thousands of cross-layer callback calls while retaining
   sub-millisecond to low-millisecond pause responsiveness at observed rates.
2. Read `mz_zip_archive_file_stat` first and use its embedded, zero-terminated
   filename for normal-length names. Fall back to the full filename API only
   when the embedded field may be truncated.
3. Return the normalized path produced by `safeEntryPath()` directly instead
   of normalizing it again.
4. Rely on the common system-entry filter once rather than duplicating it in
   the miniz loop.
5. Build exact lookup keys directly from stored normalized entry paths.
6. Classify file count, size, solidity, and BMS paths in one scanner pass with
   the same bounded pause polling.

Add an `IndexingArchives` progress stage after root traversal and before the
prepared-entity wait. It makes the remaining fresh archive cost distinct from
directory enumeration without altering scheduling or hiding total work.

## Constraints

- `ClearChartMeta()` remains unchanged and destructive.
- Archive formats, backend selection, cache validity, parsing, SQLite ordering,
  and the dynamic scheduler remain unchanged.
- Unsafe paths and system-generated entries remain excluded.
- Long ZIP filenames must continue to use the untruncated filename.
- Cancellation must be observed during a large index, not merely before it.
- No local throughput claim is made because the machine lacks the user's
  representative archive library.

## Validation

A real ZIP fixture with hundreds of entries will assert that listing completes
with a bounded number of pause-hook calls; the current per-entry implementation
must fail this regression first. A long-name fixture will protect the filename
fallback. Scanner tests will protect chart discovery/cache results and progress
stage order. Completion requires the focused archive/scanner tests, all CTests,
the desktop main build, and the iOS build-only workflow.
