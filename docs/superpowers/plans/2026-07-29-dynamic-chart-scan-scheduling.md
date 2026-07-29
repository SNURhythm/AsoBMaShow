# Dynamic Chart Scan Scheduling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dynamically share one chart-scan worker budget between ordinary chart parsing and independent archive indexing while preserving ordered SQLite/checkpoint behavior and the existing fast archive-entry pipeline.

**Architecture:** Add a small FIFO work scheduler used only for filesystem/entity preparation. `ChartLibraryScanner` remains the producer and sole state/SQLite owner; workers return sequence-keyed ordinary-chart or archive-inspection results, which are merged in discovery order before the existing archive batch parser runs. Narrow the 7-Zip global cache lock so independent uncached archives can actually open concurrently.

**Tech Stack:** C++23, `std::thread`, mutexes/condition variables, libarchive test fixtures, 7-Zip SDK backend, SQLite, CMake/CTest.

## Global Constraints

- Use one shared worker budget; do not reserve fixed worker counts by entity type.
- Keep SQLite writes, checkpoint writes, progress callbacks, and scan-diff mutation on the scanner thread.
- Preserve the current optimized inner/outer non-solid archive-entry parse pipeline.
- Preserve cancellation, pause, resume, archive-cache, solid-archive, and deterministic discovery-order behavior.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`; parser changes belong in `../bms-parser-cpp`.
- Do not deploy an iOS or Android build. The final local compile check is `cmake --build cmake-build-debug --target main -j 6`.

---

### Task 1: Add the shared FIFO work scheduler

**Files:**
- Create: `src/ChartScanWorkScheduler.h`
- Create: `src/ChartScanWorkScheduler.cpp`
- Create: `tests/chart_scan_work_scheduler_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `chart_scan::WorkScheduler(std::size_t workerCount)`
- Produces: `bool WorkScheduler::enqueue(std::function<void()> work)`
- Produces: `void WorkScheduler::finish()` to close, drain, and join
- Produces: `void WorkScheduler::cancel()` to close, discard queued work, and join
- Produces: `std::vector<std::exception_ptr> WorkScheduler::takeExceptions()`
- Guarantees: accepted work is FIFO, each work item occupies one scheduler worker, shutdown is idempotent, and destruction cannot leave joinable threads

- [ ] **Step 1: Write the failing scheduler concurrency test**

Create `tests/chart_scan_work_scheduler_tests.cpp`. The first task represents a blocked archive index. After it starts, enqueue three ordinary-shaped tasks and prove all three start before the archive task is released:

```cpp
#include "../src/ChartScanWorkScheduler.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

void testLaterEntitiesUseWorkersWhileArchiveIsActive() {
  chart_scan::WorkScheduler scheduler(4);
  std::mutex mutex;
  std::condition_variable cv;
  bool archiveStarted = false;
  bool releaseArchive = false;
  int ordinaryStarted = 0;

  assert(scheduler.enqueue([&] {
    std::unique_lock lock(mutex);
    archiveStarted = true;
    cv.notify_all();
    cv.wait(lock, [&] { return releaseArchive; });
  }));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return archiveStarted; }));
  }

  for (int i = 0; i < 3; ++i) {
    assert(scheduler.enqueue([&] {
      std::lock_guard lock(mutex);
      ++ordinaryStarted;
      cv.notify_all();
    }));
  }
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return ordinaryStarted == 3; }));
    releaseArchive = true;
  }
  cv.notify_all();
  scheduler.finish();
  assert(scheduler.takeExceptions().empty());
}
```

Add tests in the same file for FIFO dequeue order on one worker, exception capture without worker termination, idempotent `finish()`, and `cancel()` discarding queued work after active work is released.

- [ ] **Step 2: Register the test target and verify RED**

Add this target inside the existing test block in `CMakeLists.txt`:

```cmake
add_executable(chart_scan_work_scheduler_tests
    tests/chart_scan_work_scheduler_tests.cpp
    src/ChartScanWorkScheduler.cpp
)
target_include_directories(chart_scan_work_scheduler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(chart_scan_work_scheduler_tests PRIVATE cxx_std_23)
```

