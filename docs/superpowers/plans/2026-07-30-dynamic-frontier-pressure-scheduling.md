# Dynamic Frontier-Pressure Archive Scheduling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent far-future expensive archive reads from consuming every spare reader while retaining useful cost-based speculation across any worker count.

**Architecture:** Keep the scheduler's dual order/cost indexes and CPU-aware archive admission. Classify each dispatched archive read as ordered or speculative, track the normalized predicted work active in each class, and permit another cost-first dispatch only while speculative pressure is below ordered pressure. Ordered result application and all scanner/database behavior remain unchanged.

**Tech Stack:** C++23, `std::map`, `std::multiset`, `std::condition_variable`, CMake/CTest, Xcode/iOS build-only validation.

## Global Constraints

- Work only in `.worktrees/dynamic-chart-scan-scheduling` on `perf/dynamic-chart-scan-scheduling`.
- Do not encode a three-to-one, one-to-one, or any other fixed worker split.
- Preserve the existing shared worker budget, archive I/O ceiling, CPU-aware archive admission, archive-index isolation, non-preemption, and work-conserving behavior.
- Preserve ordered database, cache, progress, and checkpoint mutation.
- Preserve manual rebuild cleanup of `chart_meta`, `solid_archives`, `archive_scan_cache`, and `chart_scan_checkpoint`.
- Keep the current archive read-cost estimator and existing parsing log fields unchanged.
- Do not add or analyze image-load logging as part of parsing performance.
- Do not run a local archive throughput benchmark because this machine lacks the representative archive library.
- Do not claim a performance improvement until repeated representative device logs confirm it.

---

### Task 1: Specify Dynamic Pressure Behavior with Deterministic Scheduler Tests

**Files:**

- Modify: `tests/chart_scan_work_scheduler_tests.cpp`

**Interfaces:**

- Consumes: `WorkScheduler::enqueue(Work, WorkClass, std::size_t archiveOrder, std::size_t archiveReadCost)`
- Produces: regression coverage for pressure-limited speculation and worker-count-independent use of spare readers

- [ ] **Step 1: Add a regression test that queues all candidates before capacity becomes available**

Add this test beside the existing speculative scheduling tests:

```cpp
void testSpeculativePressureCannotConsumeEveryReader() {
  chart_scan::WorkScheduler scheduler(5, 4);
  std::mutex mutex;
  std::condition_variable cv;
  bool releaseArchives = false;
  std::array<bool, 4> releaseCpu{};
  int cpuBlockersStarted = 0;
  bool frontierStarted = false;
  bool near20Started = false;
  bool near30Started = false;
  bool far100Started = false;
  bool far110Started = false;

  auto blockingArchive = [&](bool *started) {
    return [&, started] {
      std::unique_lock lock(mutex);
      *started = true;
      cv.notify_all();
      cv.wait(lock, [&] { return releaseArchives; });
    };
  };

  assert(scheduler.enqueue(blockingArchive(&frontierStarted),
                           chart_scan::WorkClass::ArchiveRead, 10, 10));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return frontierStarted; }));
  }

  for (std::size_t index = 0; index < releaseCpu.size(); ++index) {
    assert(scheduler.enqueue([&, index] {
      std::unique_lock lock(mutex);
      ++cpuBlockersStarted;
      cv.notify_all();
      cv.wait(lock, [&] { return releaseCpu[index]; });
    }));
  }
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return cpuBlockersStarted == 4; }));
  }

  assert(scheduler.enqueue(blockingArchive(&far100Started),
                           chart_scan::WorkClass::ArchiveReadHeavy, 100,
                           1000));
  assert(scheduler.enqueue(blockingArchive(&far110Started),
                           chart_scan::WorkClass::ArchiveReadHeavy, 110,
                           900));
  assert(scheduler.enqueue(blockingArchive(&near20Started),
                           chart_scan::WorkClass::ArchiveRead, 20, 10));
  assert(scheduler.enqueue(blockingArchive(&near30Started),
                           chart_scan::WorkClass::ArchiveRead, 30, 10));

  {
    std::lock_guard lock(mutex);
    std::fill(releaseCpu.begin(), releaseCpu.end(), true);
  }
  cv.notify_all();
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] {
      return far100Started && near20Started && near30Started;
    }));
    assert(!far110Started);
    releaseArchives = true;
  }
  cv.notify_all();

  scheduler.finish();
  assert(far110Started);
  assert(scheduler.takeExceptions().empty());
}
```

