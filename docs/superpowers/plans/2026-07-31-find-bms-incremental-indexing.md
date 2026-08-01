# Find BMS Incremental Indexing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Index only the archive or extracted directory committed by Find BMS without starting a full-library refresh.

**Architecture:** Add a checkpoint-only repository snapshot mode and a `ChartLibraryScanner::ScanAdded()` entry point that reuses the parser/transaction pipeline without loading or reconciling unrelated library state. Route successful Find BMS results to a dedicated serialized library task carrying `BmsSearchResult::outputPath`; the existing refresh and rebuild paths remain unchanged.

**Tech Stack:** C++23, SQLite, CMake/CTest, existing chart scanner and library task worker

## Global Constraints

- Work on `agent/incremental-find-bms-scan` in the current checkout; do not create a worktree.
- A successful Find BMS download scans exactly `BmsSearchResult::outputPath`.
- Both extracted directories and kept archive files must be indexed.
- Additions-only scanning must not load, stat, update, or delete unrelated chart metadata.
- A supplied archive must be reindexed and replace only its own prior chart/cache state.
- Additions-only scanning must not clear the global chart-metadata-rebuild-required marker.
- Manual Refresh Library and Rebuild Library behavior must remain unchanged.
- Do not deploy or install a mobile build.

---

### Task 1: Add additions-only chart scanning

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`
- Modify: `src/repositories/ChartScanStore.h`
- Modify: `src/repositories/ChartRepository.h`
- Modify: `src/repositories/ChartScanStore.cpp`
- Modify: `src/ChartLibraryScanner.h`
- Modify: `src/ChartLibraryScanner.cpp`

**Interfaces:**
- Produces: `enum class ChartScanSnapshotLoad { Full, CheckpointOnly };`
- Produces: `ChartRepository::Session::LoadScanSnapshot(ChartScanSnapshotLoad load = ChartScanSnapshotLoad::Full)`
- Produces: `ChartLibraryScanner::ScanAdded(...)` with the same callbacks and return contract as `Scan(...)`.
- Preserves: `ChartLibraryScanner::Scan(...)` as full reconciliation.

- [ ] **Step 1: Write failing scanner regressions**

Add these tests before production changes:

```cpp
void testAddedDirectoryScanPreservesUnrelatedMissingChart() {
  TempDirectory temporary;
  const auto existingRoot = temporary.path() / "existing";
  const auto addedRoot = temporary.path() / "downloaded";
  const auto existing = writeChart(existingRoot, "existing", "Existing");
  writeChart(addedRoot, "added", "Added");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {existingRoot}) == 1);
  std::filesystem::remove(existing);

  assert(scanner.ScanAdded(*session, {addedRoot}) == 1);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 2);
  assert(std::any_of(snapshot.charts.begin(), snapshot.charts.end(),
                     [](const auto &meta) { return meta.Title == "Existing"; }));
  assert(std::any_of(snapshot.charts.begin(), snapshot.charts.end(),
                     [](const auto &meta) { return meta.Title == "Added"; }));
}

void testAddedArchivePathIsIndexed() {
  TempDirectory temporary;
  const auto archive = writeZip(
      temporary.path() / "downloaded.zip",
      {{"inside.bms", chartText("Downloaded Archive")}});

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.ScanAdded(*session, {archive}) == 2);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 1);
  assert(snapshot.charts.front().Title == "Downloaded Archive");
  assert(snapshot.archiveCache.size() == 1);
  assert(snapshot.archiveCache.front().chartCount == 1);
}
```

Register both tests in `main()`.

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
```

Expected: compilation fails because `ChartLibraryScanner::ScanAdded()` does not exist.

- [ ] **Step 3: Add checkpoint-only snapshot loading**

In `src/repositories/ChartScanStore.h`, add:

```cpp
enum class ChartScanSnapshotLoad { Full, CheckpointOnly };
```

Change the session declaration and implementation to accept the enum. In
`loadScanSnapshot()`, execute the `chart_meta`, `solid_archives`, and
`archive_scan_cache` queries only for `Full`; always load the checkpoint.
Existing callers rely on the default and retain full behavior.

- [ ] **Step 4: Share scanner implementation between full and added modes**

Add the public `ScanAdded()` method and a private implementation method:

```cpp
int ScanImpl(ChartRepository::Session &session,
             const std::vector<std::filesystem::path> &roots,
             bool reconcileExisting,
             const std::stop_token *stopToken,
             ChartScanProgressCallback progressCallback,
             ChartScanPauseCallback pauseCallback,
             ChartScanFlushRequestCallback flushRequestCallback,
             ChartScanFlushCompleteCallback flushCompleteCallback);
```