Add `chart_scan_work_scheduler_tests` to the registered test target list. Reconfigure and build:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
```

Expected: FAIL because `ChartScanWorkScheduler.h/.cpp` do not exist or the declared API is not implemented.

- [ ] **Step 3: Implement the minimal scheduler**

Declare a non-copyable scheduler with a private worker loop and these fields:

```cpp
namespace chart_scan {
class WorkScheduler {
public:
  using Work = std::function<void()>;
  explicit WorkScheduler(std::size_t workerCount);
  ~WorkScheduler();
  WorkScheduler(const WorkScheduler &) = delete;
  WorkScheduler &operator=(const WorkScheduler &) = delete;
  bool enqueue(Work work);
  void finish();
  void cancel();
  std::vector<std::exception_ptr> takeExceptions();

private:
  void workerLoop();
  void joinWorkers();
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Work> queue_;
  std::vector<std::thread> workers_;
  std::vector<std::exception_ptr> exceptions_;
  bool closed_ = false;
  bool cancelled_ = false;
};
} // namespace chart_scan
```

Start `max(1, workerCount)` threads. Workers wait for cancellation, queued work, or `closed_`; pop from the front; run outside the lock; and capture exceptions under the lock. `finish()` sets `closed_`, wakes, drains, and joins. `cancel()` sets both flags, clears the queue, wakes, and joins. The destructor calls `cancel()`.

Add `ChartScanWorkScheduler.cpp` to the `main` sources in `src/CMakeLists.txt`.

- [ ] **Step 4: Verify GREEN**

```bash
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
ctest --test-dir cmake-build-debug -R '^chart_scan_work_scheduler_tests$' --output-on-failure
```

Expected: PASS with no hangs or uncaught worker exceptions.

- [ ] **Step 5: Commit the scheduler**

```bash
git add src/ChartScanWorkScheduler.h src/ChartScanWorkScheduler.cpp \
  tests/chart_scan_work_scheduler_tests.cpp src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add chart scan work scheduler"
```

---

### Task 2: Allow independent 7-Zip cache misses to open concurrently

**Files:**
- Create: `tests/archive_file_concurrency_tests.cpp`
- Modify: `src/ArchiveFile.cpp:2057-2131`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `archive_file::listEntries(path, entries, error, pauseCallback)`
- Preserves: `gSevenZipArchiveCache`, `kMaxOpenSevenZipArchives`, and per-`SevenZipArchiveState::mutex` serialization
- Changes: `gSevenZipArchiveMutex` protects cache data only, never filesystem/SDK open work

- [ ] **Step 1: Write two real 7-Zip fixtures and the failing lock-boundary test**

Create `tests/archive_file_concurrency_tests.cpp`. Use `ArchiveRAII.h`, `archive_entry.h`, and `archive_write_set_format_7zip()` to write two unique temporary `.7z` archives, each with one regular file.

Call `archive_file::listEntries()` on each path from a separate thread. Each thread owns a pause-callback invocation counter. Synchronize only on the third callback invocation, which occurs from the 7-Zip input stream during `IInArchive::Open`, after the existing global cache lock is acquired:

```cpp
std::mutex barrierMutex;
std::condition_variable barrierCv;
int arrived = 0;
bool timedOut = false;

auto listOne = [&](const std::filesystem::path &path) {
  int pauseCalls = 0;
  std::vector<archive_file::Entry> entries;
  std::string error;
  const bool listed = archive_file::listEntries(
      path, entries, &error, [&] {
        if (++pauseCalls != 3) {
          return true;
        }
        std::unique_lock lock(barrierMutex);
        ++arrived;
        barrierCv.notify_all();
        if (!barrierCv.wait_for(lock, std::chrono::seconds(2),
                                [&] { return arrived == 2; })) {
          timedOut = true;
          barrierCv.notify_all();
        }
        return true;
      });
  assert(listed);
  assert(entries.size() == 1);
};
```

Join both threads and assert `arrived == 2` and `!timedOut`. The timeout is only a deadlock guard; the behavior under test is that both independent opens cross the same in-open barrier.

- [ ] **Step 2: Register and verify RED**

Register `archive_file_concurrency_tests` with `tests/archive_file_concurrency_tests.cpp`, `src/ArchiveFile.cpp`, `src/MinizBridge.c`, `src/bms_parser.cpp`, and `src/path.cpp`; link `${COMMON_LIBS}` and `iconv` on Apple. Add it to the registered-test list.

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target archive_file_concurrency_tests -j 6
ctest --test-dir cmake-build-debug -R '^archive_file_concurrency_tests$' --output-on-failure
```

