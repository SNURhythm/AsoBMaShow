# Hybrid Archive Read Scheduling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce manual-rebuild wall time by keeping the earliest archive on the ordered commit path moving while using otherwise-idle archive readers for the most expensive-looking later archives.

**Architecture:** Keep the existing CPU-aware admission policy and ordered database application. Represent every queued archive read once, index it both by discovery order and by estimated read cost, and track the discovery ranks of active archive reads. Dispatch the earliest queued rank when it is earlier than every active rank; otherwise dispatch the highest-cost queued read speculatively. Estimate cost from the highest accepted chart-entry ordinal found during archive indexing and propagate it through eager and checkpoint-fallback prefetch paths.

**Tech Stack:** C++17, `std::map`, `std::multiset`, CMake/CTest, libarchive-backed scanner tests, Xcode/iOS build-only validation.

## Global Constraints

- [ ] Work only in `.worktrees/dynamic-chart-scan-scheduling` on `perf/dynamic-chart-scan-scheduling`.
- [ ] Preserve ordered database, cache, progress, and checkpoint mutation.
- [ ] Preserve manual rebuild cleanup of `chart_meta`, `solid_archives`, `archive_scan_cache`, and `chart_scan_checkpoint`.
- [ ] Preserve the existing work-conserving CPU rule: a nonempty CPU queue contracts archive admission to one.
- [ ] Do not add per-entry performance logging; extend the existing archive-prefetch lines only.
- [ ] Do not claim a device performance improvement until a representative device log confirms it.
- [ ] Do not run a local performance benchmark because this machine has no representative archive corpus.

## Task 1: Implement Frontier/Speculative Archive Dispatch

**Files:**

- Modify: `src/ChartScanWorkScheduler.h`
- Modify: `src/ChartScanWorkScheduler.cpp`
- Test: `tests/chart_scan_work_scheduler_tests.cpp`

### 1.1 Add failing scheduler tests

- [ ] Add `testSpeculativeArchiveReadPrefersHigherCost()` using three workers and archive limit two.

The test must make dispatch deterministic:

1. Start archive rank 10 and block it. This is the active frontier read.
2. Start a CPU blocker and block it, occupying the third worker.
3. Queue archive rank 20/cost 2 and rank 30/cost 100.
4. Release only the CPU blocker.
5. Assert rank 30 starts while rank 20 remains queued.
6. Release all archive work, finish, and assert no scheduler exceptions.

Use the four-argument API that does not exist yet:

```cpp
assert(scheduler.enqueue(expensiveWork, chart_scan::WorkClass::ArchiveReadHeavy,
                         30, 100));
```

Using `ArchiveReadHeavy` for the expensive speculative candidate also proves
that both archive read classes share the new policy.

- [ ] Add `testNewlyReadyEarlierArchiveBecomesFrontier()` using three workers and archive limit two.

The test must:

1. Start rank 30/cost 10 and block it.
2. Start a CPU blocker and block it.
3. Queue rank 40/cost 1000, then rank 10/cost 1.
4. Release the CPU blocker.
5. Assert rank 10 starts before rank 40 because it is earlier than every active rank.
6. Release all work, finish, and assert no scheduler exceptions.

- [ ] Register both tests in the test executable's `main()`.

- [ ] Keep `testArchiveReadClassesPreserveEnqueueOrder()` and
  `testArchiveReadsPreferLowerDiscoveryOrder()` passing; together they cover
  equal/default-cost FIFO behavior and the no-active-frontier order path.

- [ ] Run the focused scheduler test build and confirm RED because `WorkScheduler::enqueue` accepts only three arguments:

```sh
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
```

Expected failure: compilation reports no matching four-argument `enqueue` overload.

### 1.2 Add a shared queued-read record and dual indexes

- [ ] Extend the public API in `src/ChartScanWorkScheduler.h`:

```cpp
bool enqueue(
    Work work, WorkClass workClass = WorkClass::Cpu,
    std::size_t archiveOrder = std::numeric_limits<std::size_t>::max(),
    std::size_t archiveReadCost = 0);
```

- [ ] Add `<memory>`, `<set>`, and `<tuple>` includes.

- [ ] Extend `WorkItem` so an active archive read retains its discovery rank:

```cpp
struct WorkItem {
  Work work;
  WorkClass workClass = WorkClass::Cpu;
  std::size_t archiveOrder = std::numeric_limits<std::size_t>::max();
};
```

- [ ] Replace the single archive-read map with a record indexed in two ways:

```cpp
struct ArchiveReadWork {
  Work work;
  std::size_t archiveOrder = std::numeric_limits<std::size_t>::max();
  std::size_t archiveReadCost = 0;
  std::uint64_t enqueueSequence = 0;
};

using ArchiveReadWorkPtr = std::shared_ptr<ArchiveReadWork>;
using ArchiveReadOrderKey = std::pair<std::size_t, std::uint64_t>;
using ArchiveReadCostKey =
    std::tuple<std::size_t, std::size_t, std::uint64_t>;

struct ArchiveReadCostKeyLess {
  bool operator()(const ArchiveReadCostKey &left,
                  const ArchiveReadCostKey &right) const;
};

std::map<ArchiveReadOrderKey, ArchiveReadWorkPtr> archiveReadsByOrder_;
std::map<ArchiveReadCostKey, ArchiveReadWorkPtr, ArchiveReadCostKeyLess>
    archiveReadsByCost_;
std::multiset<std::size_t> activeArchiveReadOrders_;
```

