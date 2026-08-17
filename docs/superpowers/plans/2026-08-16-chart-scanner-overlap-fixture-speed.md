# Chart Scanner Overlap Fixture Speed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the chart scanner's streaming-overlap guarantees while removing the multi-megabyte timing fixtures that dominate `chart_library_scanner_tests`.

**Architecture:** Add a test-build-only callback at the real ZIP streaming entry boundary. The two overlap tests use the callback as a condition-variable gate: they permit the necessary reader to start, require the first result batch to be applied while that reader is held, then release it. This proves real concurrent pipeline behavior deterministically without padding archive entries or changing the production scanner policy.

**Tech Stack:** C++23, libarchive/miniz ZIP test fixtures, CMake, CTest.

## Global Constraints

- Keep the hook unavailable in production targets; compile it only for `chart_library_scanner_tests`.
- The hook observes real ZIP entry streaming and must not bypass archive extraction, parsing, scheduling, SQLite application, or result ordering.
- Gate waits must have bounded test timeouts and always release before joining the scanner thread.
- Do not create a worktree. Commit only after explicit user authorization.

---

### Task 1: Add a test-build-only streaming-entry observer

**Files:**

- Modify: `CMakeLists.txt:3074-3117`
- Modify: `src/ArchiveFile.h:120-170`
- Modify: `src/ArchiveFile.cpp:520-545,4555-4635`
- Test: `tests/chart_library_scanner_tests.cpp:995-1120`

**Interfaces:**

- Produces `archive_file::setStreamingEntryObserverForTesting(StreamingEntryObserverForTesting)` only when `ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS` is defined.
- The observer receives `(archivePath, entryPath)` immediately before the real streaming reader delivers that ZIP entry to its consumer.
- `chart_library_scanner_tests` is the sole target defining `ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS=1`.

- [x] **Step 1: Write the failing overlap tests**

Replace the padded 128-entry bodies in the two overlap tests with 16-entry ZIP fixtures and a `StreamingEntryGate` test helper. The helper waits with `std::condition_variable::wait_for` for the selected real streaming entry, blocks it until the test releases it, and clears the observer in its destructor. Run scanning on a `std::jthread`; require that a log line for `Inserting streamed DB chart batch:` appears before releasing the held reader.

The later-archive test must hold the first entry of `ordered-drain-second.zip`; the own-stream test must hold entry 13 of `same-archive-overlap-first.zip`, after one 12-result chunk can be scheduled. Both must release the reader before joining and preserve the existing chart-count/cache assertions.

- [x] **Step 2: Run the focused test to verify RED**

Run: `cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6 && ctest --test-dir cmake-build-debug -R '^chart_library_scanner_tests$' --output-on-failure -j 1`

Expected: compilation fails because `setStreamingEntryObserverForTesting` does not exist. The missing symbol demonstrates the test requires an explicit real-reader observation boundary rather than fixture size timing.

- [x] **Step 3: Add the minimal observer implementation**

Add the target-private compile definition in CMake. Under that definition only, declare the following in `ArchiveFile.h`:

```cpp
using StreamingEntryObserverForTesting = std::function<void(
    const std::filesystem::path &, const std::filesystem::path &)>;
void setStreamingEntryObserverForTesting(StreamingEntryObserverForTesting);
```

In `ArchiveFile.cpp`, keep the observer behind a mutex, copy it before invocation, and call it directly before `emitFileData(std::move(file), ...)` in `readZipEntriesByIndexStreaming`. Do not hold the mutex while invoking the callback. This preserves normal reader behavior and lets the test gate a real entry after extraction but before scanner scheduling.

- [x] **Step 4: Run the focused test to verify GREEN**

Run: `cmake --build cmake-build-debug --target chart_library_scanner_tests -j 6 && ctest --test-dir cmake-build-debug -R '^chart_library_scanner_tests$' --output-on-failure -j 1`

Expected: `chart_library_scanner_tests` passes. The old pipeline behavior that withholds result application until all streamed entries finish would time out waiting for the first-insert log while the selected reader remains held.

### Task 2: Measure the retained fixture change and guard against flaky waits

**Files:**

- Modify: `tests/chart_library_scanner_tests.cpp:995-1120` only if timeout cleanup or predicate locking needs correction.

**Interfaces:**

- Consumes the test-only observer from Task 1.
- Produces deterministic overlap checks with no `128 * 1024` fixture padding in either overlap test.

- [x] **Step 1: Run direct repeated timing**

Run: `/usr/bin/time -lp cmake-build-debug/chart_library_scanner_tests` three times.

Expected: all runs exit zero and the retained direct wall time is materially below the 4.44s measured before this plan; the test output still contains every existing assertion.

- [x] **Step 2: Run parallel contention validation**

Run the scanner test beside the previous high-CPU IR target four times:

```sh
for round in 1 2 3 4; do
  ctest --test-dir cmake-build-debug -R '^(chart_library_scanner_tests|ir_submission_service_tests)$' --output-on-failure -j 2 || exit 1
done
```

Expected: all eight test executions pass. This validates that the gate is local to a fixture and does not leak globally across test processes.

- [x] **Step 3: Run the complete parallel suite and inspect the diff**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 6` and `git diff --check`.

Expected: full CTest passes except for a separately documented pre-existing intermittent Metal renderer failure; diff check is clean. Report the exact full-suite result rather than treating the renderer flake as fixed by this work.
