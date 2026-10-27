# Metal HUD findings implementation plan

> Completed using superpowers:executing-plans in the existing checkout.

**Goal:** Address the rendering overhead, missing iOS drawable limit, and measurement gaps identified from the M5 Metal HUD.

**Architecture:** Keep bgfx changes in generated-source compatibility patches. Reuse bounded UI upload caching and the existing latency histograms. Retain the software pacing mechanism and display-refresh policy, account for synchronous drawable waits in the frame budget, and observe real presentation separately.

**Tech Stack:** C++23, Objective-C++/Metal, bgfx, CMake, native GPU readback tests, Python build setup tests.

**Spec:** The conversation's HUD/code review and the user's request to fix all suggestions. A screenshot does not prove a leak or identify a latency root cause.

## Global constraints

- Work on `fix/metal-hud-rendering-overhead` in the current checkout; no worktree.
- No deployment.
- Preserve positive blur, brightness, opaque fullscreen composition, export behavior and lifecycle safety.
- No unbounded caches, payload scans, full-file formatting, or guessed latency claims.
- Device is currently unavailable; report physical-device validation separately.

## Review focus

- Blur off/on/off and resize must never sample stale targets.
- Removing blur must preserve transparent source RGB and opaque final output.
- UI buffer growth, index changes, vertex formats and opacity must invalidate reuse correctly.
- iOS layer changes stay on UIKit main; drawable acquisition and GPU waits stay on the worker.
- Delayed presentation callbacks and invalid timestamps must not produce false latency measurements.

### Task 1: Zero blur work and resource lifetime

Files: `src/rendering/BlurPass.{h,cpp}`, `src/main.cpp`, `tests/gameplay_bga_target_tests.cpp`, `CMakeLists.txt`.
Interface: `outputTexture()` selects scene or blurred result; `execute()` skips both blur passes at zero. Blur targets are allocated only when needed and released when disabled.
- [x] Extend the existing Metal BGA test with tiny actual readbacks, draw counts, zero/positive/negative strength, resize and memory lifecycle checks; run it and confirm failure.
- [x] Implement bypass and lazy intermediate targets. Remove unconditional blur-view touches from main.
- [x] Build `gameplay_bga_target_tests`; run `ctest --test-dir cmake-build-debug -R '^gameplay_bga_(target|sampling_metal)_tests$' --output-on-failure` (expect pass).

### Task 2: Reuse unchanged Apple UI uploads

Files: `src/rendering/UiBatchRenderer.cpp`, existing UI upload/Metal tests.
Interface: Existing `UiBatchUploadCache::uploadIfChanged` with 4096-byte bounded snapshots; include Apple paths in the existing platform gate.
- [x] Verify the existing cache behavioral tests cover changed content, replacement, failure and oversized uploads.
- [x] Enable the same cache on iOS/macOS; reuse actual GPU growth/opacity/content-change tests.
- [x] Build/run `ui_batch_metal_tests`, `ui_batch_android_upload_metal_tests`, `ui_batch_upload_cache_tests` (expect pass).

### Task 3: Configurable Metal drawable limit

Files: video settings/store, display backend/manager, settings model/scene/localization, generated Metal patches, and their existing tests.
Interface: Persist `tripleBuffering=false` by default. Metal uses two drawables with Off and three with On. A cycling Off/On control uses the existing display preview/confirmation/timeout rollback. Unsupported renderers preserve saved intent without applying it.
- [x] Verify default/persistence, preview confirmation/timeout, unsupported intent preservation, renderer failure and rollback.
- [x] Carry the setting through an app-owned reset flag, guarded against collisions with pinned bgfx definitions, so resize/export restoration remain consistent.
- [x] Keep iOS layer changes on main and verify the generated patch plus unsigned iOS compilation. No device latency reduction claim without measurement.

### Task 4: Actual presentation and memory evidence

Files: new narrow Metal observer header and CMake patch, existing latency telemetry tests and `src/main.cpp`.
Interface: Optional observer receives drawable acquisition duration and presentation request/actual times. Observations populate bounded histograms only when app telemetry is enabled. Periodic diagnostics report bgfx resource use and process resident memory without conflating those figures with Metal HUD footprint.
- [x] Add tests for valid/invalid presentation timestamp deltas, then implement the optional backend observer and telemetry consumer.
- [x] Keep the software pacer and distinguish CPU submission from physical presentation. The follow-up budgets iOS pacing from frame start so drawable waits do not add another software interval.
- [x] Run focused rendering/platform tests, desktop main build and iOS compile verification. Review the entire unstaged diff and document measurement limits.

Verification status: desktop and unsigned iOS builds pass; telemetry-enabled main compiles on both platforms; affected native tests pass 12/12 and full iOS setup suite passes 54/54. The user-requested repair of the stale platform-exclusion test expectation is included. Native Metal smoke observed the actual drawable property changing 2 → 3 → 2. Final independent review found no issues. Prepared for pull request after verification and independent review.

Follow-up: corrected iOS frame-budget accounting after a report of 60 FPS with two drawables and Unlimited. The deterministic double-wait regression failed before the fix and passes after it; four focused pacing/display tests and desktop/unsigned iOS builds pass. Device throughput still requires retesting.
