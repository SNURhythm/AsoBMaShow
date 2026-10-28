# Personal Chart Goals Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Let players keep a lasting lamp/score target for a chart and inspect exact progress independently of Combo.

**Architecture:** A small goal model and versioned SQLite store sit behind ScoreRepository. A shared ChartGoalsScene provides list/detail/edit flows; native and skin selectors own handoffs to normal play. Best-score and best-clear queries remain authoritative.

**Tech Stack:** C++23, SQLite, nlohmann JSON, existing SDL/Yoga views and localization.

**Spec:** `docs/superpowers/specs/2026-10-28-personal-chart-goals-design.md`

## Global Constraints

- Use the existing `feature/enhance-streak` checkout; no worktree or deployment.
- Goals are profile-owned, keyed by normalized SHA-256 and effective LN mode.
- At least one lamp/score target; no FAILED/NO PLAY goal target.
- Read 20 goals per page; no archive payload scans or replay hydration.
- Reuse eligible best records, including accepted IR imports; practice scores never become eligible through this feature.
- No Combo point/streak changes, deadline, automatic escalation, or result popup.
- Translate UI into all five supported locales; preserve changed-line formatting.

## Review Focus

- Near-threshold scores round upward to the next achievable EX integer, not to a false completion.
- Imported/best-clear records can differ from the highest score; neither replaces the other.
- Effective LN scopes distinguish selectable charts but collapse charts without long notes.
- Corrupt/future rows and write failures preserve existing goals and scores.
- Returning through an embedded Combo tab, custom selector, or practice scene cannot use a destroyed scene or an old profile's pending selection.

## Task 1: Goal model and durable store

**Files:** `src/goals/ChartGoal.h`, `src/goals/ChartGoal.cpp`, `src/goals/ChartGoalStore.h`, `src/goals/ChartGoalStore.cpp`, `tests/chart_goal_tests.cpp`, `cmake/ChartGoalTests.cmake`, `cmake/TestGroupCatalog.cmake`, root `CMakeLists.txt`, `src/CMakeLists.txt`.

**Interfaces:** namespace `chart_goals`; `Target { lamp=-1, scoreNumerator=0, scoreDenominator=27 }`; `Goal { chart: ChartMeta, longNoteMode: int, target: Target }`; `Progress` carries independent optional best-score/best-clear evidence and derived target completion / optional EX gap. `evaluate(Target, optional<ScoreBestSnapshot>, optional<ScoreBestSnapshot>) -> Progress`. Store functions `loadPage(sqlite3*, offset) -> Page`, `load(sqlite3*, sha256, mode) -> optional<Goal>`, `save(sqlite3*, Goal)`, `remove(sqlite3*, sha256, mode)` throw on corruption/storage failure. Page contains goals and hasMore. Only identity/title/subtitle/LN metadata and targets are serialized, not archive paths or payloads.

- [x] Write tests for AAA at maxScore=2000 (1777 is short by 1, 1778 meets), custom 99.99% at 2000 (1999 short by 1), missing/invalid maxScore, separate HARD lamp and higher scoring EASY result, and no-target/invalid-target rejection. Run to observe failure before implementation.
- [x] Implement target validation/projection with integer arithmetic. Verify tests pass.
- [x] Add SQLite tests for case-normalized identity, upsert retaining one row per scope, separate modes, file reopen, independent databases, page boundaries, deletion, corrupt/future payload rejection without replacement, and transaction rollback. Observe failing tests, implement store, and rerun.
- [x] Register target in results/profile groups, build once, and run `ctest --test-dir cmake-build-debug -R '^chart_goal_tests$' --output-on-failure` (expect all pass). Commit the tested deliverable.

## Task 2: Profile repository bridge

**Files:** `src/repositories/ScoreRepository.h`, new `ScoreRepositoryGoals.cpp`, test/build registration.

**Interfaces:** `LoadChartGoals(int offset, string &error) -> optional<chart_goals::ProgressPage>`; `LoadChartGoal(const ChartMeta&, int selectedMode, string &error) -> optional<chart_goals::GoalProgress>` (missing has empty error); `SaveChartGoal(const chart_goals::Goal&, string &error) -> bool`; `RemoveChartGoal(const ChartMeta&, int selectedMode, string &error) -> bool`. GoalProgress carries goal and evaluated progress. All operations use the existing connection lock/activity guards; writes transactionally verify the existing row before replacing it. Reads use current best-score/clear on that connection.

- [ ] Add real repository integration coverage for effective LN scoping, independent eligible bests, profile database changes, imported evidence and persistence failure. Use existing result fixtures/shared dependencies; observe failures before adding the bridge.
- [ ] Implement the bridge and build registration. Preserve existing score schema version and unrelated data.
- [ ] Run goal integration tests plus relevant existing best-record/import tests (expect pass). Commit the tested bridge.

## Task 3: Shared goal UI and selector integration

**Files:** new `src/scene/ChartGoalsScene.h/.cpp`, scene CMake registration; `MainMenuScene.h/.cpp`, `MusicSelectScene.h/.cpp`, `StreakScene.h/.cpp`, `src/i18n/Messages.inc`, UI/navigation tests, `docs/features/personal-chart-goals.md`, feature index.

**Interfaces:** `ChartGoalsScene(context, returnTarget, optional<ChartMetaRecord>, onSelectChart)`; callback receives exact current ChartMetaRecord and goal LN scope. Selectors expose no automatic play; they queue selection then resume their normal UI. Streak receives an optional open-goals callback. Native embedded Combo defers to its parent; standalone Combo returns to its retained selector before opening Goals, avoiding orphan retained scenes.

- [ ] Add behavioral navigation/form tests for valid and empty goals, failed saves preserving edits, page/delete behavior, profile-change reset, and safe deferred navigation. Observe missing behavior before implementation.
- [ ] Implement shared responsive goal list/detail/editor with rational score presets and custom rate, colored lamp targets, exact progress, edit/remove confirmation, missing-chart state, and Practice using the existing chart viewer.
- [ ] Add Personal goal to both chart menus and Personal goals to Combo before its empty-routine early return. Resolve current charts by metadata hash, and hand Open in PLAY back to the originating selector with the correct LN mode.
- [ ] Add translations and feature documentation; update plan checkboxes after verification.
- [ ] Build `main` and the affected test groups in one invocation at `-j 6`, then run focused results/profile/music-select/UI tests; broaden only for unresolved boundary coverage. Check responsive UI if a local app surface is available. Record limits if not.
- [ ] Request one fresh whole-branch review, fix important findings with regression coverage, then commit and push the existing branch to origin (set upstream if absent). Do not deploy.
