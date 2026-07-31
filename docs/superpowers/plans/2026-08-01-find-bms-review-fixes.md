# Find BMS Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Address all four actionable review findings on PR #88 without changing the intended incremental Find BMS workflow.

**Architecture:** Keep the existing serialized library worker and post-reload handoff. Harden its boundaries by retaining the selected unavailable record before reload, requiring a validated target to have a committed matching upsert before task completion, making scanner interruption state atomic, and cleaning rejected private transactions on every exit.

**Tech Stack:** C++23, SQLite, CMake/CTest, GitHub CLI

## Global Constraints

- Continue on `agent/incremental-find-bms-scan` in the current checkout; do not create a worktree.
- Preserve FIFO serialization between full scans and downloaded-path scans.
- Do not auto-select title-only or kept mismatched downloads.
- Do not change unzip preview suppression.
- Do not deploy a mobile build.
- Use red-green TDD for each behavior change and rerun the complete CTest suite before publishing.

---

### Task 1: Clean rejected extracted transactions

**Files:**
- Modify: `tests/find_bms_download_tests.cpp`
- Modify: `src/bms_search/DownloadStaging.cpp`

**Interfaces:**
- Consumes: `commitFindBmsPendingArtifact(...)` and the existing `.asobmashow-transactions/<uuid>` transaction layout.
- Produces: every post-creation failure path removes both its UUID directory and an empty transaction root.

- [x] **Step 1: Write the failing test**

Add `testExtractedCommitConflictCleansPrivateTransaction()` that creates a regular file at the extracted artifact destination, calls `commitFindBmsPendingArtifact`, and asserts failure, preservation of the destination file, and absence of `.asobmashow-transactions`.

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target find_bms_download_tests -j 6 && ./cmake-build-debug/find_bms_download_tests`

Expected: FAIL because the rejected destination leaves the private transaction root behind.

- [x] **Step 3: Write minimal implementation**

Call the already-defined `cleanupTransaction()` immediately before returning `false` from the extracted destination-is-not-a-directory branch.

- [x] **Step 4: Run test to verify it passes**

Run the Task 1 command again and require exit 0.

### Task 2: Preserve the handoff selection across reload

**Files:**
- Modify: `scripts/check_find_bms_archive_flow.py`
- Modify: `src/scene/MainMenuScene.cpp`

**Interfaces:**
- Consumes: `selectedRecordSnapshot()`, `PendingFindBmsSelectionHandoff`, and `findBmsSelectionHandoffAllowed(...)`.
- Produces: the Find BMS eligibility check uses a selection snapshot captured before `reloadChartList(true)` resets the recycler selection.

- [x] **Step 1: Write the failing flow regression**

Extend the Find BMS flow audit to require a named pre-reload selection snapshot and require that the post-reload eligibility call consumes it.

- [x] **Step 2: Run audit to verify it fails**

Run: `python3 scripts/check_find_bms_archive_flow.py`

Expected: FAIL because `applyPendingUiUpdates()` currently calls `selectedRecordSnapshot()` after reload.

- [x] **Step 3: Write minimal implementation**

When a pending Find BMS handoff is consumed, capture `selectedRecordSnapshot()` before any folder or chart reload. After reload, validate the handoff against that retained snapshot and the current generation.

- [x] **Step 4: Run audit and policy tests**

Run: `cmake --build cmake-build-debug --target main_menu_library_tests -j 6 && ./cmake-build-debug/main_menu_library_tests && python3 scripts/check_find_bms_archive_flow.py`

Expected: both commands exit 0; the existing empty-`BmsPath` unavailable-record policy test remains green.

### Task 3: Fail validated downloads without a committed target upsert

**Files:**
- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `src/scene/MainMenuLibrary.h`
- Modify: `src/scene/MainMenuLibrary.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `scripts/check_find_bms_archive_flow.py`

