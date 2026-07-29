# Streaming Archive Result Chunks Implementation Plan

> **Execution:** Implement inline in self-review mode; do not delegate or pause
> for approval.

**Goal:** Apply ordered chart results from a large archive while that same
archive is still being streamed and parsed.

**Architecture:** The archive parser publishes discovery-ordered contiguous
chunks into an indexed per-archive channel. The scanner consumes the current
archive's next expected chunk on its database thread and accepts terminal
success only after every chunk has arrived.

**Tech Stack:** C++23, `std::mutex`, `std::condition_variable`, ordered maps,
the existing work scheduler, ChartLibraryScanner integration tests, CMake.

## Constraints

- Keep all SQLite, progress, cache, and checkpoint work on the scanner thread.
- Preserve deterministic archive and archive-entry order.
- Preserve the worker budget, archive-reader limit, archive backend selection,
  and checkpoint interval.
- Restore all-or-nothing rows for an archive that fails after partial delivery.
- Do not change parser amalgamation or image-load logging.
- Do not run a local performance benchmark.

### Task 1: Characterize same-archive overlap

**Files:**
- Modify: `tests/chart_library_scanner_tests.cpp`

- [ ] Add a real stored-ZIP fixture with enough padded charts to keep the first
  archive streaming after its first ordered parse chunk is ready. Add a second
  unprepared archive so the bounded scheduler path is used.
- [ ] Assert the first archive's `Inserting streamed DB chart batch` log occurs
  before its own `Streamed archive batch via miniz ZIP` log.
- [ ] Assert all chart rows and archive-cache counts are correct.
- [ ] Build and run `chart_library_scanner_tests`; confirm the new assertion is
  RED against the full-result implementation.

### Task 2: Publish ordered parser chunks

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`

- [ ] Add an optional pipeline chunk callback carrying the first discovery
  index and a contiguous parsed-chart vector.
- [ ] Move ready prefixes out of parser slots at the chunk threshold and flush
  the remainder at terminal completion, invoking callbacks outside the
  pipeline mutex.
- [ ] Retain complete-vector behavior when no chunk callback is supplied.
- [ ] Build and run the focused test to catch ordering or lifecycle errors.

### Task 3: Consume chunks on the database thread

**Files:**
- Modify: `src/ChartLibraryScanner.cpp`
- Test: `tests/chart_library_scanner_tests.cpp`

- [ ] Extend the per-archive result channel with indexed chunk storage and an
  explicit terminal completion result.
- [ ] For a deferred archive, wait for and apply the exact next chunk while its
  reader/parser work remains active. Accumulate progress, insert count, stored
  chart count, timing, and checkpoints across chunks.
- [ ] On terminal success, write the cache and archive-complete checkpoint. On
  terminal failure after partial application, delete that archive's rows and
  skip the cache.
- [ ] Preserve one-shot prepared and single-large-archive result paths.
- [ ] Run chart-scanner, scheduler, and archive-concurrency tests until GREEN.

### Task 4: Verify, self-review, and publish

**Files:**
- Modify: PR #84 description after pushing.

- [ ] Build `platform_document_handoff_tests`,
  `chart_scan_work_scheduler_tests`, `archive_file_concurrency_tests`,
  `chart_library_scanner_tests`, and `main` with `-j 6`.
- [ ] Run all focused test executables plus `git diff --check`.
- [ ] Self-review ordering, cancellation, partial-failure cleanup, checkpoint
  correctness, and unrelated-file scope.
- [ ] Commit, push `perf/dynamic-chart-scan-scheduling`, update PR #84, and
  verify its head SHA/check status.
