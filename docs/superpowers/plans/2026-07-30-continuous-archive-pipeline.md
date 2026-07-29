# Continuous Archive Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let multi-chart archive reading and parsing begin during entity indexing while retaining the existing single-large-archive concurrent fast path and ordered database application.

**Architecture:** Keep the entity `WorkScheduler` alive through database application and publish early archive parsing into shared per-archive result states. Hold the first large archive as a speculative single-archive candidate; release it and all later large archives into the continuous pipeline after a second large archive is discovered.

**Tech Stack:** C++23, `chart_scan::WorkScheduler`, miniz/7-Zip archive backends, SQLite repository integration, CMake tests.

## Global Constraints

- Work only in the existing `perf/dynamic-chart-scan-scheduling` isolated worktree.
- Execute inline with self-review; do not dispatch subagents or pause for plan approval.
- Preserve one admitted archive reader while CPU tasks are queued and at most four archive readers when the CPU queue is empty.
- Preserve discovery-ordered SQLite, cache, progress, and checkpoint mutation.
- Preserve the full-worker concurrent backend when exactly one large archive requires parsing.
- Do not change archive codecs, the database schema, parser behavior, or image-loading telemetry.
- Do not run a local performance benchmark because this machine has no representative archive library.

---

### Task 1: Specify continuous multi-archive prefetch behavior

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `ChartLibraryScanner::Scan`, `archive_file::debugLogLines`, and real ZIP fixtures created by `writeZip`.
- Produces: `testMultipleLargeArchivesPrefetchDuringPreparation()` and strengthened assertions in `testLargeSingleArchivePreservesAllChartResults()`.

- [x] **Step 1: Strengthen the single-large-archive contract**

After the first scan in `testLargeSingleArchivePreservesAllChartResults()`, find fixture-specific log lines and require the direct backend while rejecting indexed prefetch:

```cpp
const auto logLines = archive_file::debugLogLines();
assert(hasFixtureLog(logLines, archivePath,
                     "Finished single archive concurrent chart parse:"));
assert(!hasFixtureLog(logLines, archivePath,
                      "Prefetching indexed archive chart batch:"));
```

Use a local test helper that searches the real debug log by event prefix and archive filename.

- [x] **Step 2: Add the multi-archive prefetch regression**

Create two ZIP archives containing 24 valid charts each, scan both roots, verify 48 stored charts and both cache counts, then assert both paths log indexed prefetch and neither logs post-index bounded queuing:

```cpp
assert(hasFixtureLog(logLines, firstArchive,
                     "Prefetching indexed archive chart batch:"));
assert(hasFixtureLog(logLines, secondArchive,
                     "Prefetching indexed archive chart batch:"));
assert(!hasFixtureLog(logLines, firstArchive,
                      "Queued bounded DB archive chart batch parse:"));
assert(!hasFixtureLog(logLines, secondArchive,
                      "Queued bounded DB archive chart batch parse:"));
```

- [x] **Step 3: Run the focused test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
./cmake-build-debug/chart_library_scanner_tests
```

Expected: FAIL because the current scanner emits `Queued bounded DB archive chart batch parse` for both 24-chart archives and has no indexed-prefetch event.

### Task 2: Keep archive parsing live across preparation and application

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `runArchiveChartPipeline`, `chart_scan::WorkScheduler`, `ArchiveParseJobResult`, and the ordered chunk application loop.
- Produces: shared `ArchiveParseJobState` objects that accept chunks before `archiveBatchOrder` exists and are later consumed by archive index.

- [x] **Step 1: Move the archive result state beside archive parsing types**

Define `ArchiveParseJobResult`, `ArchiveParseJobState`, and `ArchiveParseJobEvent` after `ArchiveParsedChart`. Store prefetched states by normalized archive key:

```cpp
std::mutex prefetchedArchiveStateMutex;
std::unordered_map<path_t, std::shared_ptr<ArchiveParseJobState>>
    prefetchedArchiveStates;