The cost comparator must sort cost descending, then discovery order ascending, then enqueue sequence ascending. Both maps must point to the same immutable scheduling record.

- [ ] Add private helpers:

```cpp
bool popArchiveReadLocked(WorkItem &item);
void eraseArchiveReadLocked(const ArchiveReadWorkPtr &read);
```

### 1.3 Implement hybrid dispatch

- [ ] In `enqueue`, construct one `ArchiveReadWork`, then insert it into both indexes under:

```cpp
ArchiveReadOrderKey{archiveOrder, sequence}
ArchiveReadCostKey{archiveReadCost, archiveOrder, sequence}
```

- [ ] Make `archiveReadsByOrder_` authoritative for pending-work and read-eligibility checks.

- [ ] Implement `popArchiveReadLocked` with this exact policy:

```cpp
const auto earliest = archiveReadsByOrder_.begin();
const bool needsFrontier =
    activeArchiveReadOrders_.empty() ||
    earliest->second->archiveOrder < *activeArchiveReadOrders_.begin();
const auto selected = needsFrontier
    ? earliest->second
    : archiveReadsByCost_.begin()->second;
```

Move the selected callable into `WorkItem`, copy its rank into `WorkItem::archiveOrder`, then erase the record from both indexes. Equality does not create another frontier; with the same rank already active, spare capacity uses cost priority.

- [ ] Replace both direct `archiveReadQueue_.begin()` pop sites with `popArchiveReadLocked(item)`.

- [ ] When an archive read becomes active, increment `activeArchiveReads_` and insert `item.archiveOrder` into `activeArchiveReadOrders_` while holding the scheduler mutex.

- [ ] When it completes, erase exactly one matching rank from `activeArchiveReadOrders_` and decrement `activeArchiveReads_` while holding the mutex. Assert or defensively guard that the rank exists.

- [ ] In `cancel`, clear both queued indexes. Do not clear active ranks before running workers exit; workers own removal of their ranks.

- [ ] Keep `ArchiveReadHeavy` normalized to the same active `ArchiveRead` class as today.

### 1.4 Verify and commit Task 1

- [ ] Build and run the scheduler tests:

```sh
cmake --build cmake-build-debug --target chart_scan_work_scheduler_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R chart_scan_work_scheduler_tests
```

- [ ] Repeat the test executable several times to catch synchronization flakiness:

```sh
for run in {1..10}; do ./cmake-build-debug/chart_scan_work_scheduler_tests || exit 1; done
```

- [ ] Review the diff for duplicate ownership, stale index entries, incorrect active-rank cleanup, and regression of CPU admission.

- [ ] Commit:

```sh
git add src/ChartScanWorkScheduler.h src/ChartScanWorkScheduler.cpp tests/chart_scan_work_scheduler_tests.cpp
git commit -m "perf: schedule speculative archive reads by cost"
```

## Task 2: Derive and Propagate Archive Read Cost

**Files:**

- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`

### 2.1 Add failing scanner coverage

- [ ] Add `testArchiveReadCostUsesLastChartEntryOrdinal()`.

Create a ZIP with entries in this order:

```text
readme.txt
first.bms
padding.bin
second.bms
trailing.bin
```

Run a manual scan and assert the existing bounded-prefetch log for that archive contains:

```text
requested=2 discoveryOrder=0 readCost=4
```

The requested count proves only accepted chart paths are read; `readCost=4` proves the estimate uses the one-based ordinal of the last accepted chart entry, not entry count or archive size. Use a unique archive filename and `hasArchiveLog` so unrelated global debug-log entries cannot satisfy the assertion.

- [ ] Extend `testArchiveCheckpointResumeUsesOrderedFallbackPipeline()` to require its deferred-read log to contain:

```text
requested=3 discoveryOrder=0 readCost=3
```

This covers propagation through the checkpoint fallback path rather than only eager indexed prefetch.

- [ ] Register the new test in `main()`.

- [ ] Run the focused scanner test and confirm RED because current prefetch logging has no `readCost=` field:

```sh
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R chart_library_scanner_tests
```

Expected failure: the new log assertion is false.

### 2.2 Compute the estimator during the existing index scan

- [ ] Add this field to `ArchiveScanResult`:

```cpp
std::size_t readCost = 0;
```

- [ ] Iterate archive entries with an explicit `entryIndex`. Whenever an entry passes chart-extension filtering and is newly accepted after normalized-path deduplication, set:

```cpp
result.readCost = entryIndex + 1;
```

Do not update the cost for ignored entries, rejected duplicates, or non-chart files. Since enumeration is in archive order, the final value is the highest requested chart-entry ordinal.

- [ ] If the archive is solid, clear `chartPaths` and reset `readCost` to zero because the bounded non-solid prefetch path will not use individual requested entries.

- [ ] Add `readCost=<value>` to the existing archive-index completion log so traces expose the estimator without per-entry logging.

### 2.3 Propagate the estimator to both prefetch paths

- [ ] Add `archiveReadCost` to `IndexedArchivePrefetch`:

```cpp
std::size_t archiveReadCost = 0;
```

- [ ] Extend the existing prefetch-queue log in `runIndexedArchivePrefetch` so the stable field order is:

```text
requested=<count> discoveryOrder=<order> readCost=<cost>
```

- [ ] Pass the cost to the scheduler:

```cpp
scheduler.enqueue(std::move(work), workClass, prefetch.archiveOrder,
                  prefetch.archiveReadCost)