Expected: FAIL because the first cache miss waits at the barrier while holding `gSevenZipArchiveMutex`, preventing the second archive from reaching its in-open callback.

- [ ] **Step 3: Narrow the global cache critical section**

Refactor `openCachedSevenZipArchive()` into three phases:

1. Read file state, then lock only to return a fresh cached state or erase a stale entry.
2. Release the mutex and perform `openSevenZipArchive(...)`; construct a candidate `SevenZipArchiveState` outside the lock.
3. Re-lock and perform a second fresh-state lookup. If another caller inserted an equivalent state, update its use counter and return it. Otherwise assign the new use counter, insert the candidate, trim the cache, and return it.

The slow portion must have this shape:

```cpp
const auto start = Clock::now();
CMyComPtr<IInArchive> archive;
CMyComPtr<IInStream> stream;
SevenZipFormat formatUsed = SevenZipFormat::SevenZip;
const bool opened =
    requestedFormatId == 0
        ? openSevenZipArchive(archivePath, archive, stream, formatUsed,
                              errorMessage, pauseCallback)
        : openSevenZipArchive(archivePath, requestedFormatId, archive, stream,
                              errorMessage, pauseCallback);
// No gSevenZipArchiveMutex is held above.

auto candidate = std::make_shared<SevenZipArchiveState>();
candidate->size = archiveSize;
candidate->mtime = archiveMtime;
candidate->formatId = static_cast<unsigned char>(formatUsed);
candidate->archive = archive;
candidate->stream = stream;

{
  std::lock_guard cacheLock(gSevenZipArchiveMutex);
  const auto winnerIt = gSevenZipArchiveCache.find(key);
  if (winnerIt != gSevenZipArchiveCache.end() &&
      winnerIt->second != nullptr && winnerIt->second->size == archiveSize &&
      winnerIt->second->mtime == archiveMtime &&
      (requestedFormatId == 0 ||
       winnerIt->second->formatId == requestedFormatId)) {
    winnerIt->second->lastUse = ++gSevenZipArchiveUseCounter;
    return winnerIt->second;
  }
  candidate->lastUse = ++gSevenZipArchiveUseCounter;
  gSevenZipArchiveCache[key] = candidate;
  trimSevenZipArchiveCacheLocked();
}
```

Do not weaken `SevenZipArchiveState::mutex`; operations on one cached SDK handle remain serialized.

- [ ] **Step 4: Verify GREEN and regressions**

```bash
cmake --build cmake-build-debug --target archive_file_concurrency_tests chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug -R '^(archive_file_concurrency_tests|chart_library_scanner_tests)$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Commit the lock-boundary fix**

```bash
git add tests/archive_file_concurrency_tests.cpp src/ArchiveFile.cpp CMakeLists.txt
git commit -m "perf: open independent archives concurrently"
```

---

### Task 3: Schedule ordinary parsing and archive inspection through one queue

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`
- Modify: `src/ChartLibraryScanner.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `chart_scan::WorkScheduler`
- Produces internally: sequence-keyed prepared ordinary-chart and archive-inspection results
- Preserves: `ChartLibraryScanner::Scan(...)` public signature
- Preserves: scanner-thread-only mutation of diffs, cache diffs, checkpoints, progress, and `ScanBatch`

- [ ] **Step 1: Add real mixed-workload scanner fixtures**

Extend `tests/chart_library_scanner_tests.cpp` with a stored-ZIP writer using `ArchiveRAII.h`, `archive_entry.h`, `archive_write_set_format_zip()`, and `zip:compression=store`. Reuse one literal BMS body with distinct titles.

Add `testMixedOrdinaryAndArchiveEntitiesIndexExactlyOnce()`:

```cpp
const auto archiveA = writeZip(root / "00-archive.zip",
                               {{"inside-a.bms", chartText("Archive A")}});
