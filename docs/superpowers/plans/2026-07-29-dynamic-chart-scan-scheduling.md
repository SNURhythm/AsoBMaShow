# Dynamic Pipelined Chart Scan Scheduling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace unbounded concurrent archive indexing and nested archive parser thread groups with a dynamic shared-pool pipeline that overlaps archive I/O and chart parsing without regressing archive-only throughput.

**Architecture:** Extend `ChartScanWorkScheduler` with separate constant-time CPU/archive queues, dynamic archive-I/O admission, and finish-time child-task draining. Admission contracts to one archive operation while CPU work is queued and expands to at most four for archive-only work, capped at one fewer than the worker count. Fresh archive inspection streams chart bytes into CPU tasks on that scheduler, while one-chart archives parse inline; the scanner thread later consumes ordered prepared metadata. Checkpoint/fallback archive entries use the same pipeline in a sequential second scheduler, and all database/checkpoint writes remain on the scanner thread.

**Tech Stack:** C++23, `std::thread`, mutexes/condition variables, libarchive synthetic ZIP fixtures, SQLite, CMake/CTest.

## Global Constraints

- The scanner uses the existing `parallel_worker_count()` budget.
- Archive-I/O admission contracts to one while CPU work is queued and expands to at most four for archive-only work; at least one worker is reserved whenever the total worker count is greater than one.
- CPU and archive work use separate FIFO queues so admission checks and dispatch do not scan a large mixed backlog under the scheduler mutex.
- One-chart archives and single-worker scans parse inline.
- Larger archives stream into the shared pool with limits of 12 files and 16 MiB per reader.
- One remaining random-access archive with at least 16 charts uses the archive backend's concurrent reader as the sole active pool and falls back to the shared pipeline when unsupported.
- SQLite, checkpoints, progress callbacks, and scan-diff mutation stay on the scanner thread.
- Preserve deterministic discovery and archive-entry order, pause/stop behavior, cache semantics, and solid-archive handling.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not benchmark or deploy. Verify desktop compilation with `cmake --build cmake-build-debug --target main -j 6`.

---

### Task 1: Make the scheduler resource-aware and continuation-safe

**Files:**
- Modify: `src/ChartScanWorkScheduler.h`
- Modify: `src/ChartScanWorkScheduler.cpp`
- Modify: `tests/chart_scan_work_scheduler_tests.cpp`

**Interfaces:**
- Produces: `enum class WorkClass { Cpu, ArchiveIndex, ArchiveRead, ArchiveReadHeavy }`
- Produces: `WorkScheduler(std::size_t workerCount, std::size_t archiveIoLimit = 4)`
- Produces: `bool enqueue(Work work, WorkClass workClass = WorkClass::Cpu)`
- Preserves: `finish()`, `cancel()`, exception capture, non-copyability, and idempotent joining
- Guarantees: `finish()` drains child work enqueued by active tasks; queued archive work that has reached the admission cap does not block eligible CPU work

- [ ] **Step 1: Write the archive-admission test**

Add a four-worker test that enqueues four blocking `ArchiveRead` tasks with an admission limit of two. Wait until two archive tasks are active, enqueue two CPU tasks, and assert both CPU tasks start while the first archive pair remains blocked. Also assert the observed maximum number of active archive tasks is exactly two.

- [ ] **Step 2: Write the finish-time child-task test**

Enqueue a root CPU task that waits until a separate thread has called `finish()`, then enqueues a child CPU task and returns. Assert `finish()` joins only after the child runs and that a later external enqueue is rejected.

- [ ] **Step 3: Verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
```

Expected: compilation fails because `WorkClass` and the archive limit do not exist, and the current `finish()` rejects child work after closing the queue.

- [ ] **Step 4: Implement eligible-task selection and quiescent finish**

Store CPU, archive-index, ordinary archive-read, and heavy archive-read work in separate FIFO queues. Track independent index/read counters plus `activeTasks_`, `archiveIoLimit_`, `finishing_`, `closed_`, and `cancelled_`. When CPU work is queued, admit one short index task and one total archive reader so metadata continues progressing while remaining workers parse. Serialize heavy multi-chart reads; when the CPU queue is empty, expand ordinary archive reads and indexes to `archiveIoLimit_`. Increment active counters before unlocking and decrement them after work/exception handling.

`finish()` sets `finishing_`, wakes workers, and joins. Workers exit only when `finishing_ && queue_.empty() && activeTasks_ == 0`. `enqueue()` remains valid while finishing is in progress and rejects work only after `closed_` or `cancelled_`. The worker that observes quiescence sets `closed_` and wakes all workers. `cancel()` marks the pool closed/cancelled and clears queued work.

- [ ] **Step 5: Verify GREEN and commit**

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
ctest --test-dir cmake-build-debug -R '^chart_scan_work_scheduler_tests$' --output-on-failure
git add src/ChartScanWorkScheduler.h src/ChartScanWorkScheduler.cpp tests/chart_scan_work_scheduler_tests.cpp
git commit -m "feat: bound archive work in chart scan scheduler"
```

---

### Task 2: Characterize prepared archive metadata flow

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `ChartLibraryScanner::Scan()`
- Verifies: one-chart and multi-chart synthetic stored ZIPs produce stable chart and archive-cache rows on first scan and no changes on the second scan

- [ ] **Step 1: Add a multi-entry archive correctness test**

Create a stored ZIP containing three BMS files in distinct inner folders and a non-chart file. Scan the explicit archive root, assert three chart rows and one archive cache row with `chartCount == 3`, assert each stored virtual path contains the expected inner path, then scan again and assert zero changes.

