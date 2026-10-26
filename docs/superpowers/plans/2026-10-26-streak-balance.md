# Streak Balance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make daily completion satisfying and freeze affordability predictable.

**Architecture:** Extend the existing pure streak model and reuse persisted bonus counters. Apply presentation changes in the native dashboard/results without replacing scene infrastructure.

**Tech Stack:** C++, SQLite JSON state, native views, CTest.

**Spec:** `docs/superpowers/specs/2026-10-26-streak-balance-design.md`

## Global Constraints

- Daily bonus: 10 shared, no per-assignment cap; goal-reaching play earns no bonus.
- Freeze price: 20 +5 per purchase, capped at 50; reset on a newly broken streak.
- Preserve old points/counters and existing day/edit/attendance policies.
- One build at a time; no whole-file formatting, deployment or worktrees.

## Review Focus

- Simultaneous completions must not overspend the last bonus point.
- Existing days above 10 bonus points remain readable and cannot earn more.
- Unmet rank/custom-score/lamp thresholds must not become the next target.
- Long purchase histories and late attendance must preserve capped/reset pricing.
- Completed dashboards must hide challenges by default and keep the exit usable.

### Task 1: Reward and challenge rules

**Files:** `src/streak/StreakModel.h`, `src/streak/StreakModel.cpp`, `tests/streak_model_tests.cpp`, `tests/streak_store_tests.cpp`.
**Interfaces:** Add `int bonusPoints(const Day &day)` and `int bonusLimit(const Day &day)`; retain `record`, `freezePrice`, and existing stored fields.

- [x] Add failing behavioral coverage for three overlapping assignments capped at 10; exhausted budget stopping progress; daily reset; score/lamp escalation bounded by the finishing result; fixed one-chart bonus rounds including unfinished/older rounds; 20/25/30 and 50-point ceiling.
- [x] Implement the helper, stable budget allocation and bounded candidate escalation. Preserve old over-budget days and round-trip counters.
- [x] Build model/store targets and run `ctest --test-dir cmake-build-debug -R '^streak_(model|store)_tests$' --output-on-failure -j 6`.

### Task 2: Completion presentation and handoff

**Files:** `src/scene/StreakScene.h`, `src/scene/StreakScene.cpp`, `src/scene/ResultScene.cpp`, `src/i18n/Messages.inc`, feature/spec docs.

- [x] Add the success panel, done action, separate bonus total and collapsed optional challenge section; reset disclosure on profile/day changes. Update result copy and all five locales.
- [x] Build `main tests_results tests_ui ir_score_import_projection_tests` at `-j 6`; run results/UI labels and the projection test. Inspect the native presentation when available.
- [x] Get an independent read-only review; resolve concrete findings, commit and push task files, then update PR #160 with final behavior and verification.

## Execution notes

Model/store regressions passed. The independent reviewer found that a single
assignment still invited bonus play after earning all five available points.
The initial fix accounted for assignment capacity. The user subsequently removed
per-assignment caps entirely, so one assignment can now earn the whole shared
budget; regression coverage and storage validation reflect this final rule.
The user also fixed every bonus round at one chart, superseding count escalation.
Removed the replaced, unused `streak.ui.reached` catalog entry after the catalog
check reported it. Native visual inspection was unavailable because the configured
computer-use CLI returned `[Errno 2] No such file or directory: 'orca'`.

Final policy review reported no findings. The desktop `main` build, all 61 tests
in the results/UI groups and the score-import projection integration test passed
with the final single-chart/shared-only rules. Model/store coverage includes
one assignment earning and persisting all ten points, older partial rounds,
normal quota restoration and overlapping completions at the daily limit.
