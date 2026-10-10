# iOS presentation pacing and touch latency

Timed presentation pacing reduced simulator touch-to-worker p95 from 16.9 ms
to 6.7 ms in the final 300-tap comparison (about 60%). Both runs delivered all
600 touch edges and 300 keysound commands. This measures software input
delivery on the simulator, not physical touchscreen-to-speaker latency.

## Change

iOS renders bgfx/Metal on UIKit's main thread. With the default frame-cap value
of zero, the application previously submitted frames continuously and could
block that thread in Metal instead of servicing UIKit input.

The default iOS presentation path now uses the existing `FramePacer` at the
display's refresh rate and spends its idle interval in
`WaitIOSMainRunLoopForMicros`. That helper services the default and tracking
run-loop modes in bounded slices. Native gameplay callbacks can therefore
enqueue input during the wait, without waiting for another SDL poll or render.
SDL's UIKit display backend obtains this rate from
`UIScreen.maximumFramesPerSecond`; the policy preserves 120 Hz and falls back
to 60 Hz if no usable display rate is available.

This is an additional presentation pacer for the existing iOS VSync path.
The user's saved settings are unchanged. An explicit nonzero frame cap keeps
using its existing pacer, and replay export bypasses the automatic pacer.
Foreground and export transitions reset its deadline. Rendering remains on
UIKit's main thread. Android, desktop pacing, input routing, audio buffer
sizing, and the SDL submodule are unchanged.

The implementation is in `src/main.cpp` and
`src/video/IOSPresentationPacing.h`. Policy tests cover 60/120 Hz, fractional
rates, invalid rates, explicit caps, and export. Existing frame-pacer tests
cover overrun, deadline phase, cap changes, and foreground reset.

## Measurement

The final runs used the same iPhone Duo iOS 27.1 simulator, arm64 Debug build,
Apple M1 Pro host with 16 GiB RAM, Metal renderer, generated seven-lane chart,
and `video.frameCap = 0`. Both builds used the same temporary measurement
hooks described in the [native touch report](2026-10-10-ios-native-touch-path.md).
The control was the previously measured `60ea9590` production code, equivalent
to `0293a3c4` except for documentation. The updated run preceded the final
control, providing a check against the earlier control-first experiments.

Each run used 300 clicks at the same lane position: 600 DOWN/UP edges and 300
sound commands. Input loops took 53.690 s updated and 55.858 s control. No
compiler or test suite ran during these measurements. The six detailed
handler segments paired all 600 edges, with zero missing pairs and zero
synchronous SDL finger events in both runs.

Values are p50 / p95 / p99 / maximum, in milliseconds. The standard telemetry
percentiles are upper bounds from 0.1 ms histogram buckets.

| Stage | Control | Timed pacing |
| --- | ---: | ---: |
| Touch timestamp → worker consumption | 4.7 / 16.9 / 21.8 / 31.991 | 1.7 / 6.7 / 9.6 / 19.490 |
| Touch timestamp → sound command | 4.4 / 18.0 / 19.9 / 21.857 | 1.8 / 7.6 / 10.4 / 19.531 |
| UIKit handler entry → worker enqueue entry¹ | 0.021 / 0.045 / 0.051 / 0.085 | 0.021 / 0.036 / 0.044 / 0.048 |
| Full UIKit handler¹ | 0.037 / 0.088 / 0.133 / 6.483 | 0.036 / 0.062 / 0.089 / 15.720 |

¹ The detailed handler probe uses 1 µs buckets. Its last bucket contains all
samples at or above 16.384 ms and reports the sample maximum as an upper bound
when a percentile lands there. Consequently the final control's detailed
source-to-enqueue p95 is only known to lie in **[16.384, 31.978] ms**; 31.978 ms
must not be presented as its exact p95. Updated source-to-enqueue p95 was
5.828 ms. The headline uses the standard touch-to-worker histogram, which
resolves the control's p95 without this overflow ambiguity.

The updated full-handler maximum is worse despite improved input delivery;
that 15.720 ms sample occurs after enqueue, whose maximum handler-to-enqueue
time is 0.048 ms. Its cause was not isolated. Pacing does not eliminate host
scheduling or UIKit stalls.

The updated run averaged 60 FPS with a final reported 1% low of 59.4 FPS. The
control's final cumulative average was 163.4 FPS, with large variation in
submission rate and a final five-second rate of 50.4 FPS. These are application
frame submissions, not measurements of physical display scanout. Audio
callback configuration was unchanged; simulator audio timing does not predict
physical-device output latency.

## Experiments that led to this change

Each exploratory run used 150 taps / 300 edges. These were separate sequential
runs, not randomized trials, so the final comparison above is the primary
result.

| Experiment | Source → enqueue p50 / p95 | Touch → worker p95 |
| --- | ---: | ---: |
| Original code, manual 120 FPS cap | 2.506 / 6.437 ms | 8.6 ms |
| Original code, repeated uncapped control | 4.775 / 13.882 ms | 14.0 ms |
| Rejected CADisplayLink pacing | 6.900 / 15.280 ms | 15.5 ms |
| Original code, manual 60 FPS timed cap | 1.353 / 7.425 ms | 7.7 ms |

The display-link prototype reduced submissions but did not improve touch
delivery. It was removed completely. The manual 120 FPS, display-link, and
manual 60 FPS experiments also had large source-to-enqueue maxima of 323.949,
218.885, and 190.572 ms respectively; their causes were not isolated. All
exploratory snapshots, including these outliers, are retained in the evidence.

Apple documents that [`CAMetalLayer.nextDrawable`](https://developer.apple.com/documentation/quartzcore/cametallayer/1478172-nextdrawable)
can wait when drawables are unavailable and that
[`CADisplayLink`](https://developer.apple.com/documentation/quartzcore/cadisplaylink)
participates in display timing. The attribution to excessive main-thread
render submission is supported by the pacing experiments, rather than a
measured decomposition of every Metal stall.

## Verification

- Desktop `main` and frame-pacer tests built successfully. The new policy test
  failed before implementation and passed afterward.
- The final full CTest run passed all 471 tests in 138.73 seconds.
- The production iOS simulator build passed. Gameplay input, pause/resume,
  Retry, background/return, and further input/pause passed smoke checks.
- `scripts/ios_release_verify.sh` passed all 68 native checks, 53 build-setup
  tests, 20 release-workflow tests, 18 artifact-audit tests, the unsigned iOS
  release build, and its artifact audit.
- Code review found no blocking issues. The simulator's original settings
  were restored, the generated chart and its database rows were removed, and
  the production application was installed with no measurement hooks.

No build was distributed.

## Evidence and limits

- [Comparison data](evidence/2026-10-10-ios-presentation-pacing/comparison.json)
- [Final control snapshot](evidence/2026-10-10-ios-presentation-pacing/control-final.log)
- [Final timed snapshot](evidence/2026-10-10-ios-presentation-pacing/timed-final.log)
- [Temporary updated instrumentation](evidence/2026-10-10-ios-presentation-pacing/timed-instrumentation.patch)
- [Control instrumentation](evidence/2026-10-10-ios-native-touch/updated-instrumentation.patch)

Instrumentation is retained only as evidence patches, not compiled into the
production application. The sample establishes an improvement for this
simulator workload. A physical-device test is still needed to quantify
digitizer delivery, ProMotion behavior, heavy-skin workloads, simultaneous
fingers, and audible output. Replay export bypass is covered by policy tests
and code review, not a new end-to-end video export benchmark.