- [ ] **Step 2: Verify the characterization is green before refactoring**

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug -R '^chart_library_scanner_tests$' --output-on-failure
```

Expected: PASS. This protects archive ordering/cache behavior while the production threading model changes; the scheduler tests provide the failing behavioral coverage for the concurrency change.

- [ ] **Step 3: Commit the characterization**

```bash
git add tests/chart_library_scanner_tests.cpp
git commit -m "test: characterize pipelined archive scan results"
```

---

### Task 3: Pipeline fresh archive parsing through the entity scheduler

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`

**Interfaces:**
- Extends local `PreparedArchive` with ordered prepared chart results
- Extends local `ArchiveParseBatch` with per-entry `parseAttempted` and prepared metadata
- Adds a local archive-stream helper that consumes `ArchiveParseBatch`, a scheduler, and an ordered result sink

- [ ] **Step 1: Add ordered prepared-result fields**

Represent each archive entry as an inner path plus `parseAttempted` and `optional<ChartMeta>`. When merging discovery results, move matching eager metadata into the archive batch. An attempted parse with empty metadata must remain attempted so malformed charts are not parsed twice.

- [ ] **Step 2: Implement the bounded archive producer**

For one entry, or when the shared worker budget is one, call `readArchiveEntriesStreaming()` and `parseChartMeta()` inline from an `ArchiveRead` task. For larger batches, stream from the `ArchiveRead` task and enqueue one `Cpu` task per file. Use a fixed result vector keyed by normalized inner path and condition-variable backpressure for 12 files/16 MiB. Each CPU task releases its byte/file charge after parsing. A shared completion record publishes only after the producer is done and all accepted CPU tasks have completed.

- [ ] **Step 3: Invoke the producer immediately after archive indexing**

Construct the entity scheduler with `archiveIoLimit = min(4, workerCount > 1 ? workerCount - 1 : 1)`. Archive inspection tasks use `WorkClass::ArchiveIndex`; streaming producers use `WorkClass::ArchiveRead`. On fresh scans, a readable, non-solid archive publishes its `PreparedArchive` after indexing and independently queues the producer. Cached, solid, unreadable, and checkpoint-bearing cases retain their existing cheap/index-only paths.

- [ ] **Step 4: Consume prepared metadata without rereading**

During ordered diff-to-batch conversion, carry prepared metadata into the matching archive entry. The later archive application path builds an ordered `ArchiveParseJobResult` directly when every pending entry was already attempted.

- [ ] **Step 5: Verify fresh-scan tests**

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug -R '^chart_library_scanner_tests$' --output-on-failure
```

Expected: all mixed, many-small-archive, multi-entry, stop/pause, cache, and storage tests pass.

---

### Task 4: Remove nested archive parser pools from fallback/resume work

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`

**Interfaces:**
- Removes: `ArchiveParsePipelineShape`, outer `std::async` archive jobs, and per-archive producer/parser thread vectors
- Reuses: the Task 3 bounded archive producer
- Preserves: ordered `ArchiveParseJobResult` consumption and checkpoint application

- [ ] **Step 1: Submit only unprepared archive suffixes**

After checkpoint validation, keep using the resource-aware scheduler and its bounded archive admission. For each archive at or after the resume position, copy only entries whose parse was not attempted into a pending batch and submit one `ArchiveRead` producer. Store results by archive index; prepared prefixes stay in their existing ordered slots.

Before creating that scheduler, give one remaining batch of at least 16 charts to `readArchiveEntriesConcurrently()` with the full worker budget. This call runs only after the entity pool has joined, and its callbacks parse metadata on the backend workers. If the backend rejects parallel reading, submit the batch to the shared bounded pipeline instead.

- [ ] **Step 2: Drain once and apply in archive order**

Finish the scheduler after all archive roots have been submitted. Merge newly parsed suffix results with any eager results, then run the existing single scanner-thread insert/cache/checkpoint loop in `archiveBatchOrder`. Failed reads produce an empty optional result and retain current logging behavior.

- [ ] **Step 3: Delete obsolete nested-concurrency code**

Remove `parseArchiveBatchConcurrently()`, `parseArchiveBatchStreaming()`'s private worker/producer threads, `ArchiveParsePipelineShape`, `ActiveArchiveParseJob`, `std::future`, `std::async`, launch/take/wait helpers, and unused `<future>` includes/constants.

- [ ] **Step 4: Verify scanner and scheduler tests and commit**

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug -R '^(chart_scan_work_scheduler_tests|chart_library_scanner_tests)$' --output-on-failure
git add src/ChartLibraryScanner.cpp
git commit -m "perf: pipeline archive chart parsing"
```

---

### Task 5: Regression verification and publication

**Files:**
- Review: every file changed since `origin/develop`
- Update: the existing GitHub pull request description if its architecture summary is stale

**Interfaces:**
- Verifies: scheduler, scanner, archive backend, desktop compile, clean worktree

- [ ] **Step 1: Run focused and related tests**

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests archive_file_concurrency_tests chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug -R '^(chart_scan_work_scheduler_tests|archive_file_concurrency_tests|chart_library_scanner_tests)$' --output-on-failure
```

- [ ] **Step 2: Run the required desktop build**

```bash
cmake --build cmake-build-debug --target main -j 6
```

- [ ] **Step 3: Self-review**

Run `git diff --check`, inspect the complete branch diff, confirm no nested archive parser thread creation remains in `ChartLibraryScanner.cpp`, confirm all worker-owned state outlives scheduler joins, and confirm no benchmark or deployment command was run.

- [ ] **Step 4: Push and update the pull request**

```bash
git push origin perf/dynamic-chart-scan-scheduling
gh pr edit 84 --body-file /tmp/asobmashow-pr-84.md
```

Report the tests/build run and the pull request URL without claiming measured speedup.
