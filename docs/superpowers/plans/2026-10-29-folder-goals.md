# Folder Goals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Add live folder/table lamp-percentage and average-EX goals with progress bars.

**Architecture:** A folder-goal model/store owns validated targets and metadata-only membership/progress projection. ScoreRepository wraps profile persistence and atomic achievement credit; a dedicated folder-goal scene integrates both selectors and Personal goals. Existing chart-goal UI and evidence remain intact.

**Tech Stack:** C++23, SQLite, existing retained View/Yoga UI, CMake/CTest.

**Spec:** docs/superpowers/specs/2026-10-29-folder-goals-design.md

## Global Constraints

- Missing charts are excluded from both denominators and shown separately.
- Available unplayed charts contribute 0% EX; rates are equally weighted.
- Live membership; profile and LN scope; no payload scans or per-chart score queries.
- Preserve semantic lamp ordering, eligible imports, source identity, and exactly-once credit.
- Work in the current branch without a new worktree; commit/push verified changes.

## Review Focus

- Table replacement must retain source identity and exclude missing entries.
- Multiple physical copies must not inflate denominators.
- Profile changes must discard pending actions and cached projections.
- Membership/import completion must not produce play achievement credit.
- Compact progress bars must retain readable current/target labels and missing counts.

### Task 1: Model, storage and indexed progress

**Files:** Create src/goals/FolderGoal.h, FolderGoal.cpp, FolderGoalStore.h, FolderGoalStore.cpp; tests/folder_goal_tests.cpp. Update cmake/ChartGoalTests.cmake and TestGroupCatalog.cmake.

**Interfaces:** folder_goals::Source, Target, Goal, Progress, GoalProgress; evaluate(Target, vector<Member>); load/save/remove(sqlite3*, ...); project(sqlite3* scores, sqlite3* charts, Goal).

- [x] Write model/store tests: 3/4 available Easy charts gives 75% with 2 missing; Failed includes failed plays; unplayed zero; 100/200 plus 900/1000 gives 70%; duplicate SHA counted once; empty is not achieved; invalid percentages rejected.
- [x] Run folder_goal_tests and observe failure before implementing.
- [x] Implement validated source/type/mode targets, versioned rows and revision/date preservation; use indexed metadata and score summaries for projection, not attempts/payloads.
- [x] Extend tests for table source replacement, missing/duplicate entries, LN selection/fallback, corruption and exact percentage thresholds; run to PASS.

### Task 2: Profile repository and achievement integration

**Files:** Create src/repositories/ScoreRepositoryFolderGoals.cpp; update ScoreRepository.h/Internal.h/Queries.cpp, result presentation, shared test dependencies and repository tests.

**Interfaces:** PreviewFolderGoal, LoadFolderGoals, SaveFolderGoal, RemoveFolderGoal, TakeFolderGoalAchievements. Store projection reused inside the score transaction; membership is read through a readonly chart connection.

- [x] Add repository tests for save/reload/remove and unchanged versus changed targets; profile isolation; imports do not award; local crossing awards once and recovery never repeats.
- [x] Run focused repository test to observe missing integration failure.
- [x] Implement locked profile APIs, atomic dates/credit and result/Kombo receipts. Before/after score projection must distinguish completion caused by a local play from membership changes.
- [x] Run focused goal and repository tests to PASS.

### Task 3: Folder goal scene, progress bars and selector actions

**Files:** Create src/scene/FolderGoalsScene.h/.cpp and src/view/GoalProgressBar.h; update ChartGoalsScene, MainMenuScene, MusicSelectScene and LibraryFolderItemView; add localized strings using existing locale assets.

**Interfaces:** FolderGoalsScene(ApplicationContext&, SceneReturnTarget, optional<Source>, SelectFolder); SelectFolder receives saved source and LN mode. GoalProgressBar displays current percent and target percent, with semantic color/achieved style.

- [x] Add scene checks for supported source actions in both selectors, folder-goal entry in Personal goals, current/target labels, missing counts and progress-bar layout.
- [x] Run checks and observe missing-feature failure.
- [x] Implement create/edit/details/list, stable source navigation, profile guards, two target-type buttons, lamp dropdown and percent input, progress bars in cards/details.
- [x] Verify focused checks, build main once at -j 6 and inspect layout via available desktop capture if feasible.

### Task 4: Verification and delivery

**Files:** docs/features/personal-chart-goals.md and this plan.

- [x] Document missing-chart policy, live membership, progress and supported folder scope.
- [x] Run affected tests (goal, result persistence, selector integration), diff checks, and desktop main compile.
- [x] Request one fresh whole-branch review; fix material findings and rerun affected checks.
- [x] Commit only task changes and push current upstream. Report evidence and any remaining limitations.

## Completion evidence

- Desktop `main` build passed with `-j 6`.
- Twelve focused CTests passed in 16.17 seconds: folder/model/store, folder scene,
  chart goals/scene, score import projection, Kombo scene, both selector behavior,
  repository projection, selector tabs, responsive scene layout and native view geometry.
- Fresh review found Windows directory separators and selection-triggered repeated
  aggregation. Both were fixed and covered by regression tests. Recursive folder
  navigation and empty-level matching were also corrected.
- Native screenshot inspection was attempted but the available app-control server
  could not bind the standalone debug executable (`cgWindowNotFound`). The test
  process was stopped; compact/desktop bar geometry is covered by native View tests.
- No deployment performed. No worktree created. Private fixtures untouched.
