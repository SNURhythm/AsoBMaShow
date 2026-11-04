# Play Recap Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Browse and export chart improvements grouped by upgrade kind over selectable periods.

**Architecture:** A recap model resolves local calendar ranges and aggregates persisted local score attempts. A repository wrapper supplies existing eligibility SQL. A responsive scene uses the same snapshot for browsing and bounded PNG export pages.

**Tech Stack:** C++20, SQLite, existing View/Yoga UI and bgfx PNG exporter.

**Spec:** docs/superpowers/specs/2026-11-04-play-recap-design.md

## Global Constraints
- No parser changes, new dependencies, worktrees, or distribution actions.
- Preserve immutable result facts and private profiles.
- Local-only, chart/LN-mode-separated comparisons with half-open boundaries.

## Review Focus
- Aborted zero-break plays must not improve combo records.
- DST and invalid calendar input must not silently shift selected dates.
- Same chart in multiple sections, repeated upgrades, and legacy lamps.
- Imported/modified results must not distort eligible improvements.
- Pagination, export interruption, deleted chart titles, profile changes.

### Task 1: Recap model and repository
- [x] Add focused in-memory SQLite tests for baseline/window aggregation, independent eligibility, LN separation, completion, sessions and calendar ranges.
- [x] Run the tests before implementation to establish failure.
- [x] Implement `recap::Snapshot`, time-range helpers, and `recap::load(sqlite3*, Range, scoreSql, lampSql, clearSql)` in `src/recap/PlayRecap.h` and `PlayRecapQuery.h`.
- [x] Expose `ScoreRepository::LoadPlayRecap` in the existing query translation unit.
- [x] Run the focused target and register in `area_results`.

### Task 2: Browse and export
- [x] Add `src/scene/PlayRecapScene.{h,cpp}` with period controls, grouped section rows, navigation, empty/error states, PNG export and cancellation.
- [x] Add entry points in both selectors, source registration, localized strings, and feature documentation.
- [x] Compile `main`, run relevant results/UI checks, review the diff and fix findings.
- [x] Commit and push the task changes to the current upstream.

## Verification

Desktop `main` and `play_recap_tests` build successfully. Seven focused CTest cases pass: recap, score cache, localization catalog, selector tabs, selector records lifecycle, partial image exporter, and skin export. Fresh review findings were fixed (legacy mode baselines, unresolved chart identity, resize cancellation, partial-export location).

The pre-existing `responsive_scene_layout_tests` fixture fails to compile because it lacks `View::LayoutBatchScope`, `applyCandidateLoad`, and `candidateLoadTask`; its sources and the scenes it extracts are unchanged by this feature. A desktop launch succeeded, but native window inspection returned `cgWindowNotFound`, so visual validation and mobile device export remain unverified.
