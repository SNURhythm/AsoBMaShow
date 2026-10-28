# Presentation-completion pacing experiment

**Rejected after device testing.** Candidate `486da733` lowered reported HUD
Present Delay to about 14.5 ms, but frame rate fell from 120 to 60 FPS. The
presentation callback and wait were removed, restoring the `469a7f9c` runtime
configuration: three drawables, Apple GPU limit one and the prior timer pacing.
The sections below retain the experiment's design and local verification record.

The user approved this controlled test after the lookback trace showed long
GPU-end-to-display delay and no observed improvement from GPU limits 3, 2 or 1.
Baseline: `2f02fd77`, whose runtime is the one-frame candidate `469a7f9c`.

## Scope and behavior

- Keep three drawables, GPU limit one on Apple platforms, existing rendering,
  `presentDrawable`, bgfx revision, reset flags and shader binaries unchanged.
- Add an iOS-only primary-drawable presentation callback to the generated bgfx
  backend. Track a sequence token, not a retained drawable or command buffer.
- After submitting a normal frame, wait on the application worker for its
  presentation callback before starting another frame. UIKit and realtime
  input/audio remain independent. No CAMetalDisplayLink is introduced.
- Bound the entire wait to 50 ms. Check background, export and quit cancellation
  at least every 2 ms. Missing callbacks or dropped frames cannot stall the
  application indefinitely. Do not wait during export or inactive lifecycle state.
- Consume timed-out/cancelled tokens. Late callbacks must not release a later
  frame. Reset pending state across lifecycle/export bypass; never reset the
  monotonically increasing token identity while old callbacks can remain.
- Preserve the existing frame-start-based timer calculation after the wait so
  time spent waiting consumes the current frame budget.
- No new telemetry, per-frame logging, files, display acquisition provider or
  changes to normal GPU/resource ownership. The callback is the pacing mechanism.

## Implementation and verification

1. Write focused behavioral tests for completion before/after waiting, timeout,
   cancellation, stale callbacks, superseded tokens and native callback lifetime.
   Use explicit synchronization rather than fixed sleeps.
2. Implement the small completion gate and generated Metal hook, then integrate
   the cancellable wait at the existing iOS pacing boundary. Extend relevant
   build-setup coverage, retaining upstream-source immutability and fixed-drawable
   assertions. Add the test target to the rendering group.
3. Run focused gate, Metal readback, native video lifetime and iOS pacing/lifecycle
   checks; build desktop `main` and an unsigned iOS device build. Use one build
   per directory. Perform one independent review of the experiment, then commit
   and push on the current branch. No deployment or installation is authorized.

The user previously requested lower-effort implementation subagents. Delegated
implementation uses Astra Medium, with the parent integrating and verifying.
Repository instructions prohibit creating a worktree; use the current checkout.
Focused verification takes precedence over skill defaults requesting repeated
full-suite runs for this localized experiment.

## Device decision

Compare the same chart, settings, warmup and HUD mode with `469a7f9c`. Keep the
experiment only if delay improves while 120 FPS remains stable. Revert it if
cadence drops or delay does not improve. Passing local tests proves the wait's
contracts and build compatibility, not a device latency benefit.

Review particularly for stale callback wakeups, waiting during export/background,
holding a renderer lock while waiting, double pacing, native resource retention,
and accidental activation on non-iOS backends.

## Local outcome

Implemented the experiment with the wait outside `bgfxRenderMutex`. iOS uses
single-threaded bgfx with flip-after-render, so the primary presentation token
is registered before the application reaches the wait.

- Desktop `main` and the unsigned iOS device build passed.
- All 89 selected rendering, video and iOS CTest checks passed. These include
  actual Metal readback and native NV12 texture lifetime at GPU limits 1, 2 and 3,
  in single-threaded and multithreaded configurations.
- All 56 iOS build-setup checks passed. The native callback fixture verifies
  token-only capture, drawable release, late callbacks, secondary-window bypass
  and the disabled non-iOS path using the generated registration block.
- Independent review found no production correctness issues. Its positive
  timeout coverage suggestion was added: requesting a one-second wait without
  a callback exercises the 50 ms clamp and token consumption. The gate test
  passed again after this test-only addition.

No build was uploaded or installed by the agent during local verification.

## Device outcome

The user subsequently reported approximately 14.5 ms Present Delay and 60 FPS
on the iPad. This fails the requirement to preserve 120 FPS, so the experiment
was reverted rather than retained for its small delay reduction. Delay at the
reduced frame rate does not demonstrate an improvement at the target cadence.

The result is consistent with the completion wait preventing useful overlap
between consecutive frames. It does not establish precisely when the callback
arrived or which display deadline was missed; those events were not measured.
Waiting for visible presentation before beginning the next frame is unsuitable
for this device's 120 FPS path as implemented.

The rollback passed desktop and unsigned iOS builds, three focused Metal
build-setup checks, and 86 of 87 rendering/video/iOS checks on the first run.
The unchanged download-recovery fixture failed its retained-offset assertion
and passed an isolated rerun; its archive-content assertions passed in both.
Production code and test registration match the pre-experiment baseline exactly.
