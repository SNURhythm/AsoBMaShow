# Capture 25: intermittent worker stalls, not sustained GPU saturation

The strongest targets are a long preparation stall and work around the gameplay
transition. Typical GPU rendering is comfortably below one 120 Hz interval.
This capture does not identify a particular expensive shader, lock, allocation
or I/O operation; it does locate intervals to instrument next.

## Source and method

User-supplied `AsoBMaShow-performance-log 25.txt`, reported as the latest pushed
build (`921a82d5`, deliberate preparation wait removed). The file itself does not
embed build/device identity. Its raw contents include private paths and are not
committed. The [derived summary](data/2026-10-28-metal-log25-summary.json) records
the SHA-256, cohorts, distributions and exceptions without those paths.

There are 8,192 retained continuous token IDs, 14571–22762, spanning 68.851 seconds
of callback receipt, generation zero, and zero recorder contention losses.
Earlier 14,570 tokens were overwritten. All numbers are finite. Token continuity
covers accepted callbacks, not every OS callback or every intended refresh.
Gameplay is a sampled flag, not a scene identifier or proof of active play.

Durations are paired timestamp differences, in milliseconds; quantiles interpolate
at `(n - 1) * p`. No statistical outliers are removed. There are 7,536 gameplay
submissions, 7,535 with display timestamps. Of 656 non-gameplay records, 616 have
requests and 611 have display times. The snapshot tail is incomplete; missing
display times cannot automatically be counted as dropped frames.

## Steady gameplay

Across displayed gameplay records, preparation-to-request median/P99 is
**0.782/2.057 ms**, and GPU duration is **1.188/1.359 ms**. The maximum measured
GPU interval in this cohort is 1.999 ms. All displayed gameplay frames meet their
assigned target within −0.439 to +0.033 ms. The 7,533 consecutive-ID gameplay
display intervals range from 8.333125 to 8.333708 ms.

Median GPU-end-to-display is **14.363 ms**; it is waiting for scheduled display,
not GPU execution. Median preparation-to-display is **16.640 ms**, and callback
to preparation is only 0.00446 ms. These are not input-to-photon measurements.
Compared with captures 23/24, CPU median is lower and GPU median slightly higher;
different retained windows and unmatched conditions prevent causal attribution.

## Actual stalls

- **21865, gameplay:** preparation takes **66.868 ms** before `bgfx::frame`;
  preparation-to-request totals 67.037 ms. GPU work is only 1.070 ms. The request
  misses its deadline by 58.784 ms and lacks a display timestamp. Surrounding
  displayed frames are **75.001 ms** apart. This is elapsed worker-side
  preparation delay, not proof of CPU busy time or any specific function.
- **22106, last gameplay-tagged frame:** the frame renders and displays on target,
  but request-to-iteration-end lasts **360.879 ms**. The next callback arrives
  0.036 ms after that end. This is post-request application-worker delay, not a
  wait for this frame to become visible. `sceneManager.handleDeferred()` is in
  this interval and can synchronously transition/clean up/resume scenes; the
  capture cannot attribute the time to it rather than other post-request work.
- **22107, first non-gameplay frame:** preparation-end-to-render-submit takes
  **42.480 ms**, inside `bgfx::frame`; total CPU elapsed interval is 47.926 ms.
  GPU work takes 5.530 ms. This and the next frame have no display timestamp;
  the next GPU interval is 5.780 ms. The combined observed display gap from
  22106 to 22109 is **425.005 ms**. These are the only GPU intervals above 4 ms;
  worker/renderer elapsed stalls account for far more of the transition gap.
- **22532, non-gameplay:** preparation takes **134.503 ms**, GPU only 0.852 ms.
  It displays 133.346 ms after target. Another preparation-side stall, with no
  function-level attribution.

The separate 341.671 ms display gap at 22534–22575 contains 38 no-submission
callbacks plus two submissions without display timestamps. Processing continues;
it must not be labelled a continuous 342 ms rendering stall.

## Input and next investigation

Native gameplay input is routed into `RealtimeGameplayWorker::enqueueInput`,
which signals the independent worker. That worker processes timestamped input,
judges and commits keysound commands, then publishes snapshots for rendering.
The render-loop delay metric therefore does not directly measure judgement or
keysound latency. Visual feedback still depends on snapshot capture and display.
UI/legacy input paths and shared-resource contention require separate analysis.

Prioritize token-correlated spans around post-submit work and deferred scene
transitions/cleanup/resume, then preparation subdivisions and bgfx encoding and
resource work. Keep CAMetalDisplayLink and immediate preparation. Do not move
input or timestamps to make preparation-age statistics smaller.

The exported file lacks detailed stage timings, scene names, stacks, per-pass
GPU measurements and per-frame input correlation. Ordinary archive/preview log
messages use a different relative clock without a clock bridge, so they cannot
reliably identify the timed stalls. The fallback two-slot gate is bypassed for
these managed display-link frames; this capture does not implicate that gate or
establish a benefit from restoring the original pass order.