`Scan()` forwards with `reconcileExisting = true`; `ScanAdded()` forwards with
`false`. `ScanImpl()` loads `Full` when reconciling and `CheckpointOnly`
otherwise. Because the additions-only snapshot contains no chart, solid, or
archive-cache rows, the existing discovery pipeline parses every supplied BMS
path, always reindexes a supplied archive, and has no unrelated deletion work.

At both no-work and successful completion sites, call
`ClearChartMetadataRebuildRequired()` only when `reconcileExisting` is true.
Checkpoint cleanup remains common to both modes.

- [ ] **Step 5: Run the focused scanner tests and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6
./cmake-build-debug/chart_library_scanner_tests
```

Expected: build and test process exit 0, including both new directory/archive regressions.

### Task 2: Route Find BMS to a downloaded-path task

**Files:**
- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `src/scene/MainMenuLibrary.h`
- Modify: `src/scene/MainMenuLibrary.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `scripts/check_find_bms_archive_flow.py`

**Interfaces:**
- Produces: `main_menu_library::downloadedPathScanEntries(const std::filesystem::path &path)`.
- Produces: `LibraryTaskKind::IndexDownloadedPath` and `LibraryTaskRequest::downloadedPath`.
- Produces: `enqueueDownloadedPathIndexTask()` and `runDownloadedPathIndexTask()`.
- Removes: `additionalFolderToScan`, `findBmsDownloadRoot`, and `appendUniqueScanFolder()`.

- [ ] **Step 1: Write the failing exact-root policy tests**

Replace the `appendUniqueScanFolder()` assertions in
`tests/main_menu_library_tests.cpp` with:

```cpp
const auto directoryEntries = main_menu_library::downloadedPathScanEntries(
    std::filesystem::path("/library/downloaded-folder"));
ASSERT_EQ(static_cast<std::size_t>(1), directoryEntries.size(),
          "downloaded folder produces one scan root");
ASSERT_EQ(std::filesystem::path("/library/downloaded-folder"),
          std::filesystem::path(directoryEntries.front().path),
          "downloaded folder is the exact scan root");

const auto archiveEntries = main_menu_library::downloadedPathScanEntries(
    std::filesystem::path("/library/_archives/downloaded.zip"));
ASSERT_EQ(static_cast<std::size_t>(1), archiveEntries.size(),
          "downloaded archive produces one scan root");
ASSERT_EQ(std::filesystem::path("/library/_archives/downloaded.zip"),
          std::filesystem::path(archiveEntries.front().path),
          "downloaded archive is the exact scan root");
ASSERT_EQ(true,
          main_menu_library::downloadedPathScanEntries({}).empty(),
          "empty result path produces no scan root");
```

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
```

Expected: compilation fails because `downloadedPathScanEntries()` does not exist.

- [ ] **Step 3: Add the exact-root helper and dedicated task kind**

Implement `downloadedPathScanEntries()` to return an empty vector for an empty
path and otherwise one `ChartEntry` whose path is the supplied path.

In `MainMenuScene`, add the task kind and field, then add an enqueue method that
rejects an empty path and records a task titled `Index Downloaded BMS`. Extend
the worker switch to call `runDownloadedPathIndexTask()`.

The new runner opens a repository session, ensures the schema, builds entries
with `downloadedPathScanEntries()`, and calls `LoadCharts()` with an
`addedPathsOnly` flag. It reports progress with the existing
`updateLibraryTaskProgress()` and `waitForLibraryTaskResume()` callbacks. It
does not run any of `runLibraryRefreshTask()`'s folder, table, bootstrap,
bookmark, or rebuild work.

Extend `LoadCharts()` with `bool addedPathsOnly = false` and select the scanner
entry point explicitly:

```cpp
const int changedCount = addedPathsOnly
    ? scanner.ScanAdded(chartSession, roots, &stop_token, progressCallback,
                        pauseCallback)
    : scanner.Scan(chartSession, roots, &stop_token, progressCallback,
                   pauseCallback,
                   [&scene]() { return scene.pendingLibraryScanFlushRequest(); },
                   [&scene](std::uint64_t request) {
                     scene.completeLibraryScanFlush(request);
                   });
