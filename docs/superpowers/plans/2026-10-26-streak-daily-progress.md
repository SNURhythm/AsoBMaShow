# Daily Streak Progress Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for inline implementation.

**Goal:** Preserve daily eligibility and combine target achievements across eligible attempts.
**Architecture:** Split eligibility from target matching in the pure model, persist actual per-chart achievements, and reuse the existing atomic score-projection path.
**Tech Stack:** C++, SQLite JSON state, CTest.
**Spec:** `docs/superpowers/specs/2026-10-26-streak-daily-progress-design.md`

## Constraints and review focus

- Full eligibility is sticky only after it is satisfied; ineligible attempts grant no retroactive achievements.
- Targets may combine across attempts, but raised targets compare actual best lamp/EX rate.
- Used charts still count once; new days and profiles do not inherit progress.
- Version 1/2 migration preserves balances/history; version 3 corruption is rejected.
- Preserve the latest shared-budget, one-chart bonus and LN-fallback policies.

## Tasks

- [x] Add model regressions for EASY→NORMAL→HARD, FAILED→EASY→HARD, changed table membership, combined targets, raised targets, duplicate credit and next-day reset; observe failure.
- [x] Add `ChartAchievement` and `Progress.chartProgress`; separate eligibility and achievement matching, update actual best values and count only once all targets are met.
- [x] Add version 3 serialization/migration and store regressions for reopen, rollback, invalid achievements and old-state upgrades. Add a saved-score integration sequence for sticky eligibility.
- [x] Update feature/spec documentation and localized eligibility/target help.
- [x] Build `main tests_results tests_ui ir_score_import_projection_tests -j 6`; run results/UI groups and projection integration. Review, commit/push and update PR #160.

## Verification

- Model regressions failed before implementing sticky eligibility/accumulation;
  delayed earlier ineligible saves also failed before adding the timestamp guard.
- Desktop `main` and affected test targets built successfully.
- All 61 results/UI tests and the score-import projection integration test passed.
- Independent read-only review reported no findings.
