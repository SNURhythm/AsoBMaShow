# Goal Challenge Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Prepare chart goals through Challenge!, gauge selection, and a GOAL pacemaker with informative labels.

**Architecture:** Pure goal selection/gauge policy feeds the goal action and pacemaker. A profile-persisted preferred goal ID distinguishes a challenge from manual GOAL selection; selectors cache goal resolution and gameplay freezes the resolved score target.

**Tech Stack:** C++23, existing AppSettings JSON, SQLite goal repository, native view controls, CTest.

**Spec:** docs/superpowers/specs/2026-10-29-goal-challenge-design.md

## Global Constraints
- Preserve current selector folder state and apply the goal LN mode.
- No goal-table rename, rebuild, payload migration, deployment, or worktree.
- GOAL fallback is BEST; labels show AAA / 87.77% / BEST and GOAL: <target> in the dropdown.

## Review Focus
- Two goals on one chart must select the challenged ID, not an arbitrary row.
- Lamp-only, deleted, achieved, other-mode, and other-profile targets must resolve deliberately.
- Auto-shift bounds and legacy lamp aliases must not cause unnecessary gauge changes.
- Manual target changes must clear the explicit challenge and settings must survive reload.
- Completing a goal must not change the target shown on the just-finished result.

## Task 1: Policy and persistence
- [x] Add regressions in chart_goal_tests and app_settings_store_tests for gauge coverage, exact/manual goal selection, no-score fallback, and GOAL settings round-trip.
- [x] Add goals/GoalChallenge.h policy (challengeGauge and scoreTargetForGoals), GOAL pacemaker support, and selectedPacemakerGoalId settings persistence/sanitization.
- [x] Run focused policy/settings tests.

## Task 2: Challenge action and selector labels
- [x] Extend chart goal navigation tests for Challenge settings, profile guards, and unchanged practice behavior.
- [x] Apply challenge settings before returning through existing selector callbacks; rename/localize the action.
- [x] Add cached goal resolution for both selectors, brief bar labels and GOAL: <target> dropdown labels; clear explicit goal IDs on manual selection.
- [x] Verify goal navigation, selector integration, play-option controls, and localization tests.

## Task 3: Gameplay and result integration
- [x] Test exact fractional score conversion, GOAL target progression and fallback.
- [x] Resolve GOAL on gameplay setup and preserve the live target in result overrides; keep replay/export presentation consistent with available goal context.
- [x] Run relevant focused tests and desktop main build at -j 6; review diff, update feature docs, commit and push PR #164.

## Integration review
- Custom-skin targets must not override GOAL or its BEST fallback. Added an explicit runtime authority flag and regression coverage for saved IR and static targets.
- Recalled results resolve their score goal once using the recorded LN mode; live results keep their supplied frozen target. Added a regression covering target stability and excluded modes.

## Verification
- Desktop `main` and affected native test targets built successfully with `-j 6`.
- All 20 selected suites passed, including the skin suite rerun after correcting its test frame serials.
- Recalled-result regression first reproduced the BEST fallback, then passed with the frozen goal target.
- No database migration or distribution action is part of this change.
