# Test Sharing Expansion Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan inline. Steps use checkbox syntax for tracking.

**Goal:** Reduce remaining duplicate desktop test objects by extending the explicit source list.

**Architecture:** Reuse the existing compile-contract partitioning helper and existing consumer list. Select additional production sources from measured identical Debug compilation commands; verify the generated Debug and Release commands after configuration.

**Tech Stack:** CMake, Ninja, Python, CTest.

**Spec:** `docs/superpowers/specs/2026-09-24-build-storage-design.md`, extended by the user's approval of the next source-list expansion.

## Global Constraints

- Stay on the current branch and checkout; no worktree or deployment.
- Keep the sharing helper, compile settings, symbols, assertions, fixtures, test inventory, and consumer list unchanged.
- Do not change Android storage or dependency sharing in this increment.
- Remove only exact obsolete objects with verified shared replacements after validation.
- Continue through verification, commit, and push without further approval pauses, as instructed by the user.

## Review Focus

- Every new shared command must match an original command except its object output.
- Existing test-specific definitions and link requirements remain isolated by the unchanged helper.
- Debug and Release configuration differences remain separate.
- Main application compilations and registered tests remain unchanged.
- Cleanup must not remove an object still referenced by either generated graph.

### Task 1: Extend and verify the source list

**Files:** `cmake/SharedTestDependencies.cmake`, `docs/build-storage.md`, this plan.

**Interfaces:** Add production source paths to `SOURCES` in `asobmashow_share_test_sources(dependencies ...)`; preserve `TARGETS` and the helper implementation.

- [x] Capture Debug/Release compile databases, Debug object sizes, and CTest inventory in this plan's ignored workspace. Identify currently unlisted production sources with identical commands in at least two existing consumers.
- [x] Merge only those source paths into the sorted explicit list. Regenerate both existing build trees with `cmake -S . -B cmake-build-debug` and `cmake -S . -B cmake-build-release`.
- [x] Compare normalized commands, application commands, and CTest inventory. Require every new shared command to exist in the prior graph and a reduction in total compilation entries.
- [x] Run `python3 tests/shared_test_sources_tests.py -v`, `cmake --build cmake-build-debug -j 6`, and `ctest --test-dir cmake-build-debug --output-on-failure -j 6`. Diagnose the known parallel audio timeout separately if it recurs.
- [x] Build and run representative affected Release tests; repeat the Debug build to check incremental reuse.
- [x] Obtain one independent review, address material findings, remove only proven retired objects, and record net savings.
- [x] Run `git diff --check`, commit task-only changes, and push the current branch.

Expected: unchanged effective compile settings and test behavior, fewer live objects, passing affected builds/tests, measured savings, and published task changes. This list-only change uses existing fixture coverage and real graph/build verification; no implementation-mirroring unit test is added.

## Outcome

Added 124 production sources; both graphs have 536 fewer entries. Debug replacements
save 273.43 MiB net after retired-object removal. Full Debug build, existing
sharing fixture, three Release tests, and incremental checks passed. CTest passed
402/404 in parallel, with both timeout cases passing isolated reruns. See
`docs/build-storage.md` for the controlled audio comparison and exact measurements.
The independent review found no material issues and verified each executable's
replacement link edge. Validation covers the local macOS Debug/Release graphs.
