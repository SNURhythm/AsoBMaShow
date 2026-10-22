# Other Archive Reading Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend shared archive optimizations to RAR, 7z, uncompressed TAR and sequential fallback readers, with measured correctness and scale evidence.

**Architecture:** Give SDK consumers exclusive reusable handles; amortize RAR4 bounded extraction over a batch; retain validated TAR header offsets and read selected entries through libarchive at those offsets. Reuse sequential fallback cursors for forward requests where safe. All optimizations live below application callers. Keep decompression dependencies explicit for solid/compressed streams.

**Tech Stack:** C++23, libarchive, unarr, 7-Zip SDK, CMake/CTest.

**Spec:** User approved the format-specific proposal and requested “implement all”. Existing shared ZIP optimization and gameplay-residency constraints remain applicable.

## Global Constraints

- Gameplay PCM stays fully resident; no new reading, decoding or eviction during play.
- No whole-archive payload cache or unbounded scratch, no new format support requirements.
- Validate source generation when reusing state; release idle descriptors when the backend permits suspension.
- Preserve CRC/integrity checks, canonical names, source replacement detection, cancellation and memory limits.
- Storage mutation retains its own physical validation and crash recovery.
- No worktrees, deployment or unrelated edits. One build per directory; heavy desktop and Android builds run sequentially.

## Review Focus

- Reentrant callbacks must never deadlock on a shared SDK cursor.
- Failed/cancelled extraction must not poison later pooled readers.
- Source replacement with identical size/mtime must not reuse stale readers or offsets.
- TAR offsets must work with PAX/GNU metadata and sparse entries, or explicitly fall back safely.
- Compressed-stream reuse must not skip requested earlier entries or retain unbounded decoded payloads.

## Task 1: Exclusive SDK reader reuse

Files: `src/ArchiveFile.cpp`, `src/ArchiveFile.h`, `src/archive/ArchiveSourceAccess.h`, `tests/archive_file_concurrency_tests.cpp`.

- [x] Add directory-open/reuse tests for 7z/RAR5, including concurrent/reentrant reads and source replacement.
- [x] Replace shared locked cached handles with exclusive leases; reuse those leases in RAR5 workers and suitable full-extraction paths.
- [x] Suspend idle source streams and restore their cursor on reacquisition, preserving source leases only while active.
- [x] Bound idle metadata retention and verify failure/cancellation recovery.

## Task 2: Bounded RAR4 batches

Files: `src/ArchiveFile.cpp`, `tests/archive_file_concurrency_tests.cpp`.

- [x] Verify the current per-member open amplification with an open counter.
- [x] Route bounded streaming through one ordered reader per batch, enforcing limits and cancellation as bytes are produced.
- [x] Preserve random-offset validation and CRC checks, and verify nested and cancelled callbacks.
- [x] Avoid private unarr ABI dependencies or whole-payload caching merely to retain a cheap file handle.

## Task 3: Validated TAR offsets

Files: `src/ArchiveFile.cpp`, `src/ArchiveFile.h`, `tests/archive_file_concurrency_tests.cpp`, fixtures/helpers as needed.

- [x] Record libarchive header offsets only for uncompressed TAR; persist a distinguishable backend/version.
- [x] Read selected headers from a bounded source slice, validating path/size through libarchive before extraction.
- [x] Route single, bounded, vector, streaming, ranged and supported concurrent entry points through the shared path.
- [x] Exercise PAX/GNU names, sparse entries, truncation, corrupt/stale indexes, cancellation and limits.
- [x] Measure late-member reads and source bytes using a sparse large TAR with an untouched large member.

## Task 4: Sequential fallback reuse

Files: `src/ArchiveFile.cpp`, `tests/archive_file_concurrency_tests.cpp`.

- [x] Measure repeated forward selections in a compressed TAR.
- [x] Reuse a bounded number of exclusively owned sequential cursors for compatible forward requests; reopen for backward requests or changed sources.
- [x] Preserve codec state and buffered input without caching decompressed payloads; discard failed states.
- [x] Verify source changes, ordering, Unicode, cancellation and bounded read behavior across fallback APIs.

## Task 5: Integration and delivery

- [x] Run focused archive tests during iteration, then build desktop main and affected archive/library/audio groups.
- [x] Measure optimized before/after and byte counts across applicable formats; distinguish existing solid batching from new gains.
- [x] Run Android compile/device checks sequentially; no upload or app-data deletion.
- [x] Independent review, update the report and existing PR, commit and push to current upstream.

## Implementation decisions

- RAR4 keeps one reader per batch, not an idle handle pool: public unarr cannot
  suspend its stream without private ABI coupling.
- Global-PAX TAR uses sequential parsing because later headers inherit context.
- Only gzip/bzip2 cursors are retained between batches. XZ/LZMA/Zstd dictionary
  memory is archive-selected and unavailable for accounting through libarchive;
  those decoders are released after each batch.
- Existing solid-block batching is preserved. Independent solid requests still
  replay required dependencies; no unselected decoded-payload cache was added.
- Results and API coverage: [other archive reading report](../../reviews/2026-10-22-other-archive-reading.md).
