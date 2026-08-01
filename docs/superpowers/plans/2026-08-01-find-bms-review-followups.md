# Find BMS Review Follow-ups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconcile library records for variants removed by a Find BMS commit and complete eligible preview handoffs even when the active chart query hides the downloaded chart.

**Architecture:** The storage commit produces an exact best-effort removal manifest that travels through `BmsSearchResult` into the existing serialized downloaded-path task. That task deletes database records under the removed roots before its additions-only scan. Visible selection remains preferred, with a Find-BMS-only exact-path lookup driving the normal selection callback when search or record filters hide the chart.

**Tech Stack:** C++23, SQLite, CMake/CTest, GitHub CLI

## Global Constraints

- Continue on `agent/incremental-find-bms-scan` in the current checkout; do not create a worktree.
- Do not start a full library scan for a Find BMS download.
- Keep cleanup and indexing FIFO-serialized with any current full scan.
- Report only roots whose disk removal completed without an error.
- Preserve active folder, search text, chart filters, and sort state during preview handoff.
- Keep selection-generation, durable-hash, and title-only mismatch policies unchanged.
- Write and run a failing regression before each production behavior change.

---

### Task 1: Carry successful disk removals into targeted library cleanup

**Files:**
- Modify: `src/BmsSearchService.h`
- Modify: `src/bms_search/DownloadStaging.h`
- Modify: `src/bms_search/DownloadStaging.cpp`
- Modify: `src/bms_search/DownloadedArchiveWorkflow.h`
- Modify: `src/bms_search/DownloadedArchiveWorkflow.cpp`
- Modify: `src/bms_search/DownloadSupport.cpp`
- Modify: `src/bms_search/PendingArtifact.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `scripts/check_find_bms_archive_flow.py`

**Interfaces:**
- `BmsSearchResult` produces `std::vector<std::filesystem::path> removedPaths`.
- `commitFindBmsPendingArtifact(...)` keeps its existing callers compatible and accepts an optional final `std::vector<std::filesystem::path> *removedPaths` after `FindBmsRenameOperation`.
- `DownloadedArchiveWorkflowDependencies::commitArtifact` consumes `(const BmsSearchPendingArtifact &, std::string &, std::vector<std::filesystem::path> &)`.
- `LibraryTaskRequest` consumes `downloadedRemovedPaths`; `enqueueDownloadedPathIndexTask(...)` receives the same vector as its final defaulted argument.

- [x] **Step 1: Write failing storage and result propagation tests**

Extend `tests/find_bms_download_tests.cpp` so:

```cpp
std::vector<std::filesystem::path> removedPaths;
assert(commitFindBmsPendingArtifact(artifact, error, {}, &removedPaths));
assert(removedPaths == std::vector<std::filesystem::path>{expectedAlternate});
```

Cover an archive/extracted alternate, renamed same-storage-ID variants, an unrelated variant that is not reported, automatic workflow commit propagation, and manual Keep Files propagation.

- [x] **Step 2: Run the test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6
```

Expected: compilation fails because the removal output and result field do not exist.

- [x] **Step 3: Implement exact successful-removal reporting**

In `DownloadStaging.cpp`, replace variant cleanup's unobserved `remove_all` calls with a helper equivalent to:

```cpp
void removeVariantPath(const std::filesystem::path &path,
                       std::vector<std::filesystem::path> *removedPaths) {
  std::error_code error;
  const std::uintmax_t removed = std::filesystem::remove_all(path, error);
  if (!error && removed > 0 && removedPaths != nullptr) {
    removedPaths->push_back(path.lexically_normal());
  }
}
```

Use it only for alternate and stale identity variants, not transaction, staging, backup, or current destination cleanup. Clear the supplied vector before a validated commit attempt so failed commits never expose stale caller data.

- [x] **Step 4: Propagate the manifest through both commit workflows**

Add `removedPaths` to `BmsSearchResult`. Update the workflow dependency callback to populate a local vector and move it into `result.removedPaths` only after a successful commit. In `resolvePendingArtifact`, pass `&result.removedPaths` for Keep and clear the vector for Delete or failed resolution.

- [x] **Step 5: Verify storage/result GREEN**

Run:

```bash
cmake --build cmake-build-debug --target find_bms_download_tests -j 6 && ./cmake-build-debug/find_bms_download_tests
```

Expected: exit 0.

- [x] **Step 6: Write a failing main-menu flow regression**

Extend `scripts/check_find_bms_archive_flow.py` to require:

```text
findBmsResult.removedPaths -> enqueueDownloadedPathIndexTask
task.downloadedRemovedPaths -> DeleteChartMetaInDirectory
DeleteChartMetaInDirectory occurs before LoadCharts in runDownloadedPathIndexTask
```

- [x] **Step 7: Run the audit to verify RED**

Run: `python3 scripts/check_find_bms_archive_flow.py`

Expected: failure stating that removed Find BMS variants are not reconciled.

- [x] **Step 8: Implement serialized targeted reconciliation**

