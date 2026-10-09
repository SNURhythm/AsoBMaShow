# SDL3 coverage and obsolete application work

Reviewed 2026-10-09 against application base `0a4e8f85`, SDL
`3d22d98cf633b673aae4f278ea55775481ae9279`, and SDL_ttf
`661ba00eb142c060768b752c9420b50c961b909e`. This pass covers input,
events, window/display lifecycle, timing, threading, IO, audio/rendering
boundaries and mobile bridges. It does not change font handling or dependencies.

A source inventory found 125 distinct SDL call-like identifiers across 89 app
source files (859 textual occurrences, including comments/macros). That is a
navigation aid, not a runtime coverage measurement. Decisions below were checked
against the pinned SDL implementation and actual application consumers.

## Implemented reductions

| Area | Change | Evidence / effect |
| --- | --- | --- |
| Controller events | Disable `SDL_EVENT_JOYSTICK_UPDATE_COMPLETE` and `SDL_EVENT_GAMEPAD_UPDATE_COMPLETE` when the app SDL input backend starts. | The app consumes individual edges and has no completion-marker consumer. `SDL_joystick.c` emits these only when enabled; `SDL_gamepad.c` uses joystick completion only to emit gamepad completion. Removes up to two unused events per changed mapped-controller update before app watches/queue dispatch. Raw events remain enabled because SDL uses them to generate mapped events. |
| Mobile display | Skip `SDLDisplayBackend::observeRuntimeState()` for fixed mobile displays. | Its sole purpose is remembering normal desktop window geometry. The real adapter previously allocated/copied a display list every idle frame. Desktop observation, capture, VSync preview and rollback remain. |
| Android private cache | Delegate lookup to `SDL_GetAndroidCachePath`, removing the Java `getCacheDirPath` bridge. | SDL3 wraps the same `Context.getCacheDir()` and caches the lookup. Preserve canonical private-container identity and null/error rejection. A missing cache root is now rejected by filesystem canonicalization; normal Android `getCacheDir()` supplies an existing directory. |
| iOS drawable setup | Remove the second successful pixel-size query and duplicate mobile logical-size query. | Both pixel paths now query the same `SDL_GetWindowSizeInPixels`; the old comparison no longer offered a distinct source. Keep invalid-size fallback and the separate Display Zoom override. |
| Sensor startup | Remove the app's explicit `SDL_INIT_SENSOR` reference. | No app SDL sensor consumers exist. Supported gyro input uses native backends; SDL's internal joystick sensor fusion initializes/refcounts its own sensor dependency. |
| SDL2 accelerometer shim | Remove name/shape detection, stored flag and 2.5× gain branches for the former “iOS Accelerometer” joystick. | SDL3 removed `SDL_HINT_ACCELEROMETER_AS_JOYSTICK` and its synthetic joystick. Real SDL axes now follow normal signed normalization. CoreMotion/Android gyro paths remain. A coincidentally named external joystick no longer gets an accidental gain multiplier. |
| Dead integration | Delete unused `SDLInputSource` keyboard class/CMake entry, `rebaseWrappingTimestampMillis` and its obsolete tests, and `GetIOSWindowHandle`. | Keyboard consumers use the registry/realtime backend, SDL3 exposes 64-bit nanosecond event fields, and the Metal owner now obtains its layer through SDL. Current 64-bit clock rebasing and the active `SDLTouchInputSource` remain. |

SDL input configuration is process-wide, as were the backend's existing joystick
and gamepad enabling calls. This application owns that configuration; backend
stop does not restore event states. These are maintenance and work-elimination
changes, not a measured FPS, battery or physical-input-latency improvement.

## Coverage decisions: work SDL3 does not replace

| App responsibility | Decision and reason |
| --- | --- |
| Native keyboard/controller workers | Keep. Nanosecond event fields do not remove SDL main-thread pumping latency. macOS event taps, Windows hooks/XInput sampling and Linux evdev readers have separate delivery/focus contracts. |
| Input queues and timestamp domains | Keep. Registry queues also carry MIDI/gyro/native events and enforce ordering, reentrancy and subscription boundaries. SDL ticks and audio/steady clocks have different epochs; 64-bit types do not eliminate rebasing. Android watchers may run on the Java touch thread. |
| Stable bindings and legacy key/button generations | Keep. SDL runtime IDs are not persisted device identities. SDL keyboard state after a pump can miss an ordered press/release pair; event-based state is intentional. |
| Frame pacing and iOS run-loop servicing | Keep. `SDL_DelayNS`/`SDL_DelayPrecise` are wait primitives, not missed-deadline/cap/export policy. Precise delay may spin. iOS waits service UIKit modes; desktop sliced waits also service independent native input. |
| Display preview/rollback and orientation | Keep. SDL does not expose an equivalent transactional normal/maximized geometry snapshot or coordinate bgfx/export reservations. Dynamic iOS orientation requests do more than set an SDL hint. |
| Rendering/audio | Keep bgfx, miniaudio and FFmpeg. SDL_GPU/audio APIs are alternative architectures, not drop-in deletion of camera, shader, decoding, mixing, export or renderer synchronization work. |
| Bounded asset and archive IO | Keep. `SDL_LoadFile` would discard byte limits/cancellation. SDL Android IO still reaches packaged assets that ordinary streams do not. Explicit Apple bundle lookup is required because SDL3 removed SDL2's implicit resource fallback. |
| Android SAF and DocumentsProvider | Keep. Pinned SDL's Android folder dialog is unsupported, and its file picker does not implement persisted grants, tokens, cancellation, mutation leases or verified moves. |
| iOS Files/security-scoped storage | Keep. Pinned SDL has no corresponding iOS dialog backend, security-scoped bookmark coordination or backup-exclusion policy. Preserve existing directory identity. |
| Native iOS editor | Keep. App UI provides visible native selection, accessibility and callbacks; SDL's hidden input field is not that surface. Clipboard operations already use SDL. |
| Locale, encoding and networking | Keep script-aware Android BCP-47 tags (SDL drops script), MS932 conversion, and HTTP redirect/progress/cancellation contracts. SDL URL launching and generic IO do not replace them. |
| Background media/platform dispatch | Keep OS MediaSession/Now Playing, foreground-service/session policy and picker-related bgfx suspension. Android Java UI thread is not the SDL main thread; `SDL_RunOnMainThread` is not equivalent to every native dispatch. |
| Thread compatibility | Keep Android `ThreadCompat` stop-token/jthread support. It compensates for the toolchain's C++ library, not SDL2. |

