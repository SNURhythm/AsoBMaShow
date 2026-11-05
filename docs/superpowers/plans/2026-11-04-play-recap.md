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

The pre-existing `responsive_scene_layout_tests` fixture fails to compile because it lacks `View::LayoutBatchScope`, `applyCandidateLoad`, and `candidateLoadTask`; its sources and the scenes it extracts are unchanged by this feature. The initial native window inspection returned `cgWindowNotFound`. The single-receipt revision below was subsequently validated through actual offscreen Metal rendering; mobile Photos/device behavior remains unverified.

## Single-receipt export revision

User requested one receipt-style PNG independent of device resolution. Replace export pagination with a 960-pixel-wide paper-colored receipt containing all sections, wrapped titles, summary counts, and before/after values. Reuse view rendering, capture bounded GPU strips, normalize GL readback orientation, and stream one atomic PNG. Remove page-count and cancellation copy. Validate PNG pixels/CRC/completeness, a 70,000-row image, and actual receipt layout independent of device dimensions; compile main and render a synthetic multilingual receipt for visual inspection.

Revision validation: desktop build and five focused tests pass, including streaming PNG CRC/pixel checks, incomplete-output cleanup, a 70,000-pixel-tall PNG, bottom-up partial-strip normalization, and fixed-width receipt layout taller than 16,384 pixels. Actual Metal exports of synthetic multilingual receipts (960×1986 and 960×6290) were inspected, including both strip joins and the final footer. Temporary smoke instrumentation was removed from `main.cpp` before committing. Fresh review identified GL row orientation; the full texture is now normalized before selecting the final strip's valid rows.

## BP and lamp-group revision

Replace the third section with BP improvements in both browsing and the receipt. Confirmed beatoraja semantics in `play/BMSPlayer.java` (setMinbp) and `ScoreData.java` (strict minimum updates): BAD + POOR + MISS + total notes − passed notes. The app's `captureChartScoreWrite` persists the equivalent metric as `bad_points`. Use valid saved values, including failed plays with remaining-note penalties; only use a missing-value judgement fallback when completion is established. Do not rewrite stored facts.

Group lamp improvements by achieved lamp in descending logical lamp order, preserving alphabetical order within each group. Repeat the group heading at a browse-page boundary and emit it once per group in the unpaginated receipt. Add regressions for empty POOR, BP versus combo-break divergence, failed-play saved penalties, invalid BP, explicit zero, and ordered lamp headings.

BP/grouping verification: desktop `main` build and all five focused recap, receipt, export and localization tests passed. Regression coverage confirms saved-BP precedence, unpassed-note penalties, empty POOR inclusion, malformed BP exclusion, and best-first lamp ordering with one receipt heading per group.