```

- [ ] Keep `archiveReadCostByPath` beside the existing discovery-order map. When applying prepared archive entities in discovery order, store `scan.readCost` under the same normalized archive key used by the order map.

- [ ] When creating eager `IndexedArchivePrefetch`, copy both discovery order and cost from the just-completed `ArchiveScanResult`.

- [ ] In the checkpoint fallback path, look up both order and cost by archive key. Default order to the existing fallback value and cost to zero only when the map has no entry.

- [ ] Extend the fallback queue log with `discoveryOrder=` and `readCost=` in the same order as eager prefetch, and pass both values to `enqueue`.

### 2.4 Verify and commit Task 2

- [ ] Build and run the scanner tests:

```sh
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R chart_library_scanner_tests
```

- [ ] Run the scheduler and scanner tests together:

```sh
ctest --test-dir cmake-build-debug --output-on-failure -R "chart_(scan_work_scheduler|library_scanner)_tests"
```

- [ ] Review the scanner diff to confirm archive enumeration still happens once, read cost is zero for solid archives, eager/fallback paths share the same estimator, and logs contain no entry-level noise.

- [ ] Commit:

```sh
git add src/ChartLibraryScanner.cpp tests/chart_library_scanner_tests.cpp
git commit -m "perf: propagate archive read cost"
```

## Task 3: Full Verification, Self-Review, and Publication

**Files:**

- Review: all branch changes versus `develop`
- Update: GitHub PR #84 description or comment

### 3.1 Perform static self-review

- [ ] Inspect repository state and the complete branch diff:

```sh
git status --short --branch
git diff --check develop...HEAD
git diff --stat develop...HEAD
git diff develop...HEAD -- src/ChartScanWorkScheduler.h src/ChartScanWorkScheduler.cpp src/ChartLibraryScanner.cpp tests/chart_scan_work_scheduler_tests.cpp tests/chart_library_scanner_tests.cpp
```

- [ ] Confirm manual-rebuild cleanup still names all four persisted structures:

```sh
rg -n "chart_meta|solid_archives|archive_scan_cache|chart_scan_checkpoint" src tests
```

- [ ] Check that all archive-read enqueue sites provide cost where known and that no old queue member remains:

```sh
rg -n "ArchiveRead|ArchiveReadHeavy|archiveReadQueue_|archiveReadsBy(Order|Cost)|archiveReadCost" src tests
```

- [ ] Fix and commit every issue found by self-review before continuing.

### 3.2 Run required local verification

- [ ] Build the desktop application target:

```sh
cmake --build cmake-build-debug --target main -j 6
```

- [ ] Run the complete CTest suite:

```sh
ctest --test-dir cmake-build-debug --output-on-failure
```

- [ ] Run the iOS build-only path, which does not upload:

```sh
scripts/ios_firebase_deploy.sh --build-only
```

- [ ] If a failure is unrelated and pre-existing, capture the exact failing command and evidence. Otherwise fix it test-first and rerun the failed command plus the full relevant suite.

### 3.3 Final branch review and push

- [ ] Confirm the worktree is clean and commits are intentional:

```sh
git status --short --branch
git log --oneline --decorate origin/develop..HEAD
```

- [ ] Push normally without force:

```sh
git push origin perf/dynamic-chart-scan-scheduling
```

- [ ] Confirm local and remote branch SHAs match:

```sh
git rev-parse HEAD
git rev-parse origin/perf/dynamic-chart-scan-scheduling
```

### 3.4 Update and verify PR #84

- [ ] Update PR #84 with:

  - latest representative manual rebuild: 14.682 s;
  - previous comparison runs: 13.756 s and 13.353 s;
  - latest aggregate archive-read work: 45.097 s versus 42.727 s;
  - observed correlation: extraction duration versus estimated unpacked size 0.13, versus highest requested entry ordinal 0.58;
  - the new architecture: one discovery frontier plus cost-prioritized speculative reads;
  - preservation of CPU contraction and ordered database/checkpoint mutation;
  - exact desktop, CTest, and iOS build-only validation results;
  - an explicit note that a new representative device log is still required to validate performance.

- [ ] Verify PR state and URL:

```sh
gh pr view 84 --json number,title,state,isDraft,headRefName,baseRefName,url
```

- [ ] Report the commits, validations, PR URL, and the remaining device-log validation requirement. Do not state that the performance regression is fixed until that trace is available.
