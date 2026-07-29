# Ordered Archive Result Drain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore overlap between archive parsing and ordered database/cache/checkpoint application without reintroducing nested thread pools.

**Architecture:** Archive workers publish indexed results through the existing result slots plus a condition variable. The scanner waits for and applies only the next discovery-order result while later worker tasks remain active, then joins the scheduler after ordered application finishes.

**Tech Stack:** C++23, `std::mutex`, `std::condition_variable`, the existing `chart_scan::WorkScheduler`, ChartLibraryScanner integration tests, CMake/Ninja.

## Global Constraints

- Keep SQLite, archive-cache, progress, and checkpoint mutation on the scanner thread.
- Preserve deterministic archive discovery order.
- Preserve the current fixed worker budget and dynamic archive admission rules.
- Do not change parser amalgamation or archive backend selection.
- Do not run a local performance benchmark; this machine has no representative archive library.

---

### Task 1: Characterize ordered application overlap

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `ChartLibraryScanner::Scan`, `archive_file::debugLogLines`, the existing ZIP fixture writer.
- Produces: a regression test proving the first deferred archive is applied before a slower later archive finishes streaming.

- [ ] **Step 1: Write the failing integration test**

Create a 16-chart stored ZIP followed by a much larger padded stored ZIP. Run a real scanner and locate the unique log lines `Inserting streamed DB chart batch: <first>` and `Streamed archive batch via miniz ZIP: <second>`. Assert that the insertion line occurs first and that all charts were stored.

- [ ] **Step 2: Run the test target and verify RED**

Run: `cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6 && ./cmake-build-debug/chart_library_scanner_tests`

Expected: the new ordering assertion fails because the current `finish()` barrier places every stream-complete line before every insert line.

- [ ] **Step 3: Commit the characterization test with the implementation after GREEN**

The test and production fix form one behavior change and will be committed together after Task 2 passes.

### Task 2: Drain archive results while workers remain active

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`

**Interfaces:**
- Consumes: `ArchiveParseJobResult`, `archiveParseResults`, `chart_scan::WorkScheduler`.
- Produces: condition-variable publication and ordered `waitTakeArchiveParseResult(index)` behavior.

- [ ] **Step 1: Add result notification and ordered waiting**

Add a condition variable next to `archiveParseResultMutex`. Notify it after storing a result. Add a scanner-thread helper that moves an available indexed result under the mutex, polls cancellation between bounded waits, and returns an error result if cancelled.

- [ ] **Step 2: Guarantee one terminal result per queued archive job**

Wrap each queued archive root job so exceptions publish an `ArchiveParseJobResult` containing the same archive index, inner start, pending paths, and diagnostic. This prevents the ordered consumer from waiting forever.

- [ ] **Step 3: Move scheduler shutdown after ordered application**

Remove the pre-loop `finish()` call. Use the ordered waiter when a result is not prepared synchronously. After the database loop, cancel on stop or finish normally, then report collected scheduler exceptions.

- [ ] **Step 4: Run focused tests and verify GREEN**

Run: `cmake --build cmake-build-debug --target chart_library_scanner_tests chart_scan_work_scheduler_tests archive_file_concurrency_tests -j 6`

Then run each produced test executable. Expected: all exit successfully, including the new overlap assertion.

- [ ] **Step 5: Commit**

Run: `git add src/ChartLibraryScanner.cpp tests/chart_library_scanner_tests.cpp docs/superpowers/specs/2026-07-29-ordered-archive-result-drain-design.md docs/superpowers/plans/2026-07-29-ordered-archive-result-drain.md && git commit -m "perf: overlap archive parsing with result application"`

### Task 3: Verify and publish the PR update

**Files:**
- Modify: PR #84 description only after the local commit is pushed.

**Interfaces:**
- Consumes: the completed branch and existing PR #84.
- Produces: a pushed commit and PR description documenting the diagnosed barrier and validation.

- [ ] **Step 1: Run full focused verification**

Run the document-handoff, scheduler, archive-concurrency, and chart-library-scanner tests, build `main`, and run `git diff --check`.

- [ ] **Step 2: Self-review the final diff**

Confirm there is no parser-amalgamation change, checkpoint policy change, backend selection change, unrelated formatting, or deployment action.

- [ ] **Step 3: Push and update PR #84**

Push `perf/dynamic-chart-scan-scheduling`, then update the PR root-cause, scheduling-model, and validation sections with the ordered-drain change.