This test deliberately enqueues both expensive candidates first. The current unrestricted cost-first implementation starts ranks 100, 110, and 20, so the wait for rank 30 fails. The intended policy starts rank 100 speculatively, observes that its cost has saturated speculative pressure relative to the active frontier, then starts ranks 20 and 30 as ordered work.

- [ ] **Step 2: Add a complementary test proving there is no fixed speculative-reader quota**

Add:

```cpp
void testCheapSpeculationCanFillMultipleReaders() {
  chart_scan::WorkScheduler scheduler(5, 4);
  std::mutex mutex;
  std::condition_variable cv;
  bool releaseArchives = false;
  std::array<bool, 4> releaseCpu{};
  int cpuBlockersStarted = 0;
  bool frontierStarted = false;
  bool nearStarted = false;
  bool far100Started = false;
  bool far110Started = false;
  bool far120Started = false;

  auto blockingArchive = [&](bool *started) {
    return [&, started] {
      std::unique_lock lock(mutex);
      *started = true;
      cv.notify_all();
      cv.wait(lock, [&] { return releaseArchives; });
    };
  };

  assert(scheduler.enqueue(blockingArchive(&frontierStarted),
                           chart_scan::WorkClass::ArchiveRead, 10, 1000));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return frontierStarted; }));
  }

  for (std::size_t index = 0; index < releaseCpu.size(); ++index) {
    assert(scheduler.enqueue([&, index] {
      std::unique_lock lock(mutex);
      ++cpuBlockersStarted;
      cv.notify_all();
      cv.wait(lock, [&] { return releaseCpu[index]; });
    }));
  }
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return cpuBlockersStarted == 4; }));
  }

  assert(scheduler.enqueue(blockingArchive(&nearStarted),
                           chart_scan::WorkClass::ArchiveRead, 20, 1));
  assert(scheduler.enqueue(blockingArchive(&far100Started),
                           chart_scan::WorkClass::ArchiveReadHeavy, 100,
                           300));
  assert(scheduler.enqueue(blockingArchive(&far110Started),
                           chart_scan::WorkClass::ArchiveReadHeavy, 110,
                           200));
  assert(scheduler.enqueue(blockingArchive(&far120Started),
                           chart_scan::WorkClass::ArchiveReadHeavy, 120,
                           100));

  {
    std::lock_guard lock(mutex);
    std::fill(releaseCpu.begin(), releaseCpu.end(), true);
  }
  cv.notify_all();
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] {
      return far100Started && far110Started && far120Started;
    }));
    assert(!nearStarted);
    releaseArchives = true;
  }
  cv.notify_all();

  scheduler.finish();
  assert(nearStarted);
  assert(scheduler.takeExceptions().empty());
}
```

The three speculative costs total 600, below the active ordered pressure of 1000, so all three spare readers may speculate. This would fail if the implementation hid a permanent one-reader speculative lane behind the new terminology.

- [ ] **Step 3: Register both tests**

In `main()`, call them immediately after `testSpeculativeArchiveReadPrefersHigherCost()`:

```cpp
  testSpeculativeArchiveReadPrefersHigherCost();
  testSpeculativePressureCannotConsumeEveryReader();
  testCheapSpeculationCanFillMultipleReaders();
  testNewlyReadyEarlierArchiveBecomesFrontier();
```

