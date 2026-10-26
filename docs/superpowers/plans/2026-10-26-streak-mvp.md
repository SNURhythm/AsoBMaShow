# Streak MVP Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in this checkout.

**Goal:** Deliver playable, persistent daily assignments and streak rewards.
**Architecture:** Pure streak model, transactional score-repository integration,
and a native dashboard/editor reachable from the built-in selector.
**Tech Stack:** C++23, SQLite, nlohmann JSON, existing Yoga/native views.
**Spec:** docs/superpowers/specs/2026-10-26-streak-mvp-design.md

## Global Constraints

- Current checkout only; commit and push task-related changes.
- No monetization or distribution actions; no whole-file formatting.
- Use existing profile database and existing score/lamp semantics.
- One build at a time in cmake-build-debug, at -j 6.

## Review Focus

- Retries and recalled results cannot award twice.
- Midnight, absent days, late saves and clock rollback preserve earned rewards.
- Multiple table levels and duplicate chart memberships count correctly.
- Impossible upper score/lamp bounds cannot weaken randomized challenges.
- Profile changes, storage failures and malformed configuration fail safely.

### Task 1: Rules and calendar accounting

Files: src/streak/StreakModel.{h,cpp}, tests/streak_model_tests.cpp,
cmake/StreakTests.cmake, cmake/TestGroupCatalog.cmake.
Interface: State, Config, Assignment, Play; configure, advance, record,
buyFreeze, summary with explicit calendar days and injected random choices.
- [x] Write behavioral tests for distinct charts, thresholds, repeat escalation,
  day protection, freezes, surplus, milestones, delayed saves and config edits.
- [x] Run failing test target, implement model, rerun to green.

### Task 2: Durable progress

Files: src/streak/StreakStore.{h,cpp}, score repository header/projection,
CMake source lists, tests/streak_store_tests.cpp.
Interface: repository LoadStreak, ConfigureStreak, BuyStreakFreeze; internal
RecordOnConnection runs under the score insert's transaction.
- [x] Test persistence, duplicate writes, rollback, table matching and isolation.
- [x] Add versioned state storage and indexed table lookups; wire new score
  projection atomically, using the pre-attempt record and play timestamp.
- [x] Run store tests and the affected results/profile tests.

### Task 3: Native MVP and verification

Files: src/scene/StreakScene.{h,cpp}, MainMenuScene.cpp, ResultScene.cpp,
localization resources, feature documentation.
- [x] Build dashboard, goal controls, multi-table assignment editor and freeze
  purchase using existing responsive native controls.
- [x] Show progress and next-round requirements; acknowledge streak progress in
  results. Persist edits safely and preserve already-earned rewards.
- [x] Build main and affected tests; inspect runnable UI when available.
- [x] Obtain independent final review and fix important findings.
- [x] Commit and push verified work.

## Verification record

- Desktop `main` and affected test targets built successfully.
- 81 results/UI/profile CTests passed; `ir_score_import_projection_tests` also
  passed, including atomic rewards and legacy-identity regressions.
- All four independent-review findings were reproduced by failing tests and
  fixed: deferred callback completion, returned-freeze reassignment, legacy
  chart identity matching, and active-progress validation.
- Initial group run exposed the new catalog-ID convention mismatch (fixed), a
  stale pre-existing selector preview fixture (updated), and a transient
  `foundation_profile_archive_faults` point-9 failure. The latter passed both
  alone and in the final grouped run.
- Native launch succeeded, but desktop automation could not attach to the game
  window (`cgWindowNotFound`), including with a temporary app bundle. No visual
  layout verification is claimed; the smoke-test processes were stopped.
- Policy choices raised by review are documented in the spec and feature guide.
  Rich animation and custom-skin integration remain the agreed later UI/UX pass.
