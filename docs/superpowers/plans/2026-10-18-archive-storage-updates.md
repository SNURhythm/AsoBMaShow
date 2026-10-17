# Archive Storage Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Optimize selected entries inside supported archives in place, batching each archive without a full-size duplicate.

**Architecture:** A disk-backed archive manifest supplies candidate evidence and format-specific record layout. A durable bounded-write journal installs verified records and compacts each archive once; startup recovery finishes interrupted writes before playback.

**Tech Stack:** C++23, SQLite, existing FFmpeg/audio transcoders, existing archive readers and platform file durability primitives.

**Spec:** `docs/superpowers/specs/2026-10-18-archive-storage-updates-design.md`

## Global Constraints

- Work in the current checkout and branch; no worktree or deployment.
- Preserve format and archive path; no second full archive or extracted tree.
- Bound memory and per-candidate scratch; spill entry metadata to private SQLite.
- Preserve chart/unselected content and reject unsafe or unsupported structures.
- Reserve and refresh each physical archive once for the selected-folder union.
- Recover durable in-place operations before reopening playback/library readers.

## Review Focus

- ZIP64 offsets and data descriptors must survive compaction and renaming.
- Ambiguous, legacy-encoded or unsafe names must not target a different entry.
- Crash after an overlapping write must resume without reading overwritten input.
- Cancellation/codec failure must still leave previously committed entries readable.
- Selecting two folders from one large archive must not duplicate scanning, staging or compaction.

### Task 1: Archive manifests and capability checks

**Files:** new `src/storage/StorageArchive.h`, `StorageArchive.cpp`, `StorageArchiveIO.h`, `StorageArchiveIO.cpp`; `src/storage/CMakeLists.txt`, `cmake/StorageManagementTests.cmake`, `cmake/TestGroupCatalog.cmake`; new `tests/storage_archive_tests.cpp`.

**Interfaces:** `StorageArchive` owns a bounded-cache SQLite entry manifest and a pinned local file handle. Its open/visit/find/extract operations expose normalized entry paths, raw record ranges, logical sizes and collision information without an all-entry vector. Capability errors are `StorageDiagnostic` values. ZIP and tar adapters share manifest and IO.

- [x] Add failing round-trip inventory/extraction tests for stored/deflated ZIP, ZIP64 sparse offsets, Unicode/CP932 and tar; reject unsupported/unsafe layouts.
- [x] Implement checked format parsing, streamed extraction/CRC verification, spill-backed lookup and immutable source identity evidence.
- [x] Run `storage_archive_tests`; verify entry bytes against existing readers and bounded manifest memory.

### Task 2: Durable bounded in-place writes

**Files:** new `src/storage/StorageArchiveTransaction.h`, `StorageArchiveTransaction.cpp`; extend `tests/storage_archive_tests.cpp`.

**Interfaces:** `StorageArchiveTransaction` accepts verified replacement records/deletions, persists manifest edits with redo evidence, finalizes the container once, and exposes `RecoverStorageArchives(StoragePlatformPaths, Checkpoint)` returning `StorageReport`. Test dependencies expose fault points and scratch/compaction metrics.

- [x] Add failing tests for replacement plus deletion, one compaction, tight scratch limits, identity changes and faults around journal sync, archive write, cursor advance and truncation.
- [x] Implement redo replay, bounded forward compaction with persisted chunk hashes, metadata regeneration, source authority checks and owned-journal cleanup.
- [x] Run the archive tests and verify every recovered result through the existing reader, including unchanged-entry hashes and no full-size temporary file.

### Task 3: Eligibility and optimizer integration

**Files:** `StoragePreflight.h/.cpp`, `StorageOptimizer.h/.cpp`, `StorageApplicationIntegration.cpp`, `StorageManagementScene.cpp`, `StorageMeasurementState.cpp`, archive tests and existing storage tests.

**Interfaces:** compact per-archive preflight evidence records physical identity, selected inner roots, policy, counts and logical group totals. `EligibleRoots()` returns physical archive paths. `StorageOptimizer` batches each manifest once, uses existing encoders on bounded per-entry staging, finalizes on cancellation, and reports physical bytes reclaimed plus a physical changed root.

- [x] Add failing tests for overlapping/two-folder archive selections, WAV/FLAC policy counts, video deletion, untouched sibling/chart/image data, failed encode, cancellation, stale preflight and unsupported archive reasons.
- [x] Integrate archive preflight with existing local-directory scanning and retain aggregate UI reporting.
- [x] Wire full-archive admission, startup recovery and one scoped refresh; invalidate all affected archive measurements.
- [x] Run affected storage/archive/settings lifecycle/localization checks.

### Task 4: Documentation and platform verification

**Files:** `docs/storage-management.md`, current design/plan, localized messages and platform source registration if required.

- [x] Update supported-format, extra-space, recovery and cancellation behavior without promising whole-archive rollback.
- [x] Run `cmake --build cmake-build-debug --target main -j 6` and focused CTest groups.
- [x] Run `scripts/ios_firebase_deploy.sh --build-only` (no upload); fix platform findings.
- [x] Review the completed diff, commit task-related changes and push the current upstream.
