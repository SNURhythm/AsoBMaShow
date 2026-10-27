# Metal Drawable Acquisition Pacing Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for this cohesive experiment; preserve the user's previously selected lower-effort implementation delegation.

**Goal:** Test lower presentation delay at 120 FPS by allowing two outstanding primary iOS drawables and waiting only when acquiring another.

**Architecture:** Backport the acquisition-time pacing idea from FunkinCrew/bgfx `cac072f4`, without changing the pinned backend or shaders. A process-lifetime, token-based gate owns two presentation slots; native callbacks capture only their token. A scoped application callback enables pacing only during normal iOS frame submission.

**Tech Stack:** C++17 synchronization, Objective-C++ Metal callback, generated CMake backend patch, existing iOS single-threaded bgfx submission.

**Spec:** The design and constraints below implement the recommendation the user approved with "let's try". Routine implementation choices do not require another approval round.

## Design and constraints

- Baseline is `63eb0290`, whose runtime matches `469a7f9c`. The earlier end-of-loop wait was rejected after approximately 14.5 ms Present Delay at only 60 FPS.
- Keep three CAMetalLayer drawables and Apple GPU limit one. The two presentation slots are independent of the GPU-completion limit.
- Pace only the primary iOS swapchain, immediately before `nextDrawable`. Preserve CPU preparation, existing timer accounting, ordinary presentation/release order, export readback and native texture ownership.
- Use the existing generated backend patch; leave the upstream submodule, shaders and non-iOS behavior unchanged. No end-of-loop presentation wait or display link.
- Reserve a slot before acquisition. If acquisition returns null, immediately release that token. Register a presented handler on a successful drawable that owns only the token, never a swapchain/application/drawable pointer.
- Gate storage survives late callbacks and shutdown. Tokens are never reused across cancellation, timeout or renderer recreation. Completion frees only the matching live token.
- Permit two pending tokens; a third acquisition waits until either is completed. Bound total added waiting to 50 ms, checking the application predicate outside the mutex at intervals no longer than 2 ms. On timeout or cancellation, invalidate old reservations and bypass pacing for that acquisition; native `nextDrawable` retains its existing behavior.
- Enable the gate through a thread-local RAII scope around the existing `submitIOSFrame` wrapper. The predicate checks UIKit activity, background, quit, export-active and export-requested state. Discard, unscoped calls and export/background paths bypass pacing and clear pending state. Scope exit clears/restores the callback context, but must not clear ordinary submitted reservations between frames.
- Clear pending tokens on primary swapchain teardown/reinitialization and inactive/export transitions. No callback may dereference the application context after the submission scope ends.
- This wait necessarily runs inside bgfx's existing renderer ownership at drawable acquisition. Cancellation uses the existing short lifecycle snapshot and atomic export/app flags without taking renderer ownership again; no lock inversion. No new renderer lock is introduced.
- No deployment or installation. Work in the current checkout; commit and push task changes after verification. Do not create a worktree or format whole files.

## Approach and reference

The rejected strict application-loop wait serializes CPU preparation with visible presentation. The selected backend acquisition wait retains that preparation overlap and permits two outstanding frames. An unchanged cherry-pick would still allow only one slot at our GPU limit, and can leak a slot when `nextDrawable` returns null; its raw semaphore callback also needs a stronger lifetime guarantee.

Reference: https://github.com/FunkinCrew/bgfx/commit/cac072f4a53fbdf4fbce9fe3c68976820bddd86e

Original upstream PR was closed without merging or confirmed latency improvement: https://github.com/bkaradzic/bgfx/pull/3734

## Review focus

- Two outstanding drawables progress; a third waits for either earlier completion, including out-of-order callbacks.
- Missing drawables, missing callbacks and inactive/export transitions cannot leak a slot or create an unbounded added wait.
- Late callbacks after cancellation, timeout or teardown cannot free a newer reservation or touch destroyed native/application state.
- Scoped predicate lifetime and lock order remain valid inside synchronous iOS bgfx submission; offscreen/export calls never need a presentation callback.
- The generated hook activates only for the primary iOS drawable and preserves existing native resource ownership and timer accounting.

## Task: acquisition gate, integration and focused verification

**Files:** `cmake/BgfxMetalPresentation.cmake`; new `cmake/BgfxMetalPresentationGate.h/.cpp`; `src/main.cpp`; `tests/ios_build_setup_tests.py`; new focused gate/native callback fixtures; `CMakeLists.txt`; `cmake/TestGroupCatalog.cmake`.

**Interfaces:** A small scoped predicate API (`bool (*)(void*)`), a token-returning `acquire` bounded to 50 ms, matching-token `complete`, and `cancel`. Token zero denotes bypass. Implementation may choose exact names while keeping all consumers consistent.

- [x] Add behavioral tests: two immediate reservations; third waits for an older completion; out-of-order and duplicate callbacks; failed acquisition returns its reservation; timeout and cancellation; stale callbacks after reset; scope-disabled/export bypass; native callback lifetime and platform/window gating. Synchronize worker tests explicitly rather than adding fixed sleeps.
- [x] Confirm meaningful tests fail before implementing their behavior.
- [x] Implement the gate and generated acquisition hook, plus scoped activation and lifecycle cancellation in the application. Preserve fixed pool assertions and upstream source immutability.
- [x] Pass focused gate/native fixtures and relevant iOS build-setup tests. Register new checks in `area_rendering` (and platform label where useful).
- [x] Build `main`, `tests_rendering`, `tests_video`, `tests_platform_ios` in one command, then run the rendering/video/iOS CTest label union at `-j 6`. Build unsigned iOS with `scripts/ios_firebase_deploy.sh --build-only`.
- [x] Obtain one independent review and document results and remaining device uncertainty.

Publish the verified candidate on the current branch and update PR #161.

## Device acceptance

Compare with `63eb0290` using the same chart, settings, warmup and HUD mode. Accept only a lower Present Delay while maintaining 120 FPS. Local correctness/build checks cannot establish that benefit. Revert this experiment if cadence falls or delay does not improve.

## Local outcome

- All 90 selected rendering/video/iOS CTest checks passed at `-j 6` in 13.19 seconds, including the three new gate/native fixtures, actual Metal readback, and native NV12 lifetime at GPU limits 1, 2 and 3 in single-threaded and multithreaded runs.
- The three affected iOS build-setup checks passed: generated backend/reflection compatibility, main-thread layer boundaries, and retained native texture ownership.
- Desktop `main` and the unsigned iOS device build passed. The modified backend and application submission wrapper compiled and linked for iOS.
- Independent review found no actionable issues. Production code was unchanged after review and broad verification.
- The generated-hook fixture checks failed acquisition recovery, callback execution after scope/swapchain destruction, discarded and unscoped submissions, secondary windows, occlusion, and the disabled non-iOS path. It also rejects changed upstream patch anchors and verifies source immutability.

No deployment or installation was performed by the agent. The 50 ms fallback bounds added gate waiting; it does not bound native drawable acquisition or guarantee display latency.

## Device result and decision

The user reported approximately 15.9 ms Present Delay at 120 Hz on candidate
`3f018f0a`. They described the iPad as slightly heated while using a fast charger
and considered the change effective. Retain this candidate based on that feedback;
no further pacing adjustment or rollback is made.

The reported 120 Hz operation is encouraging after the earlier experiment's
60 FPS regression. The delay is close to the previous approximately 16 ms result,
and temperature/charging conditions were not matched. This observation does not
yet isolate a latency improvement from run-to-run variation or thermal effects.
A comparison with `63eb0290` under the same cool-device, charging, chart, settings
and HUD conditions remains the way to quantify any gain.
