# Metal HUD findings implementation plan

> Completed using superpowers:executing-plans in the existing checkout.
> The frame-start timer accounting was reverted for an A/B comparison, then
> restored after no observable change. Branch-added telemetry was subsequently
> removed; see [the follow-up](../../reviews/2026-10-27-metal-hud-followup.md).

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

### Task 3: Fixed Metal drawable pool

Files: `src/main.cpp`, generated Metal patches, and existing iOS setup tests.
Interface: Always configure three CAMetalLayer drawables on iOS/macOS. Remove
the experimental display setting, persisted field, capabilities, custom reset
flag, UI/localization and setting-specific tests. Ignore old saved values.
- [x] Remove configurable buffering from settings, display transactions and UI.
- [x] Set the native drawable count to three across resize/reset paths.
- [x] Keep iOS layer mutations on main and update generated-source regression checks.
- [x] Retain the separate iOS frame-budget correction and telemetry.

### Task 4: Actual presentation and memory evidence

Files: new narrow Metal observer header and CMake patch, existing latency telemetry tests and `src/main.cpp`.
Interface: Optional observer receives drawable acquisition duration and presentation request/actual times. Observations populate bounded histograms only when app telemetry is enabled. Periodic diagnostics report bgfx resource use and process resident memory without conflating those figures with Metal HUD footprint.
- [x] Add tests for valid/invalid presentation timestamp deltas, then implement the optional backend observer and telemetry consumer.
- [x] Keep the software pacer and distinguish CPU submission from physical presentation. The follow-up budgets iOS pacing from frame start so drawable waits do not add another software interval.
- [x] Run focused rendering/platform tests, desktop main build and iOS compile verification. Review the entire unstaged diff and document measurement limits.

Verification status: the final fixed-three policy passed desktop and unsigned iOS builds, seven focused native tests and all 54 iOS setup tests. A native Metal smoke check retained three drawables across resize, VSync and MSAA resets. Legacy-settings persistence was verified once, then its obsolete fixture was removed as requested. Physical iPad throughput remains a device measurement; further scheduling candidates are documented in the review report.
