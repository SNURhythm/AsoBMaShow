# Find BMS Preview Handoff Implementation Plan

**Goal:** After a matched Find BMS download is indexed, start its preview and
refresh the Ready panel only if chart selection stayed unchanged throughout the
download and parsing interval.

**Architecture:** Capture a UI-thread selection generation at download start,
carry the target identity through the serialized downloaded-path task, resolve
the indexed target with an indexed repository hash query scoped to the exact
committed output, then consume a mutex-protected handoff after library reload.
Apply it through the ordinary selection callback with preview enabled.

**Tech Stack:** C++23, SQLite, CMake/CTest, existing library worker and preview
selection pipeline

## Constraints

- Continue on `agent/incremental-find-bms-scan` in the current checkout; do not
  create a worktree.
- A running or queued full scan remains serialized with downloaded indexing.
- Download completion alone cannot update selection; parsing/indexing must have
  committed first.
- Any intervening chart selection change invalidates the handoff, including an
  away-and-back sequence.
- Title-only and mismatched kept downloads are indexed but never auto-selected.
- Unzip auto-selection continues suppressing preview.
- Do not deploy or install a mobile build.

## Task 1: Add policy regressions

**Files:**

- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `src/scene/MainMenuLibrary.h`
- Modify: `src/scene/MainMenuLibrary.cpp`

1. Add failing tests for a Find BMS target identity, selection-generation
   eligibility, and exact-output path selection. Cover unchanged, changed,
   away-and-back, SHA conflict, MD5 fallback, missing identity, directory
   paths, archive virtual paths, outside paths, and deterministic duplicates.
2. Build `main_menu_library_tests` and confirm the missing policy interfaces
   fail compilation.
3. Implement the small pure helpers and run the focused test binary green.

## Task 2: Add indexed hash lookup

**Files:**

- Modify: `tests/chart_repository_tests.cpp`
- Modify: `src/repositories/ChartRepository.h`
- Modify: `src/repositories/ChartRepositoryInternal.h`
- Modify: `src/repositories/ChartRepository.cpp`
- Modify: `src/repositories/ChartRepositoryQueries.cpp`

1. Add failing repository tests that insert normalized and duplicate chart
   identities, then query by uppercase/whitespace SHA-256 and MD5 fallback.
   Require SHA-256 to remain authoritative and empty/invalid identities to
   return no rows.
2. Add `SelectChartMetaByHash()` as an indexed query returning matching chart
   metadata in stable path order. Normalize and validate hash lengths before
   preparing SQL.
3. Run `chart_repository_tests` green.

## Task 3: Carry and publish the handoff

**Files:**

- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `scripts/check_find_bms_archive_flow.py`

1. Add selection-generation state and increment it when `onSelected` changes
   record identity.
2. Capture the generation immediately before each automatic or explicit
   candidate download. Preserve it through pending-artifact resolution.
3. Extend `LibraryTaskRequest::IndexDownloadedPath` with the target hashes and
   captured generation. Only successful matched downloads receive an automatic
   selection request; kept mismatches keep their existing indexing behavior.
4. After `LoadCharts(..., addedPathsOnly=true)` returns successfully, query the
   indexed target, scope it to `downloadedPath`, and publish a protected pending
   UI handoff before requesting reload. Refactor the incremental runner so the
   reload signal is ordered after handoff publication.
   Require an explicit completed/committed scanner result so a failed database
   write cannot qualify an older matching row at the same stable destination.
   Also require the resolved target path to appear in the result's successful
   upsert set so a parse failure cannot qualify the stale row either.
5. After chart reload, consume the handoff and validate generation plus current
   target identity. Generalize `selectChartByPathAfterReload()` with an explicit
   preview policy; use preview loading for Find BMS and suppression for unzip.
6. Update the source audit to require capture, propagation, post-scan handoff,
   and preview-enabled path selection.

## Task 4: Verify and publish

1. Build and run `main_menu_library_tests`, `chart_repository_tests`,
   `chart_library_scanner_tests`, `find_bms_download_tests`, the Find BMS flow
   audit, and desktop `main`.
2. Run the full CTest suite with failure output.
3. Run `git diff --check`; inspect the entire branch diff and verify the race,
   stale-selection, hash, archive-path, mismatch, and unzip requirements.
4. Commit the scoped changes, push `agent/incremental-find-bms-scan`, and update
   the existing draft pull request with the new behavior and verification.
