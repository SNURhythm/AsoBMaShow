# Streak Motion Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for inline implementation.

**Goal:** Motivational native streak UI and automatic result progress drawer.
**Spec:** `docs/superpowers/specs/2026-10-26-streak-motion-design.md`
**Architecture:** Pure reusable UI motion tracks; reusable vector progress/spark
views; redesigned StreakScene; separately owned result progress overlay whose
eligibility and one-shot lifecycle are independent of rendering.

- [x] Inspect view rendering, result persistence/lifecycle and native preview
  setup. Validate the design with a read-only builder review.
- [x] Add failing tests for deterministic motion and drawer eligibility/dismissal;
  implement the reusable motion and progress components.
- [x] Redesign dashboard, completed/empty states and routine/assignment editors,
  retaining all model behavior and controls with localized concise copy.
- [x] Replace result MVP text with animated overlay, including persistence retry,
  result skins, input capture, safe-area/resize and cleanup behavior.
- [x] Verify focused tests and desktop build; inspect rendered UI and fix issues.
  Independently review the complete diff.
- [x] Commit and push task changes to feature/streak and update PR #160.

## Verification

Desktop `main` and the results/UI test targets build successfully. All 61
results/UI CTests pass; the final styling changes also pass the focused scene
and localization checks. Native Metal offscreen previews using extracted
production dashboard/drawer builders were inspected at phone and desktop
sizes, in light/dark themes and incomplete/complete/empty states. Native window
capture was unavailable, so these were isolated render previews, not device
screenshots. Earshot's artifact audit passes for all three bundled cues.

Independent review identified four issues, each reproduced before correction:
phone drawer sizing (`streak_model_tests`), polled skin input and browser actions,
shared audio-cache budget, and midnight transaction validation
(`streak_scene_tests`). All regressions now pass. Ordinary routine edits retain
tomorrow's effective date; the explicit adjustment changes only today and
preserves partial achievements and rewards. No findings remain deferred.
