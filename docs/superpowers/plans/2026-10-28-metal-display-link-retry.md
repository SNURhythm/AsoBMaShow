# CAMetalDisplayLink retry on the updated Metal backend

## Objective

Try CAMetalDisplayLink on the current bgfx backend while retaining the accepted
two-slot drawable acquisition path as fallback. The user explicitly requested
this experiment after reporting about 15.9 ms Present Delay at 120 Hz on a warm,
fast-charging iPad. They subsequently reported that pausing and resuming gameplay
reduces the delay to about 12 ms until exiting and starting gameplay again.

The earlier display-link experiment reported about 23 ms and was removed. This
retry uses the updated backend and keeps its ordinary command-buffer presentation
and drawable ownership. It does not establish a latency improvement before a
device comparison.

## Design constraints

- Enable on iOS 17 and later, on the existing application/render worker run loop.
  Set preferred frame latency to 1. Keep three layer drawables and GPU limit 1.
- Accept the callback's drawable before input/scene update. bgfx consumes that
  borrowed drawable with its normal retain/release ownership and ordinary
  command-buffer `presentDrawable` scheduling.
- Never call `nextDrawable` for a layer while a display link is attached. A
  managed layer with an invalid, missing, or already consumed lease returns nil;
  only a detached layer may fall back to ordinary acquisition.
- Check lifecycle generation, layer geometry, export ownership, and quit state.
  Detach before backgrounding, owner-thread work, inline export, prepared-frame
  retirement, or timer fallback. Invalidation stays on the owning worker.
- Bound missing-callback waits and back off before retrying. Reject stale
  callbacks without creating an unbounded wait or retaining stale drawables.
- A display-link frame bypasses the two-slot acquisition gate and post-submit
  timer wait. All other frames retain current timer/gate behavior. Cancel old gate
  reservations on transition; stale presentation callbacks remain harmless.
- Keep supported caps: display refresh for unlimited/at-or-above refresh, and
  exact refresh divisors of at least 30 Hz. Other caps use the timer path.
- Leave gameplay/audio clock semantics, macOS app activation, and diagnostic
  telemetry unchanged. No deployment is requested.

## Work and verification

1. Add a worker-owned native bridge, pure lease/rate policy, and a small drawable
   provider contract; integrate with the current generated bgfx Metal source.
2. Integrate main-loop pacing, lifecycle/export handoff, and prepared-frame
   retirement. Extend behavioral tests for lease ownership, fallback transitions,
   cap policy, and native provider acquisition/presentation behavior.
3. Build `main`, rendering/video/iOS-platform test groups, and run those CTests.
   Run the focused iOS build-setup checks and the unsigned iOS build helper.
4. Independently review the complete experiment, update PR 161, commit and push.

## Device comparison

Use the same chart, settings, warm-up, and thermal/charging conditions as the
accepted acquisition-gate candidate. Record Present Delay and FPS separately for
fresh gameplay and after pause/resume. Require sustained 120 FPS as well as useful
latency behavior. Compare against `7b4f9c01` (runtime `3f018f0a`). If the experiment
regresses, remove this retry while preserving the accepted fallback candidate.

Pause/resume resets playback clock state and can stop idle rendering, but does
not directly reset the drawable pool or presentation gate. The observed reduction
is evidence for investigating timing/queue state; its cause is not established.

## References

- [CAMetalDisplayLink](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink)
- [Preferred frame latency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency)
- [Metal command-buffer presentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:))

## Result

Implemented the worker-owned bridge and native drawable provider on the current
backend. The timer path retains the two-slot acquisition gate. Detachment covers
owner work, inline export, background waits, discarded prepared frames, and
shutdown. An attached layer remains managed even between frame leases.

Verification:

- Desktop `main` plus rendering, video, and iOS-platform test targets built.
- All 92 selected CTests passed in 13.87 seconds, including native Metal
  readback/video checks and the new provider and export handoff fixtures.
- All three affected iOS build-setup checks passed.
- `scripts/ios_firebase_deploy.sh --build-only` passed the unsigned iOS device
  build, compiling the new bridge and generated Metal backend.
- The initial CMake run caught a test registration placed before its helper
  definition; moving it beside the existing frame-pacing registrations fixed it.

The native ownership fixture attaches a real CAMetalDisplayLink to a real Metal
layer and deliberately seeds its lease with a real drawable. It checks idle and
consumed leases, normal presentation, lifecycle rejection, detachment, and timer
fallback without depending on headless callback delivery. This verifies ownership
behavior, not live callback cadence or device latency.

Independent review passed specification and code-quality checks with no remaining
actionable correctness findings. No deployment or installation was performed by
the agent.

## First device result and decision

After the `490ab0b7` candidate, the user reports approximately **15.1 ms Present
Delay before pausing, with 120 FPS sustained**. This is about 0.8 ms below the
previous 15.9 ms acquisition-gate report and does not reproduce the earlier
presentation-wait experiment's drop to 60 FPS.

Keep this display-link candidate. Treat the improvement as provisional: the
previous report was on a warm iPad using a fast charger, and matching thermal and
charging conditions have not been confirmed. Post-resume delay for this candidate
has not yet been reported; the earlier approximately 12 ms post-resume result
belongs to the acquisition-gate candidate and remains a separate comparison.