const auto ordinaryA = writeChart(root, "10-ordinary-a", "Ordinary A");
const auto ordinaryB = writeChart(root, "20-ordinary-b", "Ordinary B");
const auto archiveB = writeZip(root / "30-archive.zip",
                               {{"inside-b.bms", chartText("Archive B")}});

assert(scanner.Scan(*session, {archiveA, ordinaryA, ordinaryB, archiveB}) == 4);
assert(session->CountAllChartMeta() == 4);
const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
assert(snapshot.archiveCache.size() == 2);
assert(scanner.Scan(*session, {archiveA, ordinaryA, ordinaryB, archiveB}) == 0);
```

Add `testManySmallArchivesPreserveDiscoveryOrderAndCache()` using exactly six ZIP roots with one chart apiece. Assert six charts, six cache rows, stable second scan, and chart titles/paths matching each archive rather than completion order.

Add the actual concurrency regression `testArchiveInspectionUsesMultipleEntityWorkers()`. Create six stored ZIPs containing only `readme.txt`, so no later archive-entry parser or existing prefetch worker can satisfy the assertion. Capture the calling test thread ID in advance. The pause callback ignores that ID; on worker threads it records distinct IDs and waits at a condition-variable rendezvous until at least two worker IDs have arrived. Skip only when `parallel_worker_count(kFixtureCount) <= 1`. Assert that at least two non-caller worker IDs inspected archives.

- [ ] **Step 2: Establish the TDD boundary**

The mixed and cache fixtures characterize correctness and may already pass on serial code. `testArchiveInspectionUsesMultipleEntityWorkers()` is the scanner-level behavioral RED: current code invokes archive inspection inline on the caller thread, and ZIPs without BMS entries cannot reach the existing parse-prefetch workers. Run the existing scanner test before production edits, then add the fixtures and run again:

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug -R '^chart_library_scanner_tests$' --output-on-failure
```

Expected baseline: PASS. After the fixtures compile, expected: FAIL because the set of non-caller archive-inspection thread IDs is empty. Confirm the failure is this assertion, not fixture creation or parsing.

- [ ] **Step 3: Make archive inspection return data instead of mutating shared state**

Remove the `knownChartPaths` mutable reference from `scanArchiveForChartsOrSolid()`. Keep its local duplicate-entry set and return all virtual chart paths in `ArchiveScanResult`.

Introduce scanner-local prepared records:

```cpp
struct PreparedOrdinaryChart {
  std::filesystem::path path;
  bool parseAttempted = false;
  std::optional<bms_parser::ChartMeta> meta;
};
struct PreparedArchive {
  std::filesystem::path path;
  std::int64_t archiveSize = 0;
  std::int64_t mtimeNs = 0;
  const ArchiveScanCacheRecord *cache = nullptr;
  std::optional<ArchiveScanResult> scan;
};
using PreparedEntity =
    std::variant<PreparedOrdinaryChart, PreparedArchive>;
```

Store `std::optional<PreparedEntity>` slots in discovery order. Each worker writes only its assigned slot under a result mutex. Only the scanner thread merges slots into existing collections.

- [ ] **Step 4: Turn traversal into the shared-queue producer**

Create the scheduler before roots are iterated:

```cpp
const std::size_t entityWorkerCount = static_cast<std::size_t>(
    parallel_worker_count(kIndividualParseBatchSize));
chart_scan::WorkScheduler entityScheduler(entityWorkerCount);
```

For each newly discovered ordinary chart, reserve a slot and enqueue `parseChartMeta(path, nullptr)` on a fresh scan. If `checkpoint.found`, record `parseAttempted = false` without eager parsing.

For each unique archive, do cheap file-state/cache checks on the producer. Resolve valid cache hits immediately. Enqueue stale/missing-cache inspection as one closure calling `scanArchiveForChartsOrSolid(path, pauseCallback)`.