```

The incremental path does not supply full-scan flush callbacks because it does
not own a paused full-library reconciliation checkpoint.

- [ ] **Step 4: Wire Find BMS results and remove obsolete refresh plumbing**

In `applyFindBmsUpdates()`, replace the full refresh call with:

```cpp
enqueueDownloadedPathIndexTask(findBmsResult.outputPath);
```

Keep the existing status condition covering both `Downloaded` and completed
Keep Files mismatch resolution. Remove capture/reset/storage of
`findBmsDownloadRoot`, remove `additionalFolderToScan` from refresh task APIs,
and remove `appendUniqueScanFolder()`.

Update `scripts/check_find_bms_archive_flow.py` so the audit requires
`enqueueDownloadedPathIndexTask(findBmsResult.outputPath)`,
`IndexDownloadedPath`, and `ScanAdded`, and rejects
`startLibraryRefresh(findBmsDownloadRoot)` plus the removed transient-root
plumbing.

- [ ] **Step 5: Run focused task-routing tests and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
./cmake-build-debug/main_menu_library_tests
python3 scripts/check_find_bms_archive_flow.py
```

Expected: both binaries/scripts exit 0 and the audit prints `Find BMS archive-flow audit passed`.

### Task 2A: Guard an in-flight full scan from download transaction paths

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `src/ChartLibraryScanner.cpp`
- Modify: `src/bms_search/DownloadStorageIdentity.h`
- Modify: `src/bms_search/DownloadStaging.cpp`

- [ ] **Step 1: Add a failing transaction-directory regression**

Create a `BMSSEARCH` fixture containing a final chart directory, a chart beneath
`.asobmashow-transactions/<uuid>/commit`, and a similarly named non-reserved
directory. Run a full scan and assert that only the final and similarly named
normal charts are stored. Update the swap-failure regression to require the
commit rename source to come from that private namespace and require cleanup
after rollback restores the destination.

- [ ] **Step 2: Verify RED**

Build and run `chart_library_scanner_tests` and `find_bms_download_tests`.
Expected: the scanner assertion fails because the iterator enters the private
tree, and swap-failure injection does not trigger because the commit source is
still a destination-like sibling.

- [ ] **Step 3: Add the narrow traversal exclusion**

Define one reserved Find BMS transaction directory name. Prepare commit and
backup payloads under `BMSSEARCH/.asobmashow-transactions/<uuid>/` so they stay
on the destination filesystem while remaining outside the chart namespace.
Clean the UUID directory and remove the reserved parent only when empty. In the
scanner, call `disable_recursion_pending()` only for that exact reserved direct
child of `BMSSEARCH`.

- [ ] **Step 4: Verify GREEN**

Rebuild and run both focused binaries. Expected: the private-namespace,
rollback, and existing scanner/download suites exit 0.

### Task 3: Verify, self-review, commit, and publish

**Files:**
- Review: all changed source, test, spec, plan, and audit files.

**Interfaces:**
- Produces: pushed branch `agent/incremental-find-bms-scan` and a draft pull request targeting `develop`.

- [ ] **Step 1: Run focused integration verification**

Run:

```bash
cmake --build cmake-build-debug --target chart_library_scanner_tests main_menu_library_tests find_bms_download_tests main -j 6
./cmake-build-debug/chart_library_scanner_tests
./cmake-build-debug/main_menu_library_tests
./cmake-build-debug/find_bms_download_tests
python3 scripts/check_find_bms_archive_flow.py
```

Expected: build and every focused check exit 0.

- [ ] **Step 2: Run the full desktop suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 100% of tests pass with zero failures.

- [ ] **Step 3: Self-review requirements and diff**

Run `git diff --check`, inspect `git diff develop...HEAD` plus the uncommitted
diff, and verify each design requirement against code/tests. In particular,
confirm manual refresh still selects effective entries and imports tables,
incremental scanning uses only `outputPath`, unrelated metadata cannot be
deleted, exact archives are reindexed, and rebuild-required state is not
cleared by `ScanAdded()`. Confirm a running full scan cannot descend into the
reserved Find BMS private transaction namespace, while similarly named user
folders remain scannable and transactional rename/rollback stays same-volume.

- [ ] **Step 4: Commit scoped implementation changes**

Stage only the files named in Tasks 1-2 plus this plan and commit with:

```bash
git commit -m "perf: index Find BMS downloads incrementally"
```

- [ ] **Step 5: Push and open the draft PR**

Push with tracking:

```bash
git push -u origin agent/incremental-find-bms-scan
```

Create a draft pull request targeting `develop`. The body must summarize the
root cause, dedicated task/scanner-mode fix, behavior for extracted and packed
downloads, and the exact focused/full verification evidence.