- [ ] **Step 4: Build and run the focused test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^chart_scan_work_scheduler_tests$'
```

Expected: the target compiles, but `testSpeculativePressureCannotConsumeEveryReader()` aborts after its two-second wait because the current scheduler starts the second far-future expensive read instead of rank 30. The complementary cheap-speculation test should not be reached in that run.

---

### Task 2: Balance Active Ordered and Speculative Read Pressure

**Files:**

- Modify: `src/ChartScanWorkScheduler.h`
- Modify: `src/ChartScanWorkScheduler.cpp`
- Test: `tests/chart_scan_work_scheduler_tests.cpp`

**Interfaces:**

- Consumes: queued `ArchiveReadWork::archiveReadCost`, the existing order/cost indexes, and `activeArchiveReadOrders_`
- Produces: active ordered/speculative pressure accounting used by `popArchiveReadLocked(WorkItem &item)`

- [ ] **Step 1: Carry each active read's scheduling classification**

Extend `WorkItem` in `src/ChartScanWorkScheduler.h`:

```cpp
struct WorkItem {
  Work work;
  WorkClass workClass = WorkClass::Cpu;
  std::size_t archiveOrder = std::numeric_limits<std::size_t>::max();
  std::size_t archiveReadCost = 1;
  bool archiveReadSpeculative = false;
};
```

Add these members beside `activeArchiveReadOrders_`:

```cpp
std::size_t activeOrderedArchiveReadPressure_ = 0;
std::size_t activeSpeculativeArchiveReadPressure_ = 0;
```

The cost stored in `WorkItem` is normalized to at least one. The boolean records the selection decision at dispatch so completion subtracts from the same total even if queue state changes while the callback runs.

- [ ] **Step 2: Add overflow-safe pressure helpers**

In the anonymous implementation scope at the top of `src/ChartScanWorkScheduler.cpp`, add:

```cpp
namespace {

void addPressure(std::size_t &pressure, std::size_t cost) {
  const auto maximum = std::numeric_limits<std::size_t>::max();
  pressure = cost > maximum - pressure ? maximum : pressure + cost;
}

void removePressure(std::size_t &pressure, std::size_t cost) {
  pressure = cost > pressure ? 0 : pressure - cost;
}

} // namespace
```

`ChartScanWorkScheduler.h` already includes `<limits>`, so no new public include is needed. Add `<limits>` to the `.cpp` only if the implementation does not receive it transitively after includes are reorganized.

- [ ] **Step 3: Replace unrestricted cost-first selection with the pressure gate**

Replace the selection block in `popArchiveReadLocked` with:

```cpp
const auto earliest = archiveReadsByOrder_.begin();
const auto highestCost = archiveReadsByCost_.begin();
const bool needsFrontier =
    activeArchiveReadOrders_.empty() ||
    earliest->second->archiveOrder < *activeArchiveReadOrders_.begin();
const bool canSpeculate =
    !needsFrontier && highestCost->second != earliest->second &&
    highestCost->second->archiveReadCost != 0 &&
    activeSpeculativeArchiveReadPressure_ <
        activeOrderedArchiveReadPressure_;
const ArchiveReadWorkPtr selected =
    canSpeculate ? highestCost->second : earliest->second;

item.work = std::move(selected->work);
item.workClass = WorkClass::ArchiveRead;
item.archiveOrder = selected->archiveOrder;
item.archiveReadCost = std::max<std::size_t>(1, selected->archiveReadCost);
item.archiveReadSpeculative = canSpeculate;
eraseArchiveReadLocked(selected);
return true;
```

The three ordered cases are: no active read, a queued rank earlier than every active rank, and speculation already carrying at least as much predicted work as ordered dispatches. A zero-cost estimate cannot independently trigger speculation, and if the cost and order indexes point to the same record the earliest record remains ordered.

- [ ] **Step 4: Add pressure when an archive callback becomes active**

Extend the existing archive-read activation branch in `workerLoop()`:

```cpp
} else if (item.workClass == WorkClass::ArchiveRead) {
  ++activeArchiveReads_;
  activeArchiveReadOrders_.insert(item.archiveOrder);
  auto &pressure = item.archiveReadSpeculative
                       ? activeSpeculativeArchiveReadPressure_
                       : activeOrderedArchiveReadPressure_;
  addPressure(pressure, item.archiveReadCost);
}
```

This runs under `mutex_`, immediately after selection and before executing the callback, so a second worker sees the first worker's updated pressure.

- [ ] **Step 5: Remove pressure on every archive callback completion path**

Extend the existing archive-read completion branch after erasing one active order:

```cpp
auto &pressure = item.archiveReadSpeculative
                     ? activeSpeculativeArchiveReadPressure_
                     : activeOrderedArchiveReadPressure_;
