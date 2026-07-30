# Archive Read FIFO Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent later small archive reads from overtaking earlier heavy archive reads.

**Architecture:** Keep CPU and archive-index queues unchanged. Route both public archive-read work classes through one internal FIFO because they share the same admission counter and limit.

**Tech Stack:** C++20, `std::thread`, `std::condition_variable`, CMake/CTest

## Global Constraints

- Preserve dynamic CPU/archive admission behavior.
- Preserve public `WorkClass` values and caller APIs.
- Manual rebuild must continue clearing all index and scan caches.
- Do not claim local throughput results without representative archives.

---

### Task 1: Preserve FIFO Across Archive Read Classes

**Files:**
- Modify: `tests/chart_scan_work_scheduler_tests.cpp`
- Modify: `src/ChartScanWorkScheduler.h`
- Modify: `src/ChartScanWorkScheduler.cpp`

**Interfaces:**
- Consumes: `WorkScheduler::enqueue(Work, WorkClass)`
- Produces: enqueue-ordered execution across `ArchiveRead` and `ArchiveReadHeavy`

- [ ] **Step 1: Write the failing regression test**

Add `testArchiveReadClassesPreserveEnqueueOrder()`. Start a one-worker CPU
task and block it, enqueue a heavy read that records `1`, enqueue a regular
read that records `2`, release the CPU task, call `finish()`, and assert the
literal result `std::vector<int>{1, 2}`.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
```

Expected: the new assertion fails with the observed order `{2, 1}` because
the ordinary archive-read queue currently has strict priority.

- [ ] **Step 3: Implement the minimal scheduler change**

In `WorkScheduler::enqueue`, route both archive-read enum cases to
`archiveReadQueue_`. Remove `heavyArchiveReadQueue_` and all separate pending,
eligibility, cancellation, pop, and completion branches. Continue marking
popped archive-read work as `WorkClass::ArchiveRead`, which retains the shared
`activeArchiveReads_` accounting.

- [ ] **Step 4: Run focused verification and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests archive_file_concurrency_tests chart_library_scanner_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
./cmake-build-debug/archive_file_concurrency_tests
./cmake-build-debug/chart_library_scanner_tests
```

Expected: all focused tests pass.

- [ ] **Step 5: Run full verification**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
scripts/ios_firebase_deploy.sh --build-only
```

Expected: desktop build succeeds, all CTest tests pass, and the iOS build-only
command reports `BUILD SUCCEEDED` without uploading a build.

- [ ] **Step 6: Review and publish**

Run `git diff --check`, review the complete diff, commit only the scoped files,
push `perf/dynamic-chart-scan-scheduling`, and update PR #84 with the trace
evidence and verification results.