Add `downloadedRemovedPaths` to the task request and enqueue signature. Pass the result vector from both downloaded and kept-mismatch call sites. Before constructing scan entries, run:

```cpp
for (const auto &removedPath : task.downloadedRemovedPaths) {
  if (taskSession->DeleteChartMetaInDirectory(removedPath) < 0) {
    requestLibraryReload(true);
    throw std::runtime_error(
        "Failed to reconcile replaced Find BMS files");
  }
}
```

Do not create a separate worker or scan; the existing library task queue is the ordering boundary.

- [x] **Step 9: Verify targeted reconciliation GREEN**

Run:

```bash
cmake --build cmake-build-debug --target find_bms_download_tests main -j 6 && ./cmake-build-debug/find_bms_download_tests && python3 scripts/check_find_bms_archive_flow.py
```

Expected: all commands exit 0.

### Task 2: Select eligible downloaded charts outside active filters

**Files:**
- Modify: `src/scene/MainMenuLibrary.h`
- Modify: `src/scene/MainMenuLibrary.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `scripts/check_find_bms_archive_flow.py`

**Interfaces:**
- Produces `std::optional<ChartMetaRecord> findBmsUnfilteredHandoffRecord(const ChartMetaPathBatchReadOutcome &, const std::filesystem::path &)`.
- Consumes `ChartRepository::Session::SelectChartMetaByPaths` only after visible-query selection fails and only for `AutoSelectionPreview::Load`.

- [x] **Step 1: Write failing exact-record policy tests**

Add cases to `tests/main_menu_library_tests.cpp` proving that the helper returns the exact record from a Loaded outcome and rejects StorageFailure, missing records, and a record whose normalized path differs from the request.

- [x] **Step 2: Run the policy test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
```

Expected: compilation fails because `findBmsUnfilteredHandoffRecord` is undefined.

- [x] **Step 3: Implement the exact-record helper**

Return a record only when `outcome.status == ChartMetaPathBatchReadStatus::Loaded` and a record has `meta.BmsPath.lexically_normal() == requestedPath.lexically_normal()`. Return `std::nullopt` for empty paths and every other outcome.

- [x] **Step 4: Verify policy GREEN**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6 && ./cmake-build-debug/main_menu_library_tests
```

Expected: exit 0.

- [x] **Step 5: Write a failing selection-flow audit**

Require `selectChartByPathAfterReload` to call `SelectChartMetaByPaths` and `findBmsUnfilteredHandoffRecord` for `AutoSelectionPreview::Load` before its All Songs fallback. Reject `searchText.clear()` or assignment to `chartRecordFilters` within that function.

- [x] **Step 6: Run the audit to verify RED**

Run: `python3 scripts/check_find_bms_archive_flow.py`

Expected: failure stating that Find BMS handoff still depends on active filters.

- [x] **Step 7: Implement off-list normal selection**

After visible selection fails, exact-load the path. When a record is returned, unselect any visible selected row, set `recyclerView->selectedIndex = -1`, rebind visible rows, clear a matching suppression path, and call `recyclerView->onSelected(*record, -1)`. Return afterward. Keep the All Songs recursion unchanged for `AutoSelectionPreview::Suppress` and lookup failures.

- [x] **Step 8: Verify selection GREEN**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests main -j 6 && ./cmake-build-debug/main_menu_library_tests && python3 scripts/check_find_bms_archive_flow.py
```

Expected: all commands exit 0.

### Task 3: Verify and publish the follow-ups

**Files:**
- Modify: `docs/superpowers/plans/2026-08-01-find-bms-review-followups.md`

**Interfaces:**
- Consumes all Task 1 and Task 2 changes.
- Produces a clean pushed branch at PR #88.

- [x] **Step 1: Run focused verification**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests chart_repository_tests chart_library_scanner_tests find_bms_download_tests artwork_cache_contract_tests main -j 6
./cmake-build-debug/main_menu_library_tests
./cmake-build-debug/chart_repository_tests
./cmake-build-debug/chart_library_scanner_tests
./cmake-build-debug/find_bms_download_tests
./cmake-build-debug/artwork_cache_contract_tests
python3 scripts/check_find_bms_archive_flow.py
git diff --check
```

Expected: every build and command exits 0.

- [x] **Step 2: Run full verification**

Run: `ctest --test-dir cmake-build-debug --output-on-failure`

Expected: all 178 tests pass.

- [x] **Step 3: Self-review and request independent review**

Inspect the complete diff against both new P2 threads. Require no Critical or Important findings before publishing.

- [ ] **Step 4: Commit and push**

Stage only the plan plus scoped source, audit, and test files. Commit with `fix: reconcile Find BMS replacement handoff`, push `agent/incremental-find-bms-scan`, and confirm local, remote, and PR head SHAs match.

- [ ] **Step 5: Re-fetch PR comments and checks**

Use the bundled thread-aware comment script. Report which findings are addressed; do not reply to or resolve review threads without explicit authorization.
