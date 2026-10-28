# Gameplay Frame Stage Timing Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Export per-frame CPU stages and gameplay phases alongside existing Metal presentation timestamps.

**Architecture:** A header-only worker-local collector accumulates fixed stage totals/counts under a display-link token and clock callback. RAII scopes close on early returns. MetalDisplayLink publishes the completed or interrupted collector into its existing ring once per frame; the exporter appends v2 fields.

**Tech Stack:** C++17 collector/native tests, existing C++20 gameplay code, Objective-C++ CAMetalDisplayLink.

**Spec:** `docs/superpowers/specs/2026-10-28-gameplay-frame-stage-timing-design.md`

## Constraints and review focus

- Preserve display-link/input behavior; no deployments, worktrees, whole-file formatting or private paths in exports.
- Rollover, interruption and stale scope destructors must not modify a later frame.
- Nested/repeated stages are inclusive and counted; absent stages remain distinguishable from zero-duration work.
- Capture under contention remains nonblocking and reports dropped updates.
- Gameplay phase changes during update/terminal work remain visible; non-gameplay frames do not inherit metadata.
- Clock reads and allocation are avoided when no token is active; live accumulation is allocation-free.

## Tasks

1. Extend `tests/metal_presentation_timing_tests.cpp` with a failing v2 export assertion. Add deterministic collector cases for nested/repeated stages, early return, interruption/token replacement, invalid clock, inactive collection, phase transitions and rollover. Add allocation observation using existing test support if needed.
2. Add `src/video/FrameCpuTiming.h` with fixed `Stage`, `GameplayPhase`, `GameplayMode`, `FrameData`, worker-local `beginFrame`/`endFrame`/`observeGameplay`, and `Scope` (`finish` supports split capture/projection). Extend `MetalPresentationTiming.h` with bulk CPU publication and v2 export. Tests use a controlled clock, no sleeps.
3. Activate/finish collection in `MetalDisplayLink.mm`. Add scopes at actual call boundaries in `main.cpp`, `GamePlayScene.cpp` and `PlayfieldPresentationCoordinator.cpp`; observe gameplay phase at update entry/exit and rendering, and explicitly mark terminal persistence. Add the small scene observation helper declaration to `GamePlayScene.h`.
4. Run recorder tests, focused presentation-coordinator/lifecycle tests, desktop `main` build and unsigned iOS build. Measure warmed collector overhead/allocation with a bounded local probe. Review diff/integration placement and get an independent final review. Document capture instructions and column meanings, then commit/push the verified changes.

## Progress

- Design follows the user's approved diagnostic scope; implementation and validation are performed in the current checkout.

- Completed collector, native activation/publication, actual call-site scopes and phase observations, including post-notes distinction.
- Baseline export test failed at the required v2 assertion before implementation; final recorder tests pass. Deterministic tests cover nested/repeated/zero-duration scopes, interruption, stale/reused tokens, bad clocks, disabled/thread-local behavior, rollover and allocation-free collection.
- Desktop `main` and all selected targets build. All four selected tests passed: `metal_presentation_timing_tests`, `metal_drawable_provider_tests`, `playfield_presentation_coordinator_tests`, `gameplay_terminal_scene_tests` (12.73 s total at `-j 6`).
- Unsigned iOS build passed through `scripts/ios_firebase_deploy.sh --build-only --skip-init`; no upload performed.
- Independent static review found no actionable issues. Existing compiler/Metal harness warnings remain; no build, assertion or rendering failure occurred.
- Measured overhead, fixed memory size and capture/interpretation instructions are recorded in `docs/testing/gameplay-frame-timing.md`. Measurements ran after builds finished.
- Final recorder rerun also passes CSV column-count/prefix checks and concurrent CPU/event snapshot coherence. Documentation links and `git diff --check` pass.
