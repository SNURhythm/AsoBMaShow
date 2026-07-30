# Archive Read Discovery Priority Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Schedule queued archive reads by archive discovery order instead of asynchronous index-completion order.

**Architecture:** Keep the CPU and archive-index FIFOs unchanged. Replace the archive-read deque with an ordered map keyed by discovery order and enqueue sequence, then carry the scanner's existing prepared-entity sequence into both prefetch and fallback archive-read submissions.

**Tech Stack:** C++20, `std::map`, `std::thread`, `std::condition_variable`, CMake/CTest

## Global Constraints

- Later ready archives may run while an earlier archive is still indexing.
- Active archive reads are never preempted.
- Preserve archive admission, CPU priority, cancellation, exception, and finish behavior.
- Preserve FIFO behavior when no discovery order is supplied.
- Preserve ordered database application and checkpoint writes.
- Manual rebuild continues clearing all chart index and scan cache data.
- Do not claim a local throughput result without the representative device archive corpus.

---

### Task 1: Add Ordered Archive-Read Admission

**Files:**
- Modify: `tests/chart_scan_work_scheduler_tests.cpp`
- Modify: `src/ChartScanWorkScheduler.h`
- Modify: `src/ChartScanWorkScheduler.cpp`

**Interfaces:**
- Consumes: `WorkScheduler::enqueue(Work, WorkClass)`
- Produces: `WorkScheduler::enqueue(Work, WorkClass, std::size_t archiveOrder)` with a default maximum order

- [ ] **Step 1: Write the failing scheduler test**

Add `testArchiveReadsPreferLowerDiscoveryOrder()`. Block a one-worker
scheduler with a CPU task, enqueue an archive read that records `20` with
archive order `20`, enqueue another that records `10` with archive order `10`,
release the blocker, call `finish()`, and assert the literal order
`std::vector<int>{10, 20}`.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
```

Expected: compilation fails because `enqueue` does not yet accept the archive
order. This is the missing scheduler contract, not a fixture error.

- [ ] **Step 3: Implement ordered archive-read storage**

Change the enqueue declaration to:

```cpp
bool enqueue(
    Work work, WorkClass workClass = WorkClass::Cpu,
    std::size_t archiveOrder = std::numeric_limits<std::size_t>::max());
```

Replace `std::deque<Work> archiveReadQueue_` with:

```cpp
using ArchiveReadKey = std::pair<std::size_t, std::uint64_t>;
std::map<ArchiveReadKey, Work> archiveReadQueue_;
std::uint64_t nextArchiveReadEnqueueSequence_ = 0;
```

For both archive-read work classes, insert with
`{archiveOrder, nextArchiveReadEnqueueSequence_++}`. Pop and move the first
map entry. Keep the CPU and archive-index code paths unchanged.

- [ ] **Step 4: Run the scheduler test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
```

Expected: all scheduler tests pass, including the existing equal-priority FIFO
test and the new lower-discovery-order test.

### Task 2: Carry Discovery Order Through the Scanner

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`
- Test: `tests/archive_file_concurrency_tests.cpp`

**Interfaces:**
- Consumes: `WorkScheduler::enqueue(Work, WorkClass, std::size_t archiveOrder)`
- Produces: archive prefetch and fallback read jobs submitted with the scanner's prepared-entity sequence

- [ ] **Step 1: Record archive discovery order**

Add `archiveReadOrderByPath` beside `preparedEntities`. Introduce a
`reserveArchivePreparedEntity(const path_t &archiveKey)` lambda that calls
`reservePreparedEntity()`, records the returned sequence by archive key, and
returns it. Use this lambda for both cached and uncached archive entities.

- [ ] **Step 2: Prioritize indexed prefetch reads**

Add `std::size_t archiveOrder` to `IndexedArchivePrefetch`, initialize it from
the `sequence` argument in `prepareArchiveCharts`, and pass it as the third
argument when `queueIndexedArchivePrefetch` calls `entityScheduler.enqueue`.
The held-large-archive path carries the same structure and therefore retains
the same discovery order.

- [ ] **Step 3: Prioritize fallback reads in the same namespace**

Before queuing an unprepared archive batch, look up
`archiveReadOrderByPath[archiveScanKey(batch->archivePath)]`. Pass the stored
sequence to `archiveParseScheduler->enqueue`; use
`std::numeric_limits<std::size_t>::max()` only if the key is unexpectedly
absent.

- [ ] **Step 4: Run focused integration verification**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests archive_file_concurrency_tests chart_library_scanner_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
./cmake-build-debug/archive_file_concurrency_tests
./cmake-build-debug/chart_library_scanner_tests
```

Expected: all focused tests pass, including archive overlap, ordered result
application, cancellation, and scheduler admission coverage.

- [ ] **Step 5: Run full verification**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
scripts/ios_firebase_deploy.sh --build-only
```

Expected: the desktop build succeeds, all 178 CTest cases pass, and the iOS
build-only command reports `BUILD SUCCEEDED` without uploading a build.

- [ ] **Step 6: Review and publish**

Run `git diff --check`, inspect the complete scoped diff, and reconfirm the
manual rebuild deletion statements. Commit the implementation, push
`perf/dynamic-chart-scan-scheduling`, and update PR #84 with the 13.756-second
trace, discovery-order root cause, change summary, and validation evidence.
