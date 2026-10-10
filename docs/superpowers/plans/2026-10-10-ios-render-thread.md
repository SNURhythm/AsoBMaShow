# iOS Render Thread Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Keep native iOS input ingress responsive while scene rendering or Metal stalls.

**Architecture:** UIKit owns the SDL pump, native window and layer. One application worker owns scenes and single-threaded bgfx; owned messages cross the boundary. iOS export must use that same renderer owner, with cooperative progress/cancellation service.

**Tech Stack:** C++23, Objective-C++, SDL3, bgfx Metal, CMake, XCTest-compatible Simulator diagnostics.

**Spec:** docs/superpowers/specs/2026-10-10-ios-render-thread-design.md

## Global Constraints

- Android and desktop retain their current execution model.
- No judgement-policy change, touch prediction, deployment, new worktree, or unrelated formatting is included.
- Preserve explicit frame caps and the automatic iOS refresh-rate target.
- Leave pinned submodule source files unchanged; fail generated-source patch configuration on changed anchors.
- Main never synchronously awaits the application worker; keep pumping until worker cleanup completes.
- Physical-device timing remains unverified while the user's iPad is unavailable.

## Review Focus

- Native editing callback arrives after its TextInputBox is destroyed: discard by generation.
- Pointer-backed SDL payload expires while rendering stalls: consume an owned copy.
- Background/quit arrives during queue pressure: cancellation and shutdown remain deliverable.
- A layer resize is requested during shutdown: service main work before joining the completed worker.
- Export cancels while yielding a UI frame: preserve one bgfx owner and restore renderer state.

### Task 1: Owned application handoff

**Files:** Create `src/platform/ApplicationEventQueue.h`, `src/platform/ApplicationEventQueue.cpp`, `src/platform/GenerationMailbox.h`, `tests/application_event_queue_tests.cpp`; modify `CMakeLists.txt`.

**Interfaces:** Produces `platform::OwnedApplicationEvent(const SDL_Event&)`, `event() const -> const SDL_Event&`; `platform::ApplicationEventQueue(size_t capacity)`, `push(const SDL_Event&) -> bool`, `poll(OwnedApplicationEvent&) -> bool`, `takeOverflow() -> bool`; `platform::GenerationMailbox<T>::open() -> uint64_t`, `close(uint64_t)`, `post(uint64_t,T)`, `take(uint64_t) -> vector<T>`.

- [x] Add tests for owned text/drop/candidate payloads, FIFO key release, overflow cancellation and lifecycle delivery, stale callback generations, and producer progress while the consumer is stalled.
- [x] Compile/run the focused test; expect missing interfaces, then failing behavior as implemented.
- [x] Implement bounded handoff; never call consumer code under the queue mutex. Preserve lifecycle out of ordinary queue pressure, report overflow to clear held/stale input while gameplay continues.
- [x] Run `cmake --build cmake-build-debug --target application_event_queue_tests -j 6` and `ctest --test-dir cmake-build-debug -R '^application_event_queue_tests$' --output-on-failure`; expect PASS.
- [x] Commit the handoff and its tests.

### Task 2: Main-thread platform and Metal boundary

**Files:** Create `src/platform/IOSApplicationRuntime.h`, `src/platform/IOSApplicationRuntime.mm`, `src/platform/SDLMainThread.h`, `cmake/IOSBgfxMainThreadLayer.cmake`; modify `src/iOSNatives.mm`, `src/input/IOSTouchInput.h`, `src/input/IOSTouchInput.mm`, `src/view/TextInputBox.cpp`, `src/video/SDLDisplayBackend.cpp`, affected SDL callers, `cmake/IOSBgfxMetalCompatibility.cmake`, and `tests/ios_build_setup_tests.py`.

**Interfaces:** Consumes Task 1 queue/mailbox. Produces `RunIOSApplication(std::function<int()>) -> int`, `PollIOSApplicationEvent(SDL_Event*) -> bool`, `WaitIOSApplicationEvent(SDL_Event*,int) -> bool`, `IOSApplicationActive() -> bool`, `RunIOSMainThread(std::function<void()>)`, `PollIOSNativeTextEditorCallbacks()`; viewport snapshots serve frame-time reads. Add `SetIOSGameplayTouchInputEnabled(bool)` on main.

- [x] Add generated-patch tests exercising real CMake transforms with valid and changed anchors; add callback stale-owner/teardown tests to the portable mailbox test.
- [x] Run affected tests and observe failures before adding bridges.
- [x] Marshal main-only SDL/UIKit APIs explicitly; copy returned state, keep callback delivery on the application owner, invalidate native text owners at hide/destruction. Audit SDLInputBackend lock ordering so pumping cannot occur while holding a lock required by event watches.
- [x] Wrap only Metal layer configuration on main in generated bgfx source; drawable acquisition/encoding/waits remain on the renderer.
- [x] Implement worker startup, main event pump, lifecycle cancellation, pressure recovery and completion-before-join; keep main servicing pending native requests during failure/cleanup.
- [x] Run focused native/build-setup tests and `scripts/ios_release_verify.sh`; expect tests/build PASS, no upload.
- [x] Commit the platform boundary.

### Task 3: Switch runtime and preserve export ownership

**Files:** Modify `src/main.cpp`, `src/platform/IOSApplicationRuntime.h`, `src/platform/IOSApplicationRuntime.mm`, `src/replay/ReplayExportJob.h`, `src/replay/ReplayExportJob.cpp`, `src/ReplayVideoExporter.cpp`, `src/ApplicationContext.h`, affected replay tests and `tests/ios_build_setup_tests.py`.

**Interfaces:** Consumes Task 2 runtime. Produces an application-owner export work queue and an export UI service callback; callbacks only run at safe owner boundaries, never concurrently with scene destruction.

- [x] Add tests for worker startup failure/shutdown with pending main work and single-owner export completion/cancellation.
- [x] Observe targeted failures, then move bgfx initialization, scene loop and shutdown into RunIOSApplication. Poll copied events and callbacks; preserve pacing with worker waits and cached refresh state.
- [x] Suspend subsequent presentation after background delivery; reopen ingress only after viewport resynchronization. Overflow explicitly clears held/stale input without synthesizing focus loss or pausing gameplay.
- [x] Route iOS export through the application owner; progress/cancel UI yields use the existing renderer reservation. Do not transfer bgfx thread identity to an arbitrary export worker.
- [x] Run focused tests, desktop build and full parallel CTest; expect PASS. Run unsigned iOS verification; expect PASS.
- [x] Commit the integrated runtime.

### Task 4: Simulator acceptance and final review

**Files:** Add runtime verification evidence to `docs/superpowers/specs/2026-10-10-ios-render-thread-design.md`; fix only defects demonstrated by the checks.

**Interfaces:** Consumes integrated app and its diagnostic hooks; produces reproducible evidence, not a device-latency claim.

- [x] Build/install Simulator app; exercise gameplay, pause/retry, native text, resize/rotation, background/resume and export cancellation.
- [x] Inject a bounded render stall; verify main touch ingress continues, queue remains bounded and recovery cancels stale input. Run Main Thread Checker where supported.
- [x] Fix observed failures with regression tests, then repeat affected verification only.
- [x] Dispatch one fresh whole-branch reviewer; resolve important findings and verify fixes.
- [x] Commit and push verified work to the current branch upstream. Report untested physical-device behavior explicitly.
