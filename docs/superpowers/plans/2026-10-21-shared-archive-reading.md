# Shared Archive Reading Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove the measured archive read amplification across shared archive APIs and storage management, without moving audio work into gameplay.

**Architecture:** Reuse validated ZIP directory state through exclusive reader leases, releasing file descriptors while idle and bounding retained metadata. Build filename lookup tables lazily, bound resident index retention, and preserve transient metadata-only visitors. Audit solid extraction and storage-specific readers separately rather than forcing payload readers into metadata-only workflows.

**Tech Stack:** C++23, miniz, libarchive, 7-Zip SDK, SQLite, CMake/CTest.

**Spec:** User request to improve all identified bottlenecks in all archive access paths; evidence in `docs/reviews/2026-10-21-large-archive-read-bottlenecks.md`.

## Global Constraints

- Gameplay audio remains fully resident; no new gameplay reading, decoding, or eviction.
- Archive browsing, measurement, and confirmation read metadata only; sparse 128 GiB payloads must remain untouched.
- Preserve source identity checks, Unicode/alias matching, cancellation, limits, CRC checks, and storage crash recovery.
- No worktrees, whole-file formatting, deployment, or unrelated edits.
- One build per directory; do not run desktop and Android heavy builds concurrently.

## Review Focus

- Source replacement with preserved size/mtime must invalidate both metadata caches.
- Concurrent and reentrant consumers must not share mutable extraction cursors or deadlock.
- Cache pressure must release idle metadata without invalidating active borrowed records.
- Storage confirmation must not build extra filename tables or read local payloads.
- Corrupt, Unicode, oversized and cancelled entries must retain existing rejection behavior.

## Task 1: Shared ZIP directory reuse and indexed single-file access

Files: `src/ArchiveFile.cpp`, `src/ArchiveFile.h`, `tests/archive_file_concurrency_tests.cpp`.

- [x] Extend existing archive tests to count directory initializations across public single, bounded, streaming, concurrent and ranged APIs, including source replacement and cancellation.
- [x] Run the focused archive reader test and establish the regression.
- [x] Introduce exclusive ZIP reader leases with bounded idle directory retention and no idle open file descriptors. Reuse through listing, single and batched reads, concurrent preparation, and unzip preflight/workers.
- [x] Remove the single-file filename-scan-first path in favor of validated indexed lookup, retaining matching and corruption semantics.
- [x] Run archive regression coverage and repeat the sparse read probe.

## Task 2: Index construction and bounded retention, including storage visitors

Files: `src/ArchiveFile.cpp`, `src/ArchiveFile.h`, `tests/archive_file_concurrency_tests.cpp`, storage measurement tests as appropriate.

- [x] Test lazy filename-table construction and bounded index retention with active visitor/source-change coverage.
- [x] Build exact/folded lookup tables only on lookup, not metadata listing/measurement; use synchronized one-time construction.
- [x] Bound retained index metadata and evict least-recently-used cache ownership while preserving active leases and persisted indexes.
- [x] Verify storage measurement/preflight still streams or borrows metadata, with no payload reads and no redundant lookup tables.
- [x] Measure fresh-index time and peak memory on sparse high-entry-count fixtures.

## Task 3: Solid and storage-specific access audit

Files: `src/ArchiveFile.cpp`, `src/audio/ProgressiveMusicSource.cpp`, `src/storage/StorageArchive*.cpp`, relevant existing tests.

- [x] Trace every archive access path and distinguish already-batched direct storage I/O from remaining repeated shared-reader work.
- [x] Measure repeated solid selections versus ordered batch extraction with a bounded fixture and actual read counters.
- [x] Evaluate replay reduction: existing ordered 7z batches already avoid repeated extraction. Preserve staged listening requests; merging all future work would add startup delay and scratch storage. Record the remaining separate-request limitation.
- [x] Extend behavior/byte-count tests for the affected path and verify storage transaction/recovery invariants.

## Task 4: Integration and delivery

- [x] Build/run archive, library/storage, and audio groups; build desktop main. Broaden only for shared impacts not covered by these groups.
- [x] Perform sequential Android compile/device checks for changed native readers where practical.
- [x] Update the report with measured before/after, caller coverage, and any format constraints.
- [x] Independent review, address findings, commit and push to the current upstream branch.
