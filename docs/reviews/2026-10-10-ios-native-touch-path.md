# Direct UIKit gameplay touch input

The iOS gameplay path now uses an app-owned UIKit gesture recognizer before
touches reach SDL's view. Android and iOS share native callback registration,
session epochs, callback lifetime protection, and the bounded queue used to
deliver presentation input on the main loop.

## Why change the existing raw callback?

The previous iOS path already called gameplay synchronously from
`IOSPushRawTouchEvent`; it did not wait for the next frame's SDL event polling.
However, `SDL_uikitview::touchesBegan` first called `SDL_AddTouch`, and each
finger's raw callback was followed by `SDL_SendTouch`. SDL touch/event-watch
locks could therefore delay a finger before its callback or delay the next
finger in the same UIKit batch. The raw callback also used SDL's raw-touch
spinlock. Moving an already synchronous callback earlier addresses those
dependencies; it does not remove UIKit delivery or rendering delays.

## Ownership and shared code

- `src/input/NativeRawTouchInput.{h,cpp}` owns the platform-neutral registration,
  epoch checks, callback lifetime, and deferred UI queue. The Android header
  remains a compatibility alias, with 64-bit contact identities for UIKit.
- `src/input/IOSTouchInput.mm` admits direct finger touches only while a native
  gameplay registration exists. Immediate recognition with delayed view
  delivery keeps owned gestures out of SDL. Menus, native child controls,
  pencil input, and independent mouse input retain their existing routes.
- `src/input/IOSTouchGestureRouter.h` latches ownership before DOWN. A gesture
  cannot transfer into a replacement session during Retry, or acquire a
  session halfway through a menu gesture.
- `GamePlayScene` uses the same native ingress and deferred presentation path
  for both mobile platforms. iOS keeps its existing cancellation grace period.
  Pause/background transitions cancel presentation contacts and gate gameplay;
  detachment waits for in-flight callbacks before destroying the worker.

Real coalesced UIKit samples retain their timestamps and original contact
identity. Histories from all fingers in a batch are merged chronologically
before worker enqueue. Duplicate and older samples are ignored; equal-time
release edges survive. Predicted samples are not used for judgement.

The recognizer is installed on the SDL Metal view and removed before that view
is destroyed. If installation fails, the existing iOS SDL raw callback remains
available. SDL itself is unchanged. Audio buffer sizing is unchanged by this
patch; iOS already requests 128-frame callbacks without fixed-size buffering.

## Verification scope

Regression coverage includes 64-bit contact identity, menu-to-gameplay and
Retry ownership, chronological multi-finger history, cancellation/reset,
deferred presentation delivery, native callback teardown, and pause/resume
for both mobile platform branches.

The desktop build and iOS simulator build passed. `ios_release_verify.sh`
passed its 68 native checks, 91 Python release-contract checks, unsigned iOS
release build, and artifact audit. The full 471-test CTest run passed 469;
`music_select_settings_runtime` hit its Lua callback assertion and
`foundation_profile_switch` timed out during concurrent platform compilation.
Both passed on a separate rerun without code changes (0.05 s and 9.09 s).
Simulator smoke checks exercised gameplay input, pause/resume, Retry, and
background/return followed by more gameplay input and pause.
The shared code also passed the signed Android restricted-file release build
and APK packaging through `scripts/android_firebase_deploy.sh --build-only`.

Simulator timing measures software behavior on the host. It cannot establish
physical touchscreen, device scheduling, or speaker/DAC latency. Physical iOS
measurements require a connected device.
