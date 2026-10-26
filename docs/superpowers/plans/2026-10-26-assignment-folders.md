# Assignment Folders Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for inline implementation.

**Goal:** Add a daily assignments selector root and per-assignment folders.
**Architecture:** Share model eligibility; isolate candidate projection in
MusicSelectAssignmentFolders; load indexed candidates through the existing
metadata loader; integrate lazy Container navigation in MusicSelectScene and
expandable sidebar folders in MainMenuScene. Scope native paginated queries
through a bound hash allowlist.
**Spec:** `docs/superpowers/specs/2026-10-26-assignment-folders-design.md`

- [x] Extend projection tests for root/subfolders, deduplication, counted-chart
  exclusion, sticky/late eligibility, unavailable rows and shared budget. Observe failure.
- [x] Implement shared eligibility and pure folder projection; add indexed lazy
  loading, selector refresh and day rollover.
- [x] Update user documentation; run music-select/library/results/UI groups plus desktop
  compile and relevant integration checks. Independently review changes.
- [x] Commit/push task changes and update PR #160.

## Review regressions

- Expanding the selected root cleared its native query cache without reloading
  rows. The real select-folder/random-play fixture failed before rebuilding the
  allowlist on that early-return path, and passes with the fix.
- An unnamed difficulty level was interpreted as the whole table. A real
  repository fixture containing empty and named levels reproduced the error;
  explicit exact-level filtering now selects only the unnamed level.
- Final independent review reported no actionable findings.

## Verification

- Desktop `main` build passed with the compact localized folder label.
- The four affected CTest groups cover 150 tests: 147 passed initially; the three
  fixture/catalog failures passed after fixture updates and the label change.
- Native and skin-driven day-rollover fixtures pass. Repository regressions
  cover empty levels, sticky charts, scoped counts, search, pagination and random
  selection. Projection regressions cover deduplication and assignment budgets.
- Updated the existing preview fixture for its production mono-audio API.
- A stale generated Swift `.priors` cache blocked the unrelated translation test
  build; backing it up allowed that test to build. No Swift source was changed.
