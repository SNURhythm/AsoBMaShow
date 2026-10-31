# iOS frame pacing recovery implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan.

**Goal:** Enable Improved frame pacing in lane preview and offset calibration,
and recover eligibility when the player explicitly resumes or retries gameplay.

**Architecture:** Query a scene capability separately from orientation locking.
Keep the existing lifecycle generation guard and displayed-frame handoff; add an
explicit recovery request from gameplay actions to the render-owner policy.

**Tech Stack:** C++23, SDL3, existing Objective-C++ Metal display link.

**Spec:** The user requests lane preview/calibration eligibility, explicit
Resume/Retry recovery, a new branch, and a ready-for-review PR. The saved Off
preference remains authoritative. Returning from inactivity alone does not rearm.

## Global constraints

- Work in the existing checkout on a new branch; do not create a worktree.
- Keep orientation locking confined to gameplay and detach before unlocking.
- No native Metal calls from UI actions: recovery only renews policy eligibility.
- Keep lifecycle, geometry, export, frame-cap, and platform availability guards.

## Review focus

- Recovery requested while inactive must not silently arm on foreground return.
- Repeated eligibility polling must not recover an interrupted session.
- Retry Same, new-pattern Retry, replay retry, and course restart need recovery.
- Leaving preview/calibration must restore ordinary rendering.
- A successful Resume must recover without resetting an already healthy link.

## Task 1: Scene eligibility and explicit recovery

**Files:** `src/platform/IOSApplicationLifecycle.h`, `src/main.cpp`,
`src/context.h`, `src/scene/Scene.h`, `src/scene/SceneManager.cpp`,
`src/scene/SettingsScene.h`,
`src/scene/SettingsScenePreview.cpp`, `src/scene/play/GamePlayScene.h`,
`src/scene/play/GamePlayScene.cpp`, existing lifecycle/gameplay fixtures.

**Interfaces:** `Scene::usesImprovedFramePacing() const`,
`ApplicationContext::requestImprovedFramePacing`, and
`IOSSceneMetalPolicy::requestRecovery(IOSPresentationState)`.

- [x] Extend lifecycle tests for explicit recovery and inactive rejection;
  extend gameplay action coverage for recovery notification. Observe failure.
- [x] Decouple eligibility from orientation locking. Poll the foreground scene
  capability before acquiring a frame; preview also covers calibration.
- [x] Request recovery on preview entry and successful Resume/Retry actions.
- [x] Update the settings help text and feature documentation.

## Task 2: Verify and publish

- [x] Run focused lifecycle/gameplay/native Metal/localization tests.
- [x] Compile desktop `main` and unsigned iOS using the repository helpers.
- [x] Review the final diff. Delivery is a commit pushed on the new branch and
  a ready-for-review PR to `develop`.

## Verification record

- Resume notification regression failed against the old gameplay action; the
  new lifecycle recovery API was absent. Both pass after implementation.
- Nine focused CTest checks passed, including native drawable handoff and
  lifecycle, gameplay, scene, settings, and localization coverage.
- Desktop `main` and unsigned iOS builds passed.
- Independent read-only review found no actionable correctness issues.
- Scope decision: Resume/Retry means the gameplay controls. Preview/calibration
  reentry recovers eligibility; their separate Play/Pause/Start controls are not
  recovery actions. Direct/Composited status needs physical-device verification.
