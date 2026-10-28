# Device timing capture 23: steady frames are prepared early

> The later-preparation recommendation below is withdrawn. The deliberate wait
> was removed in `921a82d5`: a lower preparation-age metric did not establish
> better input responsiveness, and capture 24 exposed extra deadline misses.
> Keep the measurements as historical evidence, not a pacing recommendation.

The current display-link capture supports the **early-preparation** explanation
for steady gameplay. Displayed frames reach their assigned target essentially on
time. Their median GPU work is 1.12 ms, followed by 14.14 ms waiting for display;
preparation starts 16.61 ms before display. The long wait is largely already
present in the target chosen by the display link, rather than a recurring miss
of that target.

This updates the hypothesis from the older aggregate lookback capture. That
capture's estimated deadline suggested a one-refresh penalty, but it predates
the current implementation. The new data correlates explicit targets and actual
display timestamps for individual frames. It does not retroactively explain the
older implementation or establish physical input-to-photon latency.

## Source and selection

Input: user-supplied `AsoBMaShow-performance-log 23.txt`. Its timing-v1 section
contains 7,764 consecutive frame IDs, covering 66.6584 seconds of accepted
callbacks, with `dropped_updates=0` and one lifecycle generation. It has not
reached the 8,192-row capacity. All numeric fields are finite, callback times
increase, and displayed gameplay records have nonzero stages with ordered GPU
start/end, request/scheduled-handler, and GPU-end/display times.

The log contains no build/device identity. Timing-v1 matches this experiment's
schema; the exact installed commit and device conditions are not independently
established by the file. The original includes private library paths and is not
committed. [Derived summary](data/2026-10-28-metal-log23-summary.json) includes its
SHA-256, counts, metrics, outlier IDs and one example frame without library data.

There are 6,939 gameplay-tagged callbacks:

- 6,761 have a presentation request and GPU timestamps.
- 6,754 also have an actual display timestamp. These form the displayed cohort
  in the table below; no additional statistical outlier exclusion is applied.
- Seven submitted frames have no display timestamp: 542, 543, 544, 704, 1360,
  7419 and 7472. Their display timing is unknown, not zero latency.
- 178 have no submission, mainly in the idle interval near the end of the
  gameplay-tagged segment. The gameplay flag does not guarantee active drawing.

This records only accepted display-link callbacks. No missing token IDs and zero
contention losses do not prove that every OS callback or every intended display
refresh was captured. Missing display events may reflect non-display or callback
availability; this recorder does not distinguish those cases.

## Results

All durations below use the 6,754 displayed gameplay frames. Quantiles use linear
interpolation at `(n - 1) * p`; units are milliseconds. Metrics are paired per
frame before aggregation, not differences of aggregate timestamps.

| Interval | Median | P95 | P99 |
| --- | ---: | ---: | ---: |
| Preparation start → presentation request | 1.140 | 1.841 | 2.877 |
| Preparation start → scheduled-handler observation | 1.269 | 2.013 | 2.962 |
| GPU start → GPU end | 1.122 | 1.268 | 1.322 |
| Preparation start → GPU end | 2.481 | 3.320 | 4.335 |
| GPU end → display | 14.139 | 14.546 | 14.819 |
| Preparation start → display | 16.615 | 16.656 | 16.665 |
| Presentation request → submission deadline (remaining margin) | 7.140 | 7.522 | 7.856 |
| Actual display − target display | +0.008 | +0.030 | +0.032 |

Every displayed gameplay frame has an early CPU request and scheduled-handler
observation. Actual display minus target ranges from −0.188 to +0.033 ms; none
is more than 1 ms late. The scheduled handler is an observation rather than the
exact internal drawable-present call, but the actual display timestamps directly
show that these frames met their assigned targets.

The target is consistently 8.3334 ms after the submission deadline. The callback
arrives roughly another refresh before that deadline, putting the assigned
display time approximately 16.6 ms ahead of callback receipt. The application
then begins preparation immediately. This is the source of the large available
margin and of the displayed state's age in the steady records.

The 6,750 intervals between consecutively numbered displayed gameplay frames
have a median of 8.333458 ms and range 8.333250–8.333667 ms: steady 120 Hz cadence.
There are three longer observed display gaps, discussed separately below.

## A concrete frame

Frame 2789 is a near-median example, not a synthetic sum of medians. Relative to
its preparation start:

| Event | Time (ms) |
| --- | ---: |
| Preparation start | 0.000 |
| Presentation request | 1.142 |
| Scheduled-handler observation | 1.263 |
| GPU start | 1.372 |
| GPU end | 2.496 |
| Submission deadline | 8.296 |
| Target display | 16.629 |
| Actual display | 16.624 |

It meets the intended target and spends 14.128 ms between GPU completion and
display. Neither a shorter GPU kernel nor removing approximately 0.121 ms of
request-to-scheduled-handler time addresses most of that interval.

## Stalls and incomplete frames remain relevant

Across all 6,761 gameplay submissions, one CPU request is late: frame 704 spends
116.738 ms from preparation to request and crosses the deadline by 108.493 ms.
It has no display timestamp. The surrounding displayed frames 703→705 are
125.002 ms apart. This is a real CPU-side stall, separate from steady waiting;
the available timestamps do not identify the function responsible.

Frame 1360 has no display timestamp despite requesting 0.612 ms before its
deadline; its scheduled-handler observation leaves only 0.410 ms. GPU completion
is after that deadline, which Apple's API allows. Frames 1359→1361 are 16.667 ms
apart. This narrow-margin case argues for a safety budget when moving work later;
it does not prove why that specific frame lacks a display event.

The final 1,500.023 ms gap (7299→7479) is mostly no-submission callbacks while
the gameplay flag remains set, plus two submitted frames with no display event.
It must not be counted as a steady rendering cadence. Menu records additionally
include two roughly 323 ms preparation/request stalls and other short misses.
No menu or loading-state explanation is inferred from the flag alone.

## Next controlled change

The next targeted experiment is to **start input draining and scene preparation
later**, while retaining the current target and presentation contract. Use a
measured preparation/encoding/scheduling budget plus a safety margin, recheck
lifecycle/export/quit state around any bounded wait, and preserve the existing
fallback and three-drawable/GPU-limit-three configuration.

Do not add an unconditional fixed sleep: shifting the existing scheduled-handler
observations by 5 ms would place 35 of the 6,754 displayed records past the
deadline even assuming unchanged workloads. A 4 ms shift would put six past it.
Those are sensitivity calculations, not predictions of an adaptive implementation.
Warm-up, transitions and recent overruns require conservative handling.

A several-millisecond reduction in age since scene preparation is plausible from
the available margin, but must be measured in a paired device run alongside
cadence and missed/unknown display events. Delaying only presentation after the
scene was sampled would improve the delay metric without making the sampled
state newer. Hardware/event-to-photon latency remains unmeasured.

The current `preferredFrameLatency = 1.0` is already the smallest documented
setting; Apple accepts only 1.0 or 2.0 and may provide greater total latency.
[Apple: preferredFrameLatency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency).
The submission deadline requires the drawable presentation call before it;
GPU work may continue afterward.
[Apple: targetTimestamp](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp).

No runtime pacing change or deployment accompanies this analysis.
