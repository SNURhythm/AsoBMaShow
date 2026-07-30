# Fresh Archive Indexing Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce the fresh archive-index work behind the folder-scanning UI phase while retaining destructive manual rebuild semantics.

**Architecture:** Streamline miniz central-directory classification by amortizing pause polling and eliminating redundant filename/path work, then classify copied archive entries in one scanner pass. Introduce a progress boundary between root enumeration and archive-index completion.

**Tech Stack:** C++23, miniz, libarchive fixtures, CMake/CTest, iOS Xcode build script

## Global Constraints

- `ClearChartMeta()` must continue deleting chart metadata and archive caches.
- Do not change archive scheduling capacity or parsing behavior.
- Do not run a local throughput benchmark because representative archives are unavailable.
- Execute in the existing isolated worktree and self-review without pausing for approval.

---

### Task 1: Bound ZIP index pause-hook overhead

**Files:**
- Modify: `tests/archive_file_concurrency_tests.cpp`
- Modify: `src/ArchiveFile.cpp`

**Interfaces:**
- Consumes: `archive_file::PauseCallback`
- Produces: bounded callback polling during `archive_file::listEntries()` for miniz ZIP indexes.

- [ ] **Step 1: Write the failing real-ZIP regression**

Create a stored ZIP with 513 regular entries, list it with a counting pause
callback, assert all 513 entries are returned, and assert fewer than 16 pause
calls. This fails if the callback is invoked once per central-directory entry.

- [ ] **Step 2: Verify RED**

Run `cmake --build cmake-build-debug --target archive_file_concurrency_tests -j 6 && ./cmake-build-debug/archive_file_concurrency_tests` and require the callback-count assertion to fail against the current implementation.

- [ ] **Step 3: Implement periodic polling**

Add a private 256-item pause-check interval and use it in `listZipEntries()`,
including checks before and after the loop. Keep error text and cleanup behavior
unchanged.

- [ ] **Step 4: Verify GREEN**

Run the same focused command and require exit code 0.

### Task 2: Eliminate redundant ZIP entry work

**Files:**
- Modify: `tests/archive_file_concurrency_tests.cpp`
- Modify: `src/ArchiveFile.cpp`

**Interfaces:**
- Produces: full filename fallback for a `MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE` boundary name.

- [ ] **Step 1: Add a long-name ZIP behavior test**

Write and list a ZIP entry whose path exceeds miniz's embedded stat filename
capacity, then assert the exact full path is returned.

- [ ] **Step 2: Establish the characterization baseline**

Run the focused archive test and require it to pass before refactoring.

- [ ] **Step 3: Refactor the hot loop**

Read file stat before resolving the name, use `stat.m_filename` unless it may
be truncated, and call the full filename API only for the boundary case.
Return `safeEntryPath()`'s normalized `generic_string()` directly. Remove the
miniz-local system filter and retain the common filter. Build exact cache keys
from the stored normalized path without another lexical normalization.

- [ ] **Step 4: Verify behavior after refactoring**

Run the focused archive test and require exit code 0.

### Task 3: Collapse scanner entry classification and split progress

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`
- Modify: `src/ChartLibraryScanner.h`
- Modify: `src/ChartLibraryScanner.cpp`
- Modify: `src/scene/MainMenuScene.cpp`

**Interfaces:**
- Produces: `ChartScanProgressStage::IndexingArchives`

- [ ] **Step 1: Write the failing progress-order test**

Capture a real archive scan's stages and assert `ScanningRoots` is followed by
`IndexingArchives` before `PreparingUpdates`.

- [ ] **Step 2: Verify RED**

Build `chart_library_scanner_tests`; require compilation to fail because the
new stage is absent.

- [ ] **Step 3: Implement the single classification pass and stage**

Merge archive file aggregation and chart-path discovery into one pass. Poll
pause at the same 256-entry interval and once after the pass. Add the enum,
report it after root traversal before waiting for prepared entities, and map it
to `Indexing archives` in the UI.

- [ ] **Step 4: Verify GREEN**

Build and run `chart_library_scanner_tests` and require exit code 0.

### Task 4: Verify, self-review, and publish

**Files:**
- Review: all changed implementation, test, spec, and plan files.

- [ ] **Step 1: Run desktop verification**

Run `cmake --build cmake-build-debug --target main archive_file_concurrency_tests chart_library_scanner_tests -j 6`, both focused test binaries, and `ctest --test-dir cmake-build-debug --output-on-failure`.

- [ ] **Step 2: Run the iOS compile check**

Run `scripts/ios_firebase_deploy.sh --build-only`; do not deploy.

- [ ] **Step 3: Self-review invariants**

Verify destructive manual cache clearing is untouched, long filenames and
unsafe paths remain correct, pause cancellation stays bounded, progress order
is monotonic, and no unrelated worktree changes are included.

- [ ] **Step 4: Commit and update PR #84**

Commit the scoped files, push `perf/dynamic-chart-scan-scheduling`, and confirm
the existing PR includes the commit.
