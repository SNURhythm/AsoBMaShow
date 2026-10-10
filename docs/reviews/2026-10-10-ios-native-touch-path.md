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

## Before/after measurement

The single-tap comparison **does not demonstrate a latency reduction**. It
confirms complete delivery through the new route, with the measured callbacks
no longer synchronously emitting SDL finger events. Avoid presenting this as
a measured physical-device speedup.

Both builds used the same iPhone Duo iOS 27.1 simulator, arm64 Debug settings,
Apple M1 Pro host with 16 GiB RAM, Metal rendering, and generated seven-lane
test chart. Baseline `fa37465bd` ran first, followed by `60ea9590`. Each run used
300 clicks at the same lane position, producing 600 touch edges and 300 sound
commands. The host input loops took 58.195 s and 57.874 s respectively. No
compiler or test suite was running during either sample.

Values below are p50 / p95, in microseconds:

| Stage | Before | After |
| --- | ---: | ---: |
| Source timestamp → native handler entry | 4,613 / 11,588 | 4,599 / 11,685 |
| Native handler entry → gameplay callback | 7 / 14 | 15 / 41 |
| Gameplay callback → worker enqueue entry | 10 / 14 | 10 / 19 |
| Native handler entry → worker enqueue entry | 16 / 27 | 22 / 45 |
| Source timestamp → worker enqueue entry | 4,631 / 11,603 | 4,623 / 11,708 |
| Full native handler | 32 / 63 | 38 / 84 |
| Source timestamp → worker consumption¹ | 4,900 / 11,900 | 5,000 / 12,200 |
| Sound command → audio callback¹ | 5,300 / 10,400 | 4,700 / 10,300 |

¹ Existing telemetry uses 100 µs histogram buckets; the six detailed handler
segments use 1 µs buckets. All percentiles are bucket upper bounds.

Both runs paired all 600 edges with zero missing pairs; the worker observed
600 inputs and published 300 sound commands. The probe counted 600 synchronous
SDL finger events in the measured baseline handlers and zero after the change.
The new adapter processed 900 samples, including the final coalesced sample
on each release, without generating extra worker inputs for unchanged lanes.

The uncontended handler is slightly more expensive with history collection and
gesture recognition. Most measured delay occurs before native handler entry.
The updated full-handler maximum was 59,308 µs versus 1,184 µs before; this
outlier occurred after the measured enqueue boundary (updated maximum enqueue
duration from handler entry was 89 µs). Its cause was not isolated. The patch
removes SDL lock/dispatch dependencies but cannot promise better scheduling
tails from this test.

### Measurement boundaries and limitations

- The baseline handler begins at `SDL_uikitview::touches*`; the updated handler
  begins at `AsoGameplayTouchRecognizer::touches*`. These are distinct UIKit
  delivery points, so their entry-time difference is not purely OS scheduling.
- Identical observer logic wraps the existing Objective-C handlers and records
  gameplay callback entry and worker enqueue entry. It records only successful
  enqueue operations, after queue publication and worker wakeup. Full-handler
  duration includes observer overhead, which has not been subtracted.
- Detailed histograms dump only after at least one second without an enqueue.
  Existing five-second telemetry logging remains enabled in both builds.
- The source timestamp is UIKit's simulated touch timestamp. The experiment
  includes simulator/host delivery and app scheduling, not a physical digitizer.
  It is one sequential run per build, without forced SDL contention.
- The simulator negotiated 8 kHz audio and approximately 85–86-frame callbacks
  (about 10.7 ms) in both runs. Those host audio timings do not characterize
  physical iOS audio output or the app's requested 128-frame device buffer.
- Modifier-click in this Device Hub produced only one contact, confirmed by
  the counters. It was excluded from these samples. Simultaneous-finger and
  coalesced-history ordering are covered by regression tests; their physical
  timing remains unmeasured because the iPad was unavailable.

The measurement patches were removed, the normal updated simulator app was
restored, and the generated chart folder was deleted after testing.

### Evidence

- [Machine-readable comparison](evidence/2026-10-10-ios-native-touch/comparison.json)
- [Baseline snapshot](evidence/2026-10-10-ios-native-touch/baseline-snapshot.log)
  and [updated snapshot](evidence/2026-10-10-ios-native-touch/updated-snapshot.log)
- Measurement-only patches for [baseline](evidence/2026-10-10-ios-native-touch/baseline-instrumentation.patch)
  and [updated code](evidence/2026-10-10-ios-native-touch/updated-instrumentation.patch)