```

Add helpers that publish a chunk or terminal result directly into a shared state and notify the existing archive-result condition variable.

- [x] **Step 2: Add a preparation-only barrier**

Make `storePreparedEntity` notify `preparedEntityCv`. After root enumeration, wait until every reserved slot has a value without finishing the scheduler:

```cpp
preparedEntityCv.wait(lock, [&] {
  return shouldStop() ||
         std::all_of(preparedEntities.begin(), preparedEntities.end(),
                     [](const auto &slot) { return slot.has_value(); });
});
```

Wrap ordinary and archive preparation tasks so exceptions still publish their fallback prepared entity before being rethrown. On cancellation, cancel the scheduler exactly as before.

- [x] **Step 3: Add speculative large-archive activation**

Represent the first archive with at least `kArchiveDirectConcurrentMinCharts` as a held candidate containing its key, path, and inner paths. When a second large archive arrives, create shared states for both and submit the held candidate as `ArchiveIo`; run the current archive pipeline from its existing archive worker. Every later large archive starts immediately. A sole candidate is left without a prefetched state so the existing direct concurrent path remains eligible.

Each started archive logs:

```cpp
archive_file::appendDebugLogLine(
    "Prefetching indexed archive chart batch: " +
    fspath_to_utf8(archivePath) +
    " requested=" + std::to_string(innerPaths.size()));
```

- [x] **Step 4: Reuse prefetched states in the ordered writer**

Build `archiveParseResults` as shared states, selecting `prefetchedArchiveStates[archiveBatchOrder[index]]` when present and a new empty state otherwise. Exclude indexes with a prefetched state from `unpreparedArchiveIndexes`; queue remaining work on `entityScheduler`, not a second scheduler. Update all result/chunk access to dereference the shared state.

- [x] **Step 5: Finish the one scheduler after application**

Remove the early normal `entityScheduler.finish()` and early exception drain. At the end of archive application, cancel or finish `entityScheduler` once and report its exceptions. Preserve early-return cancellation through the scheduler destructor.

- [x] **Step 6: Run focused tests to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests chart_scan_work_scheduler_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
./cmake-build-debug/chart_library_scanner_tests
```

Expected: PASS, including the single-large fast path, continuous two-archive prefetch, same-archive chunk overlap, checkpoint resume, cancellation, cache counts, and scheduler admission tests.

### Task 3: Verify and publish

**Files:**
- Verify: `src/ChartLibraryScanner.cpp`
- Verify: `tests/chart_library_scanner_tests.cpp`
- Update: GitHub PR #84 description

**Interfaces:**
- Consumes: the final worktree, desktop build, iOS setup tests, and iOS build script.
- Produces: reviewed commit(s) on `perf/dynamic-chart-scan-scheduling` and an updated ready PR.

- [x] **Step 1: Run fresh repository validation**

Run:

```bash
cmake --build cmake-build-debug --target main chart_library_scanner_tests chart_scan_work_scheduler_tests -j 6
./cmake-build-debug/chart_scan_work_scheduler_tests
./cmake-build-debug/chart_library_scanner_tests
python3 -m unittest tests.ios_build_setup_tests.IOSBuildSetupTests.test_ios_links_7zip_archive_registration_for_device_and_simulator
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git diff --check
```

Run the full iOS setup suite and record the existing unrelated `ir/CMakeLists.txt` membership-exception failure separately if it remains.

- [x] **Step 2: Run the iOS build-only check**

Run:

```bash
scripts/ios_firebase_deploy.sh --build-only --skip-init
```

Expected: `BUILD SUCCEEDED`; do not upload a local build.

- [x] **Step 3: Self-review and publish**

Review the complete diff for duplicate archive parsing, lost cancellation paths, result-state races, accidental ordering changes, generated files, and unrelated edits. Commit the scoped implementation, push `perf/dynamic-chart-scan-scheduling`, update PR #84 with the continuous-pipeline root cause and validation, then confirm local and remote commit equality.
