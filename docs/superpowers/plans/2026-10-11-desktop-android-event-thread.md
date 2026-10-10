# Desktop and Android Event Thread Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Service desktop/Android SDL events independently of rendering stalls.
**Architecture:** Reuse the bounded owned queue and cleanup-safe worker host; keep SDL ownership on its bootstrap thread and marshal platform requests there. Preserve iOS and existing native input and bgfx workers.
**Tech Stack:** C++23, SDL3, bgfx, Objective-C++, Android JNI.
**Spec:** docs/superpowers/specs/2026-10-11-desktop-android-event-thread-design.md

## Global Constraints

- Work on the current branch; no worktree or deployment.
- One application owner, independent event pump; no scene access on event callbacks.
- Preserve iOS behavior, Android surface retirement, native input routes and renderer selection.
- One build at a time per build directory; no whole-file formatting.
- Commit and push verified task changes; update PR 136.

## Review Focus

- Shutdown with a pending main-thread operation must finish without deadlock (Task 1).
- Focus loss/queue pressure during a render stall must not leave held inputs (Tasks 1, 2).
- Desktop text editing must not race scene destruction or judge typing (Task 2).
- Android surface loss/rotation and external picker return must retain valid native windows (Task 3).
- Resize and controller hotplug during export must survive until normal rendering resumes (Tasks 2, 3).

### Task 1: Portable SDL owner and application worker

**Files:** Create `src/platform/SDLApplicationRuntime.h`, `.cpp`, `tests/sdl_application_runtime_tests.cpp`; modify `src/platform/IOSApplicationRuntime.h`, `SDLMainThread.h`, `src/CMakeLists.txt`, `CMakeLists.txt`.
**Interfaces:** Produce `platform::runSDLApplication(SDL_Window*, std::function<int()>) -> int`, `pollApplicationEvent(SDL_Event*) -> bool`, `waitApplicationEvent(SDL_Event*, int) -> bool`, `applicationActive() -> bool`, `takeApplicationOverflow() -> bool`, `getWindowSnapshot(SDL_Window*) -> std::optional<WindowSnapshot>`. Existing `platform::onMain` and `sdlMain` become functional on non-iOS.

- [x] Add a real SDL test that fails because worker `onMain` executes on the wrong thread; test return values, exceptions and caller-side SDL errors.
- [x] Run the test and record RED; implement SDL main dispatch with exception capture.
- [x] Add runtime tests for worker separation, main progress during a stall, text payload ownership, lifecycle/overflow state, and main requests during unwinding; record RED before implementing runtime.
- [x] Implement host and copied viewport state, bounded waits, queue recovery, and explicit main-thread lifetime.
- [x] Build and run `sdl_application_runtime_tests` and `application_event_queue_tests`; expected all pass. Commit.

### Task 2: Integrate platform loops and concurrent input safely

**Files:** Modify `src/main.cpp`, `src/scene/play/GamePlayScene.cpp`, `src/input/LinuxRealtimeKeyboardBackend.cpp`, `src/MacNatives.mm`, and platform calls found by the audit. Add regression coverage in the matching existing input/display fixtures and setup tests.
**Interfaces:** Consume Task 1's host/event/snapshot functions and `platform::onMain`; preserve the existing iOS interface.

- [x] Add a regression for desktop event-watch text-focus snapshots without presentation access; run RED.
- [x] Start desktop/Android application work through the host, consume owned events, reconcile overflow/current viewport, retain native suspend handshakes and export coordination.
- [x] Publish text-focus snapshots on all platforms and marshal remaining main-thread-only operations. Add any discovered behavior regression before its fix.
- [x] Add a bounded opt-in Debug stall probe and quit option for real-app acceptance; keep release behavior unchanged.
- [x] Build desktop main and affected test targets. Run event, input, display, Android lifecycle and iOS setup checks; expected pass. Commit.

### Task 3: Platform acceptance and final review

**Files:** Update spec/plan with measured evidence; fix acceptance regressions in their owning files with failing tests first.
**Interfaces:** Consume the complete host and platform integration from Tasks 1–2.

- [x] Run desktop stall probe and native UI acceptance for text input, resize/focus and shutdown. Inspect runtime thread stacks if needed.
- [x] Run Android build-only helper and emulator acceptance for gameplay input, lifecycle, surface recreation, native picker and export where available. Preserve settings and remove only owned fixtures.
- [x] Recheck unsigned iOS build and run full desktop CTest suite with -j 6; expected pass.
- [x] Dispatch one fresh strongest-model whole-branch reviewer; resolve important findings with RED/GREEN tests and green suite.
- [x] Commit/push verified changes and update/attach PR 136 with final scope and evidence.

## Completion evidence

Initial desktop build and all 474 CTest cases passed (118.61 seconds). Android restricted
release build, unsigned iOS build and iOS artifact audit passed. The fresh
whole-branch review's four Important findings were fixed with RED/GREEN
regressions. Platform acceptance and explicit manual coverage limitations are
recorded in the spec's Recorded acceptance section; checked acceptance items
mean those bounded checks were performed, not that every platform was tested.

Execution decisions: continued on the authorized existing branch; overlapped
the fresh read-only review with final builds; preserved the existing signed
Android emulator installation by using and removing an owned fresh AVD.

### Follow-up: physical-device and remaining local acceptance

- [x] Verify user-operated desktop pointer/text interaction and real desktop exports, resize and quit cancellation.
- [x] Update the connected Android phone in place using the explicitly requested version code 6.
- [x] Measure independent pumping and raw touch ingress during a temporary five-second owner stall; restore the normal release APK afterward.
- [x] Exercise Android export, Home/surface loss cancellation, foreground recovery, gameplay and frame pacing.
- [x] Fix discovered export owner/cancellation and lifecycle state defects with failing-then-passing regressions; obtain a focused follow-up review.
- [x] Build desktop/Android/iOS, audit the iOS app, run 475 CTest cases and rerun the three affected tests after the final surface-state fix.

The [device acceptance report](../../reviews/2026-10-11-desktop-android-device-acceptance.md)
records measurements and explicit limits. Windows/Linux hosts were unavailable.
Physical end-to-end latency and arbitrary GPU stalls are not certified. The
phone also exposed a separate hard-link-based replay installation failure on
emulated external storage; saved-replay round-trip acceptance remains blocked.