removePressure(pressure, item.archiveReadCost);
```

Keep this in the common post-callback cleanup that already runs after both success and exception. Do not clear the active pressure members in `cancel()`; active workers own their rank and pressure removal before `joinWorkers()` returns.

- [ ] **Step 6: Build and run the focused scheduler test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^chart_scan_work_scheduler_tests$'
for run in {1..10}; do ./cmake-build-debug/chart_scan_work_scheduler_tests || exit 1; done
```

Expected: the CTest case and all ten direct repetitions pass. Existing tests must continue proving CPU admission, archive-index isolation, frontier-gap priority, equal-cost ordering, cancellation, exceptions, and finish behavior.

- [ ] **Step 7: Review and commit the scheduler change**

Run:

```bash
git diff --check
git diff -- src/ChartScanWorkScheduler.h src/ChartScanWorkScheduler.cpp tests/chart_scan_work_scheduler_tests.cpp
```

Review specifically for pressure updates outside the scheduler mutex, a selected read left in either queued index, pressure assigned from raw zero cost, and pressure cleanup skipped on exceptions.

Commit:

```bash
git add src/ChartScanWorkScheduler.h src/ChartScanWorkScheduler.cpp tests/chart_scan_work_scheduler_tests.cpp
git commit -m "perf: balance archive frontier and speculative work"
```

---

### Task 3: Verify Integration and Publish the Existing Branch

**Files:**

- Inspect: `src/ChartLibraryScanner.cpp`
- Inspect: `src/repositories/ChartRepository.cpp`
- Inspect: `src/repositories/ChartScanStore.cpp`
- Inspect: all files changed from `develop...HEAD`

**Interfaces:**

- Consumes: the green scheduler implementation and the existing scanner enqueue contract
- Produces: desktop/iOS validation evidence and an updated remote branch/PR #84

- [ ] **Step 1: Run focused integration tests**

Run:

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests archive_file_concurrency_tests chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(chart_scan_work_scheduler_tests|archive_file_concurrency_tests|chart_library_scanner_tests)$'
```

Expected: all three cases pass, covering scheduler behavior, concurrent archive access, and ordered scanner publication.

- [ ] **Step 2: Reconfirm manual-rebuild cleanup and scanner scope**

Run:

```bash
sed -n '1240,1290p' src/repositories/ChartRepository.cpp
rg -n "DELETE FROM (chart_meta|solid_archives|archive_scan_cache|chart_scan_checkpoint)" src/repositories/ChartRepository.cpp src/repositories/ChartScanStore.cpp
git diff develop...HEAD -- src/ChartLibraryScanner.cpp src/repositories/ChartRepository.cpp src/repositories/ChartScanStore.cpp
```

Expected: the manual cleanup path still deletes all four required stores, and this task introduces no scanner/database mutation-order change.

- [ ] **Step 3: Run the complete desktop validation**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: the desktop application target builds and the full configured CTest suite passes.

- [ ] **Step 4: Run iOS build-only validation**

Run:

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: project initialization and the iOS compile check finish with `BUILD SUCCEEDED`. This command must not upload a build.

- [ ] **Step 5: Perform the final branch review**

Run:

```bash
git diff --check develop...HEAD
git status --short --branch
git log --oneline --decorate -5
git diff --stat develop...HEAD
```

Inspect the complete scoped diff for a hidden fixed worker allocation, changed CPU admission, changed archive I/O limits, changed parsing logs, image-load instrumentation, or unrelated user work.

- [ ] **Step 6: Push and update PR #84**

Push the isolated branch normally:

```bash
git push origin perf/dynamic-chart-scan-scheduling
```

Update PR #84 with:

- the latest 14.500-second trace diagnosis;
- the correction that the four-worker example is illustrative rather than a fixed allocation;
- the dynamic active-pressure policy;
- the two deterministic scheduler scenarios;
- exact focused, desktop, full CTest, and iOS build-only results; and
- the explicit requirement for repeated representative device logs before claiming a speedup.

- [ ] **Step 7: Verify publication state**

Run:

```bash
git status --short --branch
git rev-parse HEAD
git rev-parse origin/perf/dynamic-chart-scan-scheduling
gh pr view 84 --json number,url,state,headRefName,baseRefName,headRefOid
```

Expected: the worktree is clean, local and remote branch SHAs match, and PR #84 remains open from `perf/dynamic-chart-scan-scheduling` into `develop` at the new commit.
