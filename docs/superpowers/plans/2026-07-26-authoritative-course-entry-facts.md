# Authoritative Course Entry Facts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make one full-course snapshot of chart identity, random branch, selected-LN note count, and play length authoritative for gameplay, result presentation, score persistence, replay persistence, and recall.

**Architecture:** `CoursePlaySession` owns a complete vector of authoritative `ChartMeta` snapshots alongside its source entries and exposes read access with a compatibility fallback for older/manual sessions. A normal course start parses every stage once, applies course constraints and the selected LN mode, then atomically installs the snapshots; later stage parses use the saved parser random metadata. Persisted result and replay recall rebuild this vector from stored entry facts and stage identities, and every max/full-combo consumer reads through the session API.

**Tech Stack:** C++23, bms-parser, SQLite, existing assertion-style C++ test executables.

## Global Constraints

- Scope is course facts, max-score, identity, random-branch, persistence, and recall consistency only.
- Preserve all unrelated dirty-worktree changes.
- Do not commit or push.
- Use the selected course LN mode for every frozen stage.
- A later stage must be parsed with the same BMS random branch frozen before stage one starts.

---

### Task 1: Session Snapshot Contract

**Files:**
- Modify: `src/CoursePlaySession.h`
- Test: `tests/remote_result_scene_tests.cpp`

**Interfaces:**
- Produces: `CoursePlaySession::installAuthoritativeEntryMetas(std::vector<bms_parser::ChartMeta>)`, `entryMeta(std::size_t)`, `authoritativeEntryMetasComplete()`, and `totalChartCount()`.

- [ ] **Step 1: Write the failing test**

Add a test that creates stale source entries, installs distinct authoritative metadata, mutates completed-stage metadata, and asserts result entry facts and aggregate metadata continue to use the installed snapshots.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target remote_result_scene_tests -j 6 && ./cmake-build-debug/remote_result_scene_tests`

Expected: compilation fails because the snapshot API does not exist.

- [ ] **Step 3: Write minimal implementation**

Add a private-by-convention full vector owned by the session. Install only vectors whose size exactly equals `entries.size()`; return authoritative values when complete and source-entry values otherwise. Change result fact helpers to enumerate `totalChartCount()` and `entryMeta(index)` without completed-result overrides.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 1 command and expect PASS.

### Task 2: Freeze and Replay the Same Random Branch

**Files:**
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Test: `tests/gameplay_ruleset_policy_tests.cpp`

**Interfaces:**
- Consumes: `installAuthoritativeEntryMetas`, `entryMeta`, and `authoritativeEntryMetasComplete` from Task 1.

- [ ] **Step 1: Write the failing test**

Add a parser-backed fixture with BMS `#RANDOM` alternatives and long notes, freeze the selected branch after applying the selected LN mode, reparse through the saved `ChartMeta`, and assert random values, identity, total notes, and play length match exactly.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target gameplay_ruleset_policy_tests -j 6 && ./cmake-build-debug/gameplay_ruleset_policy_tests`

Expected: compilation fails because the course snapshot preparation helper does not exist.

- [ ] **Step 3: Write minimal implementation**

Prepare every normal-course entry before entering gameplay, apply constraints and effective LN mode, atomically install all resulting metas, retain the first prepared chart, and parse later stages with `play_options::parseChart(*entryMeta, ...)` in both transition paths.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 2 command and expect PASS.

### Task 3: Persistence, Identity, Recall, and Full Combo

**Files:**
- Modify: `src/CourseIdentity.cpp`
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/ResultRecallBuilder.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Test: `tests/score_provenance_db_tests.cpp`
- Test: `tests/result_recall_builder_tests.cpp`
- Test: `tests/remote_result_scene_tests.cpp`

**Interfaces:**
- Consumes: session snapshot accessors from Task 1.

- [ ] **Step 1: Write the failing tests**

Add literal-expectation tests proving score-row `max_score`, course identity, replay `entryFacts`, aggregate result metadata/full-combo threshold, and recalled incomplete-course facts all use authoritative snapshots even when source or completed metadata disagree.

- [ ] **Step 2: Run tests to verify they fail**

Build and run `score_provenance_db_tests`, `result_recall_builder_tests`, and `remote_result_scene_tests`; expect assertion failures from stale source/completed metadata winning.

- [ ] **Step 3: Write minimal implementation**

Route course-key generation, score max calculation, replay stage metadata, aggregate presentation, and course counts through the session snapshot accessors. Install restored snapshots after persisted-result and replay-session construction.

- [ ] **Step 4: Run tests to verify they pass**

Build and run the three focused targets and expect PASS.

### Task 4: Focused Regression and Diff Audit

**Files:**
- Verify only; no planned new files.

**Interfaces:**
- Consumes: all behavior from Tasks 1-3.

- [ ] **Step 1: Run focused test suite**

Run the four touched test executables plus `cmake --build cmake-build-debug --target main -j 6`.

- [ ] **Step 2: Audit mutations and diff**

Check that replacing snapshot access with source entries, reparsing by path without stored random metadata, or overriding facts from completed results would fail at least one regression test. Review `git diff --check` and the diff against `develop`, preserving unrelated changes.

- [ ] **Step 3: Report without committing**

Report modified files, red/green evidence, final test commands, and any environmental limitation to the parent agent.
