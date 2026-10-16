# Framework-wide Idle UI Scheduling Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for integration and focused parallel workers for independent widget/scene migrations. Steps use checkboxes for tracking.

**Goal:** Skip unchanged UI drawing across built-in screens while preserving immediate rendering, animation, input, services, and native lifecycle behavior.

**Architecture:** Shared presentation revisions and render-time deadline collection drive a pre-render gate. Views invalidate on effective changes; scenes declare custom continuous drawing. Existing batching remains, and post-render whole-frame comparison is removed.

**Tech Stack:** C++23, SDL3, Yoga, bgfx, CMake/CTest, native Android/iOS integration.

**Spec:** `docs/superpowers/specs/2026-10-16-ui-idle-scheduling-design.md`

## Global Constraints

- Preserve appearance, interaction, 120 Hz animation support, and 1 ms input polling.
- Apply the framework on desktop, Android, and iOS, not only the main menu.
- Preserve native background/surface/export ownership and frame-retirement rules.
- Do not introduce per-widget display-list caches, a new UI toolkit, or a visual redesign.
- Work in `feature/ui-idle-scheduling`; no worktree, app/device launch, or deployment.
- One CMake/Ninja build per build directory; build and CTest parallelism is 6.

## Review Focus

- Same-size surface recreation and iOS interruptions must not acknowledge an invisible frame.
- Callback-driven destruction and worker completion must not retain or dereference expired views.
- Stationary animation phases need future deadlines without continuous repainting.
- Course pause, running media and time-driven skins must not be mistaken for idle UI.
- Equal setter calls and render-time preparation must converge instead of invalidating forever.

## Tasks

### 1. Shared presentation schedule

Files: create `src/ui/PresentationSchedule.h`, `tests/ui_presentation_schedule_tests.cpp`; update `CMakeLists.txt`.

Interfaces: `ui::invalidate()`, `ui::notifyCompletion()`, `ui::requestAnimationFrame()`, `ui::requestRedrawAt(uint64_t)`; `ui::FrameDemand`, `ui::FrameDemandScope`, `ui::PresentationSchedule` with explicit revision snapshots and successful-frame acknowledgment.

- [x] Add fake-clock tests for dirty/idle/deadline/continuous/maintenance behavior, mutations during drawing, completion races, failed frames, nested scopes and thread isolation; observe failure.
- [x] Implement the header-only schedule and run focused tests.

### 2. View invalidation and widget demand

Files: `src/view/View.{h,cpp}`, basic widget setters, text/input/image/scroll/recycler/keybomb widgets, image decode coordinator, corresponding tests.

Consumes: task 1 invalidation and frame-demand functions. Produces: equal-value-safe mutations and deadlines from visible drawing; cross-thread completion signaling with no View references.

- [x] Add real View mutation tests proving equal values settle and changed geometry/style/structure wakes drawing.
- [x] Add invalidation to base View and custom widget mutations, preserving surrounding formatting.
- [x] Add animation/hold/blink requests and image completion signaling; test timing and lifetime cases.

### 3. Scene activity and custom visuals

Files: `src/scene/Scene.h`, `SceneManager.cpp`, built-in scene headers/implementations, custom View setters, scene fixtures.

Consumes: task 1 invalidation. Produces: `Scene::needsContinuousPresentation() const` and explicit change notifications for custom visuals.

- [x] Test static versus continuous activity for settings, chart listening and ordinary/course pause using production logic.
- [x] Migrate all built-in scene activity; retain conservative continuous behavior for time-driven skins.
- [x] Audit custom drawing setters and service-driven refreshes; ensure no-op updates settle and completions still change the UI.

### 4. Application gate and batching simplification

Files: `src/main.cpp`, `src/rendering/UiBatchRenderer.{h,cpp}`, batching and lifecycle fixtures, `src/perf/LatencyTelemetry.h` if needed.

Consumes: task 1 schedule and task 3 scene contract. Produces: platform-independent pre-render skip with unchanged input/service pacing.

- [x] Add an integration test that counts scene drawing calls across idle updates, deadlines, invalidation and failed presentation.
- [x] Gate rendering before command construction; force scene/window/lifecycle/export restoration and acknowledge only successful presentation.
- [x] Remove old whole-frame recording/comparison and its obsolete tests; preserve immediate batching and retained CPU fallback.
- [x] Add gated update/drawing timing telemetry for subsequent measurement if existing telemetry lacks these stages.

### 5. Verification and delivery

- [x] Review the full diff for missing invalidation, permanent animation requests, native ordering and service regressions.
- [x] Build desktop and affected test targets; run focused tests, then full `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
- [x] Run iOS and Android build-only checks with repository helpers; no upload.
- [x] Resolve failures, run `git diff --check`, commit task-related changes and push the new branch upstream.
- [x] Report coverage, verified behavior and remaining service-polling/continuous-skin limitations without claiming measured battery savings.

## Verification record

- Desktop application and all test targets built successfully with `-j 6`.
- Full CTest run exercised 500 tests. One extraction fixture needed the new
  scheduler header; after that fix, its focused rerun and `--rerun-failed` passed.
  All 500 tests are verified.
- iOS unsigned build passed via
  `scripts/ios_firebase_deploy.sh --build-only --skip-init`.
- Android restricted-access release APK build passed via
  `scripts/android_firebase_deploy.sh --build-only`.
- Independent review found the missing uncapped idle wait and themed-button
  invalidation; both were reproduced, fixed, tested, and reviewed again.
- A real View fixture performs 120 service ticks with only initial and scheduled
  deadline drawing, then wakes on a programmatic size change. Scene activity and
  custom mutation fixtures use extracted production methods, including skins
  enabled and disabled. Static scene update/render paths were also audited.

The app and devices were not launched, and no distribution was performed.
Interactive whole-scene behavior and CPU/battery savings remain unmeasured.
Service polling, actual animations, continuous skins/media, and the one-second
resource-maintenance frame are intentionally retained.
