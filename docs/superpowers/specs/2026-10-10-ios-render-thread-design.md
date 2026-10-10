# iOS rendering and input thread separation

Status: architecture approved; implementation in progress.

## Goal

Keep UIKit able to receive and enqueue gameplay touches while the application
prepares a frame, waits for a Metal drawable, or waits for the GPU. Preserve
existing judgement timestamps, audio behavior, native UI, and replay export.
The user requested this work on a new branch after discussing the limitations
of timed main-run-loop pacing. Work starts on `feat/ios-render-thread`, based
on `develop` at `134b1329`, which includes latency PR #135.

Success means a deliberately stalled renderer does not stall native touch
ingress. It does not mean eliminating a timestamp grid imposed by iOS or
guaranteeing a particular physical input-to-display latency.

## Findings in the current implementation

- `src/main.cpp` calls `bgfx::renderFrame()` before initialization on iOS,
  selecting single-threaded rendering on the UIKit main thread. The comment
  and `tests/ios_build_setup_tests.py` explicitly protect this arrangement
  because the backend modifies a UIKit-owned Metal layer.
- The same application loop processes SDL events, updates scenes, builds draw
  commands, and calls `bgfx::frame()`.
- Enabling bgfx's internal worker alone does not remove the main-thread wait:
  `Context::frame()` in `bgfx/bgfx/src/bgfx.cpp` calls `renderSemWait()`.
- `RealtimeGameplayWorker` already consumes input and publishes snapshots on
  its own thread. Direct UIKit gameplay touches already enter this queue
  without waiting for the scene's SDL event processing.
- SDL's UIKit Metal view updates drawable size during layout. The bgfx Metal
  backend also configures layer properties. These operations need an explicit
  ownership boundary before moving rendering.
- Native text editing currently invokes a callback into `TextInputBox` from
  UIKit. Moving scenes without moving this callback delivery creates a race.

## Approaches

1. **Recommended: move the application scene/render loop to one worker.**
   UIKit remains on the main thread. Scene state retains a single owner, so
   this avoids making every view and scene concurrently readable. Gameplay
   and audio retain their existing workers.
2. **Enable only bgfx's internal worker.** Smaller change, but main-thread scene
   preparation and `bgfx::frame()` backpressure remain. This does not meet the
   stated success criterion.
3. **Keep scenes on main and introduce immutable render descriptions for the
   entire UI.** This can separate drawing cleanly, but requires a much broader
   redesign of built-in views, skins, textures, and rendering resources.

## Thread ownership

| Owner | Responsibilities |
| --- | --- |
| UIKit main thread | SDL event pumping; UIKit touch callbacks; window/view creation and destruction; native controls; orientation and safe-area queries; Metal layer configuration |
| Application/render worker | Scene updates and UI state; rendering preparation; bgfx initialization, frame submission and shutdown; render pacing |
| Gameplay worker | Existing input consumption, simulation, judgement, and published gameplay snapshots |
| Audio callback | Existing audio generation and command consumption |

Use bgfx's single-threaded backend on the application/render worker initially.
This avoids adding an extra CPU frame queue. The main thread owns the native
window and layer until renderer shutdown has completed.

## Handoffs

**Gameplay input:** retain the direct native ingress and its timestamps. UIKit
callbacks must not acquire the renderer mutex or wait for scene processing.
The existing immutable hit-test snapshots remain the geometry contract.

**General UI events:** pump SDL only on the main thread, then hand owned events
to the application worker. Copy pointer-backed text/drop payloads before their
SDL lifetime ends. Preserve key/button edges, focus, cancellation and lifecycle
ordering. Coalescing is limited to compatible motion/resize events between
ordering barriers. Queue pressure must never block UIKit or silently lose an
input release; exhaustion must invalidate the input session and recover to a
paused state. Lifecycle cancellation must remain deliverable under pressure.

**Platform operations:** route SDL APIs that require the main thread, UIKit
queries/mutations, and native control work through a dedicated iOS bridge.
Use copied display/viewport snapshots for per-frame reads. A worker may await
a main-thread operation only when it holds no lock needed by main-thread
callbacks; the main thread must never synchronously await that worker.

**Native callbacks:** copy native text-editor and other scene-facing completion
data into the application queue, guarded by an owner generation. Closing or
replacing a scene invalidates pending deliveries. Callbacks cannot retain raw
scene pointers across asynchronous destruction.

**Metal layer:** perform initialization and configuration on the main thread;
perform drawable acquisition, command encoding and GPU waits on the render
worker. Serialize resize/configuration handoffs without making UIKit await GPU
work. Extend the existing generated-source compatibility mechanism in
`cmake/IOSBgfxMetalCompatibility.cmake` where the backend needs adaptation;
leave pinned submodule source files unchanged. Fail configuration if expected
upstream patch anchors change.

## Lifecycle, pacing and export

- On backgrounding, immediately close native gameplay ingress and request
  render suspension. Serialize suspension with any frame already in progress;
  prevent later submissions while inactive. Resume only after publishing the
  current drawable/viewport generation. Exercise the transition while a
  drawable wait is in flight, not only while rendering is idle.
- Preserve explicit frame caps and the automatic iOS refresh-rate target.
  Pacing waits move to the worker; main-thread event delivery runs independently.
  This branch does not enable `wantsImmediatePresentation` or change VSync policy.
