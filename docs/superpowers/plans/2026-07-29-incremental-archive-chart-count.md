# Incremental Archive Chart Count Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove per-archive full-table chart recounts while preserving exact archive-cache counts across invalid charts and checkpoint resume.

**Architecture:** Reuse the scanner's successful insertion count for normal archive cache writes. Initialize that count from the repository only when resuming inside an archive, where a committed prefix already exists.

**Tech Stack:** C++20, SQLite authorization callbacks, libarchive ZIP fixtures, CMake/CTest-style test executables.

## Global Constraints

- Preserve deterministic archive discovery order and scanner-thread SQLite writes.
- Preserve cache completeness checks and checkpoint behavior.
- Do not change archive worker admission, parser behavior, or database schema.
- Do not run a local performance benchmark because this machine has no representative archive library.

---

### Task 1: Add scanner count-reuse regressions

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `ChartLibraryScanner::Scan`, SQLite's authorizer callback, `ChartScanSnapshot::archiveCache`, and existing stored-ZIP fixture helpers.
- Produces: regression coverage that rejects post-insert `chart_meta.path` reads during a normal scan and preserves exact valid counts after a mid-archive resume.

- [ ] **Step 1: Add a SQLite authorizer fixture for post-insert path reads**

Add an auto-extension-backed authorizer that marks the first `SQLITE_INSERT` on `chart_meta` and counts subsequent `SQLITE_READ` authorization requests for `chart_meta.path`. Keep its state in test-only atomics and reset the SQLite auto-extension in the fixture destructor.

- [ ] **Step 2: Add the normal-scan regression**

Create several real one-chart ZIP archives, enable the authorizer immediately before `Scan`, and assert that the post-insert path-read count is zero. Then verify that every archive cache record has `chartCount == 1`.

- [ ] **Step 3: Add the mid-archive resume characterization**

Create a 105-entry ZIP whose first 100 candidates contain one invalid chart. Return flush request `0` for the archive-phase boundary and `1` at the 100-entry checkpoint; request stop when request `1` completes. Assert 99 stored rows and `subIndex == 100`, resume, then assert 104 stored rows, cache `chartCount == 104`, and no checkpoint.

- [ ] **Step 4: Run the scanner test and verify RED**

Run: `cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6 && ./cmake-build-debug/chart_library_scanner_tests`

Expected: FAIL in the normal-scan regression because `CountChartsInArchive` reads `chart_meta.path` after insertion begins.

### Task 2: Reuse successful archive insert counts

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `ArchiveParseJobResult::innerStart`, `ScanBatch::UpsertChart`, and `ScanBatch::CountChartsInArchive`.
- Produces: `writePendingArchiveCache(const ArchiveParseBatch &, int parsedChartCount)` with a caller-supplied exact count.

- [ ] **Step 1: Change the cache writer to accept an exact count**

Replace the unconditional repository recount inside `writePendingArchiveCache` with an integer parameter. Preserve the candidate-versus-valid diagnostic and pass the supplied count to `UpsertArchiveCache`.

- [ ] **Step 2: Initialize the per-archive stored count**

For an archive beginning at entry zero, initialize the count to zero. When `innerStart > 0`, initialize it once with `scanBatch->CountChartsInArchive(batch.archivePath)` to include the checkpointed prefix.

- [ ] **Step 3: Accumulate successful suffix inserts**

Increment the stored count only when `UpsertChart` returns true. Pass the accumulated count to the cache writer only when the full ordered batch completes. For the already-completed resume branch, use the one recovery count because no suffix is inserted.

- [ ] **Step 4: Run the scanner test and verify GREEN**

Run: `cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6 && ./cmake-build-debug/chart_library_scanner_tests`

Expected: PASS, including zero post-insert path reads in a normal scan and exact 104-row cache state after resume.

### Task 3: Verify, self-review, and publish

**Files:**
- Review: `src/ChartLibraryScanner.cpp`
- Review: `tests/chart_library_scanner_tests.cpp`
- Review: `docs/superpowers/specs/2026-07-29-incremental-archive-chart-count-design.md`
- Review: `docs/superpowers/plans/2026-07-29-incremental-archive-chart-count.md`

**Interfaces:**
- Consumes: the completed implementation and existing build targets.
- Produces: a verified commit pushed to PR #84.

- [ ] **Step 1: Run focused verification**

Run:

```bash
cmake --build cmake-build-debug --target platform_document_handoff_tests chart_scan_work_scheduler_tests archive_file_concurrency_tests chart_library_scanner_tests main -j 6
./cmake-build-debug/platform_document_handoff_tests
./cmake-build-debug/chart_scan_work_scheduler_tests
./cmake-build-debug/archive_file_concurrency_tests
./cmake-build-debug/chart_library_scanner_tests
git diff --check
```

Expected: every command exits zero.

- [ ] **Step 2: Self-review the committed scope**

Inspect the branch diff against `origin/perf/dynamic-chart-scan-scheduling` and confirm that normal cache writes have no repository recount, mid-archive resume performs at most one recount, invalid charts are excluded, and unrelated code is unchanged.

- [ ] **Step 3: Commit and push**

Commit the documentation separately, then commit the test and implementation with message `perf: reuse archive chart insert counts`. Push `perf/dynamic-chart-scan-scheduling` and update PR #84 with the trace evidence, design, and verification results.