On normal traversal completion call `finish()`; on cancellation call `cancel()`. Log captured unexpected exceptions and merge populated slots in discovery order. During merge:

- add ordinary paths to `knownChartPaths` and append one `ScanDiff` with prepared metadata;
- apply current cache-hit rules unchanged;
- for successful archive scans, update `reindexedArchives`, solid/cache diffs, known paths, and virtual chart diffs once; and
- ignore failed/missing slots without database mutation.

Extend local `ScanDiff`:

```cpp
struct ScanDiff {
  std::filesystem::path path;
  bool deleted = false;
  bool parseAttempted = false;
  std::optional<bms_parser::ChartMeta> preparedMeta;
};
```

- [ ] **Step 5: Reuse ordinary results and remove competing prefetch workers**

Update `parseIndividualChartBatch()` so `parseAttempted` moves its prepared result into the output slot instead of parsing again; an attempted failure remains empty. Checkpoint fallback diffs keep `parseAttempted == false` and use the existing batch parser.

Remove the speculative archive-prefetch queue, workers, join guard, and `prefetchedArchiveResults` branches. Keep `ArchiveParseJobResult` and `activeArchiveParseJobs`. Simplify `queuedArchiveCountFrom()` and `launchArchiveParseJobs()` to count and launch all pending archive batches.

Required phase order:

```text
shared entity queue finishes
  -> prepared ordinary metadata merges in discovery order
  -> archive batches are constructed in discovery order
  -> SQLite applies ordinary results
  -> existing outer/inner archive parser uses full worker budget
  -> SQLite applies archive results and checkpoints
```

- [ ] **Step 6: Build and verify GREEN**

Add `src/ChartScanWorkScheduler.cpp` to `chart_library_scanner_tests`.

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  chart_scan_work_scheduler_tests archive_file_concurrency_tests \
  chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(chart_scan_work_scheduler_tests|archive_file_concurrency_tests|chart_library_scanner_tests)$' \
  --output-on-failure
```

Expected: all PASS with no duplicate chart insertion.

- [ ] **Step 7: Commit the scanner integration**

```bash
git add tests/chart_library_scanner_tests.cpp src/ChartLibraryScanner.cpp CMakeLists.txt
git commit -m "perf: schedule chart scan entities dynamically"
```

---

### Task 4: Regression and build verification

**Files:**
- Modify only if verification exposes a scoped defect in files above.

**Interfaces:**
- Verifies scheduler shutdown, archive backends, public scanner behavior, SQLite/checkpoints, and desktop integration.

- [ ] **Step 1: Repeat focused tests for race sensitivity**

```bash
for run in 1 2 3 4 5; do
  ctest --test-dir cmake-build-debug \
    -R '^(chart_scan_work_scheduler_tests|archive_file_concurrency_tests|chart_library_scanner_tests)$' \
    --output-on-failure || exit 1
done
```

Expected: five clean passes without hangs.

- [ ] **Step 2: Run relevant repository tests**

```bash
ctest --test-dir cmake-build-debug \
  -R '(chart_library_scanner|chart_repository|profile_archive|jukebox_restore)' \
  --output-on-failure
```

Expected: PASS.

- [ ] **Step 3: Run the desktop compile check**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` builds successfully.

- [ ] **Step 4: Review final diff and worktree**

```bash
git diff HEAD~3 --check
git status --short
git log --oneline -4
```

Confirm there are no environment files, build artifacts, parser-amalgamation edits, unrelated changes, or uncommitted implementation files.

- [ ] **Step 5: Commit only verification corrections, if any**

```bash
git add src/ChartScanWorkScheduler.cpp src/ChartScanWorkScheduler.h \
  src/ArchiveFile.cpp src/ChartLibraryScanner.cpp \
  tests/chart_scan_work_scheduler_tests.cpp \
  tests/archive_file_concurrency_tests.cpp \
  tests/chart_library_scanner_tests.cpp CMakeLists.txt src/CMakeLists.txt
git commit -m "fix: stabilize dynamic chart scan scheduling"
```

If no corrections were needed, do not create an empty commit.
