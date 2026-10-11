# Desktop and Android event/render separation

## Intent and scope

Extend PR 136's independent event delivery to desktop (macOS, Windows, Linux)
and Android. The user approved this extension with “let’s go for both desktop
and android.” Work stays on `feat/ios-render-thread`, without a new worktree or
deployment. Preserve the existing iOS runtime and native gameplay/audio paths.

Success means a stalled application/render worker cannot stop the SDL event
pump, main-thread platform operations, or native gameplay ingress. Scene updates
and ordinary UI event consumption still run on one application owner: this does
not promise UI animation while that owner is stalled or faster display scanout.

## Architecture

Keep SDL initialization, window creation, event pumping, main-thread operations,
and final window destruction on the thread which initialized SDL. Run the
application loop on one worker. Desktop/Android retain bgfx's internal render
thread and existing maximum frame latency; adding another GPU frame queue or
changing renderer backend is out of scope. Android's SDL main thread is distinct
from Java's UI thread; retain that separation and direct Java/native touch path.

Reuse `ApplicationThreadHost` and the owned bounded `ApplicationEventQueue`.
Introduce `platform::runSDLApplication`, `pollApplicationEvent`,
`waitApplicationEvent`, `applicationActive`, `takeApplicationOverflow`, and a
copied `WindowSnapshot` for non-iOS. The pump publishes window dimensions and
lifecycle state. SDL callbacks marshal synchronous worker-to-main operations via
SDL_RunOnMainThread, catch C++ exceptions inside the C callback, and propagate
them on the caller. Main services requests throughout worker cleanup before join.
No main callback may await the application worker or acquire its rendering lock.

Reusing only bgfx's existing internal worker would leave event pumping coupled
to scene updates and bgfx frame waits. A second independent SDL pump thread
would violate SDL window/event ownership. The selected host/worker split avoids
both problems and reuses the iOS ownership model with SDL's native dispatcher.

## Input and platform state

Pointer-backed SDL payloads must remain owned across the boundary. Preserve
key/button edges and lifecycle ordering. Queue pressure cancels the input stream,
retains quit/lifecycle state, reconciles attached controllers, and restores the
current viewport without replaying stale input or implicitly resuming gameplay.

Event watches now run concurrently with desktop scene updates. They must read
atomic/copied text-focus state, never presentation objects. Native keyboard,
controller, MIDI, gyro and Android raw-touch routes remain authoritative and must
not double-deliver SDL fallback events. Focus loss must release or interrupt
held gameplay input safely even during a rendering stall.

Marshal SDL window/display, cursor, clipboard, text-input and focused-window
operations to their owner. Copy data with limited lifetimes before returning.
Audit macOS native dialogs/platform APIs and Linux focused-window capture.
Window snapshots avoid a synchronous call per ordinary geometry read.

Android keeps its existing surface ownership and render-suspend handshake:
Java must not destroy a surface still referenced by the renderer. That safety
handshake can await GPU retirement; event separation cannot remove it. SDL
lifecycle pumping remains independent, and render suspension/resumption consumes
the resulting state on the application owner. Never move Java blocking pickers
onto the Java UI thread. Export keeps its exclusive renderer coordinator and
runs queued work on the application/bgfx owner on desktop/Android; SDL main must
not need that lock. The owner presents progress without dispatching scene events.
Quit, application suspension and surface loss cancel export; desktop focus loss
only hides progress. Surface availability must not invent lifecycle background
state: `applicationCanPresent` combines both for presentation, while
`applicationActive` remains lifecycle-only.

## Verification

Use actual SDL runtime tests for event delivery and synchronous main operations
while the owner is stalled, owned text/drop payloads, lifecycle/overflow recovery,
exception propagation, and cleanup-before-join. Retain existing queue/input and
display tests. Add regression coverage for concurrent desktop text focus.

Build the desktop app in cmake-build-debug with one build at a time and run the
full CTest suite with -j 6. Compile Android with the Firebase helper's build-only
mode and test the android10 emulator: startup, input, background/foreground,
rotation/surface recreation, and shutdown. Run an opt-in bounded Debug stall
probe on desktop/Android, measuring pump progress separately from latency.
Recheck iOS compilation/shared input tests. Use a fresh whole-branch review.

Hardware-only timing, unavailable Windows/Linux execution, and physical Android
GPU behavior must be reported as unverified when only local macOS/emulator
verification is possible. No Firebase, Play, or TestFlight upload is authorized.

## Recorded acceptance

Initial full CTest run: 474/474 passed in 118.61 seconds with `-j 6`.

- Desktop and Android now run application/scene work off the SDL bootstrap
  thread, retaining bgfx's internal render worker. Desktop main and all native
  test targets build; Android restricted-file release APK and unsigned iOS app
  build successfully. The iOS artifact audit passes.
- Final macOS Metal Debug run with Main Thread Checker: a 1,000 ms forced
  application stall left SDL main servicing 780 iterations; automatic quit
  completed with exit code 0 and no Main Thread Checker violation. Earlier
  native checks covered keyboard navigation, fullscreen resize and Cmd-Q.
- Android 10 arm64 emulator with the final release APK selected Vulkan. A local
  20-note fixture imported, launched, paused, resumed and reached results.
  Home/foreground destroyed and recreated the surface, and the paused scene
  returned correctly. The native folder picker rotated to portrait, canceled
  back to landscape, and library text input displayed the typed search with
  the Android keyboard. No app fatal signal, exception or ANR was observed.
- The existing android10 installation had a different signing key. It was
  preserved; these checks used a temporary owned Android 10 AVD, removed after
  acceptance. Existing user files and the connected physical device were not
  changed.
- One fresh whole-branch review identified four Important issues: desktop touch
  callbacks on the pump, missing desktop ingress/lifecycle protection, Android
  overflow retaining a synthetic suspend, and resize publication preceding the
  copied viewport. Each has an observed failing regression followed by a passing
  fix. The final suite also corrected an obsolete desktop-drain expectation and
  an existing unsynchronized worker-state assertion in a persistence fixture.

Follow-up physical-device acceptance updated the Samsung Galaxy S20 FE in place
with the user's requested version code 6. Desktop pointer/text interaction was
confirmed by the user. Desktop and Android export, native quit cancellation,
Android Home/surface recreation, injected-touch delivery during a five-second
owner stall, and physical-device pacing were exercised. This uncovered and fixed
export bgfx ownership and cancellation/resume defects. The follow-up full suite
passed 475/475; the final surface-state correction passed its focused regressions.

See [the device acceptance report](../../reviews/2026-10-11-desktop-android-device-acceptance.md)
for measurements, evidence and remaining limits. Windows/Linux execution and
physical end-to-end latency remain unverified. A forced stall exceeded Android's
existing surface-retirement timeout; survival does not certify arbitrary GPU
stalls. A separate external-storage replay installation failure blocks saved-
replay round-trip verification. The final installed release has no temporary
probe instrumentation. No deployment was performed.