- Preserve replay export's exclusive renderer reservation and automatic-pacing
  bypass. Audit and enforce bgfx thread affinity during export ownership
  transfer; a mutex alone is insufficient. Native export progress/cancellation
  remains responsive on the main thread.
- Shutdown closes ingress, cancels pending requests, and requests worker exit.
  Keep pumping main-thread work until renderer cleanup has finished, then join
  the completed worker and destroy the native window/layer. Handle startup
  failure using the same order.
- Android and desktop retain their current execution model.

## Verification and acceptance

1. Concurrency tests stall the render consumer while the producer accepts input;
   cover queue pressure, cancellation, event ownership and stale generations.
2. Test startup failure, background/foreground, resize and shutdown with pending
   platform requests. Prove there is no main-to-worker/worker-to-main wait cycle.
3. Replace the test requiring main-thread bgfx with checks for the new ownership
   boundary. Run desktop compilation, affected tests, full parallel CTest and
   `scripts/ios_release_verify.sh` with no distribution action.
4. Simulator smoke tests cover gameplay, pause/retry, native text entry,
   rotation/resize, background/foreground, and replay export/cancellation.
   Use Main Thread Checker and targeted race diagnostics where supported.
5. An instrumented render-stall experiment compares native callback-to-enqueue
   timing with and without a renderer stall. Also inspect render queue depth
   and frame pacing so lower input delay does not hide growing visual latency.
6. Report simulator evidence separately from physical-device evidence. The
   user's M5 11-inch iPad Pro is currently unavailable for connected testing;
   physical latency and real-device background behavior remain unverified until
   tested there. Do not claim a measured device improvement from simulator data.

No judgement-policy change, touch prediction, deployment, new worktree, or
unrelated formatting is included.

## Implementation and verification evidence (2026-10-11)

UIKit now pumps SDL and delivers native input while one application worker owns
scene updates, bgfx initialization, rendering and shutdown. SDL payloads cross
an owned, bounded queue; native editing callbacks cross a generation mailbox.
Main-thread platform requests remain serviced until the worker has unwound.
Replay export uses the same renderer owner and a native progress/cancel overlay.
The overlay avoids reentrant scene callbacks during scene destruction.

- Desktop build and all 472 CTest cases passed after the review corrections (257.36 seconds).
- Release verification passed 68 native cases, 53 iOS setup cases, 20 workflow
  cases and 18 artifact cases, plus the unsigned iOS build and artifact audit.
- iPhone Duo Simulator / iOS 27.1 exercised gameplay, pause/resume, retry,
  background/resume, native search editing and portrait/landscape resizing.
  Main Thread Checker was loaded and reported no violations in these runs.
- Export cancellation during encoding returned to Records. A short replay
  encoded 750 frames and saved to Photos in 10.67 seconds. Simulator Metal
  rejects shared-buffer linear textures, so its generated backend now uses the
  ordinary texture/getBytes fallback; physical devices retain the fast path.
  The generated-source regression and desktop Metal pixel readback test passed.
- An opt-in Debug one-second render stall left UIKit servicing 670 iterations,
  followed by normal quit. A sampled stack placed main in its native run loop
  and the application worker in Metal drawable acquisition. This establishes
  scheduling separation, not a physical-device latency improvement.
- A second Debug probe stalled the application worker for five seconds after
  three native touches. Five UIKit touch callbacks during that stall reached
  the input queue in 23–37 microseconds; the four callbacks before the stall
  took 17–155 microseconds, and four afterward took 26–46 microseconds. Main
  serviced 3,911 iterations during the stall, and gameplay paused normally
  afterward. These tiny simulator samples are a concurrency check, not a
  statistical latency benchmark. Reproduce with `ASOBMASHOW_IOS_TOUCH_PROBE=1`,
  `ASOBMASHOW_IOS_RENDER_STALL_AFTER_TOUCHES=3`, and
  `ASOBMASHOW_IOS_RENDER_STALL_MS=5000` in a Debug build.

The simulator checks used generated local chart/audio fixtures. Unicode IME
composition and physical-device timing/background behavior were not verified.
The application event queue remains capped at 256; its pressure, lifecycle,
shutdown and stale-generation behavior are covered by portable tests. The
existing single-owner bgfx mode is retained, so this change does not introduce
an extra bgfx producer/render frame queue. Physical frame pacing still requires
measurement on the user's iPad.

### Review corrections

The fresh branch review identified two recovery defects. Export now defers
window/display/device events while suppressing user input, so completion and
cancellation deliver pending resize and hotplug changes. After queue overflow,
the registry reconciles the actual attached SDL device set without reopening
unchanged devices. Regression tests cover preserved resize/connect/disconnect
events, lost hotplug reconciliation, repeated reconciliation and enumeration
failure. The original export-resize defect was also reproduced in Simulator:
rotation during encoding followed by cancellation returned a stretched
landscape scene on a portrait drawable.
The fixed Simulator build passed both branches: rotation during encoding
followed by cancellation returned the correct portrait layout, and a short
750-frame export rotated during encoding completed, saved to Photos in 10.52
seconds, and returned the correct portrait layout. Main Thread Checker reported
no violations. The original landscape preference was restored and the generated
chart/audio fixtures and temporary export artifacts were removed.

The final unsigned iOS build and artifact audit also passed after these corrections.