## Further opportunities, deliberately separate

1. **SDL safe-area snapshot:** SDL3 caches the content-view safe rectangle and
   emits `SDL_EVENT_WINDOW_SAFE_AREA_CHANGED`. This can replace repeated
   `FindActiveWindow`/UIKit reads. Publish a thread-safe normalized snapshot,
   refresh before resize/input callbacks and during initial setup, and retain
   current UI scaling. SDL rounds fractional insets upward and uses the content
   view rather than UIWindow; validate notch/home indicator, rotation, keyboard,
   replay export and Stage Manager before adopting it.
2. **Unsupported-event fast paths:** reject irrelevant types before input locks
   and translation allocation; do not skip registry pending dispatch or native
   focus handling. This is ordinary hot-path optimization, not SDL2 residue.
3. **Per-event clock sampling:** one SDL/steady sample pair for all edges of a
   diagonal hat avoids redundant calls and tiny cross-edge rebasing differences.
   Already-steady native timestamps can avoid receipt-clock sampling. Requires
   dedicated timestamp/order tests rather than changing judgment policy here.
4. **Common URL launching:** SDL_OpenURL can replace some wrappers, but Android
   task flags and iOS off-main completion semantics differ. Verify lifecycle and
   failure reporting on devices before deleting those branches.

Display Zoom overrides and keyboard viewport restoration are **not proven
obsolete**. SDL3 still uses nativeScale for Metal sizing; the app also has a
custom native editor. Keep those workarounds pending targeted device evidence.

## Verification

- Regression before/after: old accelerometer special case failed six new normal-
  gain assertions; the corrected queued and realtime paths pass.
- Real SDL3 virtual controller: old code emitted four unwanted completion markers
  across press/release updates; suppression retains raw/mapped button and axis
  events, raw hat events, and disconnect delivery.
- Mobile observation regression: old code queried geometry on every idle tick;
  new test verifies zero queries over 120 mobile ticks and 120 desktop queries.
  Existing normal-resize/maximize rollback test remains.
- Native Android path fixture exercises the actual extracted cache helper with
  canonical aliases and null/empty/unavailable paths.
- Focused desktop build and four focused CTest entries passed.
- Full desktop CTest: **453/453 passed** before the additional fold tests.
- Fold/unfold tests: three focused display entries passed, including the new
  iOS conditional branch. They cover changes within portrait, rotation, and
  fresh mobile snapshots without desktop restore polling. Sizes are illustrative,
  not device specifications; SDL/UIKit calls are controlled boundaries.
- Unsigned iOS device build and signed restricted-access Android build passed.
- Android debug JVM tests: **40 passed across 12 suites**, after forcing a full Java recompilation; stale
  incremental SDL2 class analysis initially prevented the debug Java compile.
- Independent review found an obsolete Java reflection test requiring the deleted
  cache bridge. Removed that test, retaining actual native cache-path coverage;
  scoped recheck approved the fix and fold-test additions.
- iPhone Duo / iOS 27.1 simulator: Debug app built and launched with Metal.
  Closed → open → closed → open → closed → partially open → open transitions
  preserved rendering and Settings state. The same running process reported
  2034×1398 pixels (678×466 logical) closed and 2853×2007 pixels (951×669
  logical) expanded. Settings opened by touch while unfolded, the Display tab
  responded after folding, and Back returned to the main menu after unfolding.
  This validates menu/settings resize and touch routing, not gameplay timing,
  physical hinges, gyro, or every safe-area/orientation combination.
  Evidence: [resize log](evidence/2026-10-09-sdl3-coverage/duo-resize.log),
  [closed Settings](evidence/2026-10-09-sdl3-coverage/closed-settings.png),
  [partially open Settings](evidence/2026-10-09-sdl3-coverage/partially-open-settings.png),
  [unfolded Back result](evidence/2026-10-09-sdl3-coverage/unfolded-after-back.png).

No deployment was run. Physical-device gesture/gyro behavior, safe-area
alternatives and Windows/Linux builds were not newly validated by this pass.

`fixedMobileDisplay` means user-selected desktop display modes are unavailable;
it does **not** mean dimensions cannot change. Native resize/pixel-size events
still query the live drawable, update UI scaling, reset bgfx and refresh gameplay
views. The new tests explicitly preserve that distinction for folding devices.
