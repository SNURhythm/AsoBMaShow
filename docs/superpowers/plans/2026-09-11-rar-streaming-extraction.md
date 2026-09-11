# Streaming RAR Extraction Implementation Plan

> **For agentic workers:** Execute inline with the executing-plans workflow. Keep native builds and measurements serialized.

**Goal:** Improve non-solid RAR full extraction without buffering complete output files, and quantify release-build performance against commit `205b9250`.

**Architecture:** Reuse private 7-Zip handlers and the existing full-extraction output callback, distributing independent RAR entries among a bounded set of workers. Preflight SDK paths, types, sizes, and solid flags against the cached index before selecting this route; retain the existing extraction route when interpretations differ. Solid streams retain sequential decoding and the existing bounded writer pipeline.

**Tech Stack:** C++23, 7-Zip SDK, existing CMake/CTest targets, RAR-generated fixtures.

**Spec:** User-approved proposal in this conversation: release profiling followed by streaming non-solid RAR extraction. Independent 7z block scheduling and dynamic resource redistribution are subsequent work, not prerequisites for this bounded pass.

## Global Constraints

- Work in the existing checkout; do not create a worktree.
- Commit and push completed, verified work per the user's standing delivery preference. Do not deploy without an explicit request.
- Keep actual expanded-byte limits, free-space reservations, CRC validation, cancellation, filesystem-alias serialization, and incomplete/completion markers intact.
- Do not change parser code, cached library-reader configuration, or unrelated tests.
- Use `apply_patch`; do not run whole-file formatting.
- Keep the five pre-existing September 8 review documents untouched.

## Task 1: Establish a reproducible release baseline

**Files:** Existing ignored benchmark directory and a new release-performance report under `docs/reviews/`.

- [x] Build the existing `archive_unzip_operation_tests` target in `cmake-build-release`, without reconfiguring desktop debug settings.
- [x] Link a benchmark against that target's production release objects, keeping benchmark assertions enabled. Preserve the baseline executable before editing production.
- [x] Generate deterministic non-solid/solid RAR and LZMA1/LZMA2 fixtures with the installed archivers. Use matching expanded payloads, remove outputs before iterations, and compare every output byte outside the extraction timer.
- [x] Capture whole-operation extraction time and process CPU/RSS. Sample a representative release run to distinguish decoder work from filesystem and synchronization overhead; do not infer a bottleneck from wall time alone.

## Task 2: Stream independent RAR entries concurrently

**Files:** `src/ArchiveFile.cpp`, `tests/archive_file_concurrency_tests.cpp`, and portable fixture data under `tests/fixtures/archive/` when needed.

**Interface:** Private optional-result helper, with `nullopt` meaning decline before output writes, `false` meaning an attempted extraction failed, and `true` meaning all selected files completed.

```cpp
std::optional<bool> extractRarArchiveFullyConcurrently(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputFolder,
    const CachedIndex &index, const UnzipExecutionPlan &execution,
    const std::stop_token *stopToken, const UnzipProgressCallback &progress,
    const PauseCallback &pause, std::string *errorMessage,
    UnzipWriteGuard &writeGuard);
```

- [x] Add a real RAR fixture regression. With `maximumWorkers = 4`, require multiple distinct extraction-progress threads, monotonic aggregate completion, matching payloads, and exact `budget.writtenBytes`; first observe failure against the baseline's sequential decode callback.
- [x] Add serial/solid controls, cancellation after extraction starts, exact/insufficient byte limits, corrupt-payload rejection, and alias/index-name fallback cases. Assert originals and incomplete markers remain after failure and completion markers are absent.
- [x] Preflight every selected entry on a fresh SDK handle: a safe normalized path must equal the cached path, file/directory status and sizes must match, and no selected file or archive may be solid or encrypted. A different backend's index must not silently change output names.
- [x] Use independent handles, deterministic size-balanced assignments, exception-safe `jthread` joins, and one shared accounting guard. Profiling superseded the initial direct-output/chunk-pull design: one bounded shared writer avoids write-lock contention, reserves one worker and its queue memory, and serializes staging across producers. Each decoder makes one extraction call, reusing its dictionary across assigned entries rather than recreating it for small chunks.
- [x] Serialize aggregate progress and stop sibling work promptly on cancellation, callback failure, resource exhaustion, or decoder failure. Do not fall back after an attempted concurrent extraction fails.
- [x] Route eligible RAR before the existing full backends. Preserve existing fallback for unsupported/preflight-declined cases and create directories/finalize markers through existing logic.
- [x] Run focused archive backend/operation/modal tests; review the changed code for cross-backend filename and callback/thread-lifetime hazards.

## Task 3: Measure, review, and verify

**Files:** Release benchmark report and `docs/skin-compat/beatoraja-music-select-gaps.md`.

- [x] Repeat paired baseline/new release measurements with reversed process order and byte verification, including a no-regression solid control. Report medians, fixture characteristics, and limits; do not promise a speedup for unmeasured formats or devices.
- [x] Run repeated focused tests, desktop main/all-target builds, and parallel full CTest. All 357 tests passed, including `visual_catch_up`.
- [x] Run unsigned iOS and Android build-only checks sequentially, never distribution actions.
- [x] Obtain a read-only correctness review, resolve introduced findings, then run `git diff --check` and report actual verification/commit status. Both RAR4 allocation findings have red/green regressions; delivery follows the user's standing commit-and-push preference.
