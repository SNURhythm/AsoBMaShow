# Selector Paging Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for task-by-task implementation; isolated helpers may run in parallel with non-overlapping file ownership.

**Goal:** Make recursive physical-folder selection responsive with bounded full-record loading.

**Architecture:** A compact globally ordered selector index supplies full chart records through a shared 128-row, six-page cache. Indexed bar access replaces eager song vectors; a cancellable loader prepares the index without blocking scene input.

**Tech Stack:** C++23, SQLite, SDL, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-09-08-selector-paging-design.md`

## Global Constraints

- No new worktrees, branches, deployment actions, parser edits or whole-file formatting.
- Preserve MainMenuScene cache behavior and existing selector ordering/filter semantics.
- Write failing tests before implementation; serialize shared build-directory use.
- Keep unrelated `docs/reviews/` files untouched.

## Task 1: Shared bounded cache

- [x] Add standalone tests for 128-row page boundaries, six-page eviction, leading records, invalid indices and retry after loader errors.
- [x] Extract the existing MainMenuScene cache into a reusable indexed page cache with a page-loader callback.
- [x] Keep the MainMenuScene adapter's count and leading-record behavior unchanged; run tests and compile main.

## Task 2: Narrow repository reads

- [x] Add repository tests comparing narrow selection fields and raw query ordering, including direct/nested folders and cancellation.
- [x] Add `Session::VisitChartMetaSelection(folder, visitor, stop)` without collecting complete records.
- [x] Add `Session::HasChartMetaForFolderOrParentFolder(folder, stop)` with OR semantics, preserving the existing parent-only API.
- [x] Run repository tests and verify rich-only fields are not decoded by the narrow visitor.

## Task 3: Indexed bar access

- [x] Add a 100,000-row fake provider and assert opening/navigation/rendering perform bounded indexed accesses.
- [x] Add `MusicSelectRowProvider`, bar-state indexed access helpers and per-directory provider installation.
- [x] Preserve eager snapshots and navigation; test close, reopen, configure, refresh, wrap and jump behavior.

## Task 4: Compact selector index and page projection

- [x] Differential-test compact ordering against existing eager projection/bar-manager behavior.
- [x] Implement global deduplication, filter resolution and stable sorting using narrow fields and immutable score caches.
- [x] Fetch requested paths in bounded batches and construct only their song bars, including replay existence.
- [x] Test duplicate representatives, cache eviction, missing records and all score/filter modes.

## Task 5: Scene integration and cancellation

- [x] Add deterministic loader tests for cancellation, stale completion and retry.
- [x] Prepare song-folder indices on a worker; install only current results and retain responsive navigation while pending.
- [x] Replace scene/property vector-only accesses with indexed helpers; category folders install immediate children only.
- [x] Keep raw folder status and explicit autoplay semantics independent from paged display.

## Task 6: Verification and performance

- [x] Run a reproducible large-library benchmark and record stage timings and bounded-work counts.
- [x] Build `main` and all test targets, then run `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
- [x] Review changes for lifecycle races, stale references, parity and unnecessary materialization; fix confirmed findings and repeat focused tests.
- [x] Commit verified changes and push once when the implementation is complete.

## Verification Results

- Desktop `main` and all test targets build successfully.
- Full parallel CTest run: 327/327 passed (45.17 seconds).
- Scene source fixtures cover 23 cases, including restoration, cancellation,
  autoplay intent, foreground resume and failure-state completion rejection.
- `scripts/ios_firebase_deploy.sh --build-only --skip-init` succeeds for unsigned
  iOS arm64, including all newly discovered selector sources. No upload occurs.
- Independent reviews and regression tests resolved stale references, filter
  fallback cache invalidation, navigation restoration and lifecycle findings.
- No device/UI smoke test or Android build was run for this change.