**Interfaces:**
- Produces: `findBmsIndexTaskSucceeded(const FindBmsChartIdentity &, bool scanCommitted, const std::optional<std::filesystem::path> &indexedTargetPath)`.
- Consumes: the target identity, `ChartScanResult::committed`, and the hash/output/upsert-filtered chart path.

- [x] **Step 1: Write failing policy tests**

Cover these literal outcomes: an identity-less kept mismatch may complete without a handoff; a durable target fails without a commit; a durable target fails without a resolved upserted path; and a durable target succeeds only with both.

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target main_menu_library_tests -j 6`

Expected: compilation fails because `findBmsIndexTaskSucceeded` does not exist.

- [x] **Step 3: Implement and wire the policy**

Implement the pure helper. In `runDownloadedPathIndexTask`, resolve the matching path only after a completed scan, evaluate the helper, request a reload and throw `std::runtime_error` on failure, and publish a handoff only for a valid target with a resolved path.

- [x] **Step 4: Run policy and scanner tests**

Run: `cmake --build cmake-build-debug --target main_menu_library_tests chart_library_scanner_tests main -j 6 && ./cmake-build-debug/main_menu_library_tests && ./cmake-build-debug/chart_library_scanner_tests`

Expected: all builds and tests exit 0.

### Task 4: Remove the scanner interruption data race

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`
- Modify: `src/ChartLibraryScanner.cpp`

**Interfaces:**
- Consumes: the existing `shouldStop()` closure called by traversal and parser worker threads.
- Produces: interruption state stored and loaded through `std::atomic_bool` with relaxed ordering; final result reads the atomic state.

- [x] **Step 1: Add a concurrent interruption regression**

Add a scanner test with several ordinary chart entities whose worker-thread pause callbacks rendezvous and return `false`, then assert the scan reports `completed == false` and terminates cleanly.

- [x] **Step 2: Attempt to verify the race under ThreadSanitizer**

Configure a disposable `cmake-build-tsan` with `-fsanitize=thread`, build `chart_library_scanner_tests`, and run it.

Expected: the new concurrent interruption path reports a ThreadSanitizer race at the plain `interrupted` flag before the fix.

Actual: the instrumented binary built successfully but the macOS ThreadSanitizer runtime exited with signal 11 before emitting diagnostics, both before and after the fix.

- [x] **Step 3: Write minimal implementation**

Replace the plain flag with `std::atomic_bool`, short-circuit with `load(std::memory_order_relaxed)`, set it with `store(std::memory_order_relaxed)`, and load it when constructing `ChartScanResult`.

- [x] **Step 4: Verify normal tests and retry the sanitized test**

Run the normal scanner test and the sanitized scanner test again.

Expected: both exit 0 and ThreadSanitizer reports no race in scanner interruption bookkeeping.

Actual: the normal scanner suite passed repeatedly; the sanitized executable hit the same startup signal 11 described above.

### Task 5: Verify and publish

**Files:**
- Modify: `docs/superpowers/plans/2026-08-01-find-bms-review-fixes.md`

**Interfaces:**
- Consumes: all Task 1-4 changes.
- Produces: a clean pushed branch with review fixes attached to PR #88.

- [x] **Step 1: Run focused verification**

Run: `cmake --build cmake-build-debug --target main_menu_library_tests chart_repository_tests chart_library_scanner_tests find_bms_download_tests artwork_cache_contract_tests main -j 6` followed by all focused binaries and `python3 scripts/check_find_bms_archive_flow.py`.

- [x] **Step 2: Run full verification**

Run: `ctest --test-dir cmake-build-debug --output-on-failure` and require all 178 tests to pass, including the newly added regressions under their existing targets.

- [x] **Step 3: Self-review and commit**

Run `git diff --check`, inspect the complete change against each review, stage only the plan and scoped source/test files, and commit with `fix: address Find BMS review feedback`.

- [ ] **Step 4: Push and inspect PR state**

Push `agent/incremental-find-bms-scan`, confirm local and remote HEAD match, and re-fetch review threads. Do not reply to or resolve GitHub threads without separate explicit authorization.
