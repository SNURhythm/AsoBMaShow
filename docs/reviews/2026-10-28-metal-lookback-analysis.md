# Metal lookback capture: delay after GPU completion

The supplied iPad trace shows that GPU rendering is short while completed frames
wait substantially longer to reach the display. During steady 120 FPS periods in
the latest app session, mean GPU active time is **1.06 ms**, GPU end to display is
**14.66 ms**, and displayed-frame interval is **8.34 ms**. This supports focusing
the next investigation on presentation scheduling. It does not identify the exact
queue, deadline or system policy causing the delay.

The user observed no improvement from GPU limits three, two or one and confirmed
**Direct** mode in the HUD. The GPU semaphore is signaled on command-buffer
completion; it does not limit frames waiting for display. A fast GPU can finish
before the next application frame starts, leaving a lower GPU limit ineffective
against this later delay.

## Source and method

Input: `LookbackCollection_26-10-27_230152_to_26-10-28_000152.aar`, containing one
Apple `.atrc` trace. Its recorded range is October 27, 23:01:54 to October 28,
00:01:52 KST, on an M5 iPad Pro running iPadOS 27.2. It was extracted with `aa`,
read with `metalperftrace overview --json --json-include-timeline`, and imported
into Xcode 27.1's Game Performance Overview template with `xctrace`.

The three AsoBMaShow sessions have one 2420 × 1668 BGRA8Unorm SDL Metal layer
each. They contain 54, 28 and 107 statistics intervals, respectively. The
[derived CSV](data/2026-10-28-metal-lookback.csv) preserves these 189 intervals.
The archive and unrelated process data are not committed.

For the steady-rate comparison below, include intervals with 119–121 presented
frames, 0.95–1.05 active seconds, and a mean on-glass interval of 8.2–8.5 ms.
These criteria separate near-120 FPS rendering from startup, menu/idle and
transition intervals. Means are weighted by each metric's native sample count,
using its reported total; they are not averages of interval means. Metric totals,
means and sample counts were checked for consistency.

| Session start (KST) | PID | Selected intervals / frames | GPU active | GPU end → display | Wait for drawable | Displayed-frame interval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Oct 27 23:39:17 | 5808 | 34 / 4,077 | 1.00 ms | 14.59 ms | 4.71 ms | 8.34 ms |
| Oct 27 23:45:33 | 5826 | 16 / 1,918 | 1.05 ms | 14.69 ms | 2.72 ms | 8.34 ms |
| Oct 27 23:50:48 | 5846 | 69 / 8,277 | 1.06 ms | 14.66 ms | 5.08 ms | 8.34 ms |

The trace lacks application commit and GPU-limit metadata. Session timing may
correspond to the sequential test builds, but that mapping is not established
by the capture and is not used as a controlled comparison.

## A continuous 30-second example

PID 5846, October 28 **00:01:20.382–00:01:50.382 KST**, contains 3,599 presented
frames across 30 consecutive one-second intervals:

| Metric | Sample-weighted mean |
| --- | ---: |
| GPU active time | 1.07 ms |
| GPU begin to end | 1.16 ms |
| Waiting in `nextDrawable()` | 6.26 ms |
| CPU begin to client present | 6.96 ms |
| GPU end to display | 15.03 ms |
| CPU begin to display | 23.27 ms |
| Displayed-frame interval | 8.34 ms |

These are separate measured intervals; their boundaries can overlap. CPU begin
is the first associated command-buffer creation or drawable acquisition, not
input arrival or the start of scene update. The CPU numbers are elapsed wall
time, not CPU execution time. Thus 23.27 ms is not an input-to-photon estimate.

Lower delay is also present in the capture: PID 5826's 23:45:36.093–23:45:37.094
interval has 120 frames at an 8.33 ms on-glass interval, 0.78 ms GPU active time
and 9.05 ms GPU end to display. Scene and transition information is absent, so
this is not proof that a particular pause/resume action caused an improvement.
It does show that a roughly 15 ms post-GPU delay is not universal in this capture.

## Metric definitions and limits

Instruments embeds definitions in the duration-table export. An independent
check matched the XML metric values to the JSON fields:

| JSON field | Instruments name and interpretation |
| --- | --- |
| `GPU Done-to-Completion Walltime Stats` | GPU End to Display: last associated GPU work completes until the frame lands on display. |
| `End-to-end Walltime Stats (CPU)` | CPU Begin to Present: first command-buffer creation or drawable acquisition until client present. |
| `End-to-end Walltime Stats (GPU)` | GPU Begin to End: span from first associated GPU work through last GPU work. |
| `End-to-end Walltime Stats (Total)` | CPU Begin to Display: the same CPU beginning until display. |
| `Frame-On-Glass Interval Stats` | Frame Interval: time between displayed frames. |
| `On-Glass Present Lateness Stats` | On Display Lateness: arrival relative to an estimated or explicit deadline. |

Apple's [CAMetalLayer performance documentation](https://developer.apple.com/documentation/analytics-reports/cametallayer-performance)
corroborates the post-GPU metric. It is not defined as drawable retirement or
callback-delivery time. The documentation does not specify a first/last illuminated
pixel boundary, so it should not be treated as a physical scanout measurement.

[HUD Present Delay](https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics)
starts at the `presentDrawable` call and ends at display. Its starting event
differs from GPU End to Display; the two numbers must not be relabeled as equal.
Likewise, on-display lateness is relative to a deadline, whose estimation method
is not exposed here. Its sample count differs from the presented-frame count.

The per-frame metric and interval tables contain **zero rows**. Available
statistics are aggregated, usually over one second; a percentile of those
averages would not be a frame-latency percentile. The imported detailed
displayed-surface table is also empty. Apple distinguishes always-on aggregates
from optional per-frame recording in [WWDC26 session 388](https://developer.apple.com/videos/play/wwdc2026/388/).

## Consequence for the next experiment

Further shader compilation or GPU frame-limit tuning does not target the long
interval revealed here. The next useful evidence is a per-frame sequence of
presentation request, GPU completion, display deadline and actual display time.
That can distinguish frames submitted too far ahead from requests arriving after
a scheduling deadline. Current timer pacing limits rate without receiving actual
display deadlines; this is a candidate mechanism, not a demonstrated fault.

Any next pacing experiment should preserve three drawables, separate display-side
pacing from GPU completion, and compare both displayed-state age and cadence.
Do not restore the previous CAMetalDisplayLink implementation unchanged: that
experiment increased the reported HUD delay.

The unfinished custom recorder was set aside after the supplied capture became
available. No diagnostic hooks, pacing changes, installation or upload accompany
this initial analysis. At that time the application remained at the one-frame
experiment `469a7f9c`; the follow-up below examines the later candidate.

## Presentation-delay follow-up at `c71bcd76`

Re-examining the original capture adds a useful signal: recorded **On Display
Lateness** is concentrated around one 120 Hz refresh interval while the display
maintains 120 FPS. Using the same steady-rate selection and count-weighted means:

| PID | Intervals | Displayed frames | Lateness samples | Mean recorded lateness | Skipped frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| 5808 | 34 | 4,077 | 2,772 | 8.3425 ms | 0 |
| 5826 | 16 | 1,918 | 1,306 | 8.3431 ms | 0 |
| 5846 | 69 | 8,277 | 5,497 | 8.3381 ms | 0 |

The previously selected continuous 30-second segment has 3,251 lateness samples
averaging 8.3374 ms alongside 3,599 displayed frames. All selected intervals
attribute exactly two command buffers per displayed frame. The derived CSV now
includes lateness count, total, mean, bounds, command-buffer count and skipped
frames so these aggregates can be reproduced without the private trace.

This is evidence of a refresh-sized presentation-timing penalty relative to the
system's deadline, not proof of an application deadline miss. The trace does not
explain how that deadline was estimated or which frames contribute lateness
samples; dividing their count by displayed frames is not a validated miss-rate
measurement. It cannot distinguish a late CPU request, command scheduling, or
system presentation policy. These sessions also predate the current
CAMetalDisplayLink retry and are not measurements of `c71bcd76`.

### What the current source rules out

- iOS calls `bgfx::renderFrame()` before initialization, enabling synchronous
  submission on the application worker, and sets `BGFX_RESET_FLIP_AFTER_RENDER`.
  `Context::swap()` renders the freshly submitted frame synchronously and flips
  it in that call. There is no extra bgfx API/render-thread frame to remove here.
- Accepted display-link frames bypass the timer wait in
  `video::iosFramePacingWait`. The two-slot acquisition gate is cancelled for a
  managed display-link drawable. Neither adds a second pacing wait in that path.
- The Apple GPU limit remains three. Its semaphore is released on GPU command
  completion, not display; changing it does not directly bound the post-GPU wait.
- Metal commits rendering at the end of `submit()`, then `flip()` commits a
  separate presentation command buffer. This is an extra scheduling boundary,
  not evidence of a fixed extra refresh. Apple's own
  [display-link sample](https://developer.apple.com/documentation/metal/achieving-smooth-frame-rates-with-a-metal-display-link)
  uses command-buffer presentation; the API name alone is not a bug.

### The timing contract stops at callback receipt

`src/video/MetalDisplayLink.mm:43` passes `update.targetTimestamp` into
`MetalDisplayLinkFrame::receive`. It checks that the deadline has not already
passed, then discards it. `targetPresentationTimestamp` is not consumed.
Afterward, the application polls input, updates the scene, constructs draws and
submits bgfx work. The provider's `shouldPresent` checks ownership, geometry and
lifecycle, but not elapsed time or deadline margin. Its `scheduled` flag means
the provider allowed the request; it is not a GPU-scheduling or display event.

Apple defines [`targetTimestamp`](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp)
as the deadline for the drawable presentation call; GPU work may continue later.
[`MTLCommandBuffer.present`](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:))
defers the drawable's presentation call until the command buffer is scheduled.
Callback receipt, the CPU convenience-method call, command scheduling, GPU
completion and display are different boundaries. Checking only the first does
not establish whether the final request meets the intended slot.

The actionable architectural gap is that the loop receives display timing but
does not carry it through frame preparation and presentation. This is not yet
a demonstrated cause of the current device's delay.

### Next controlled experiment

Compare callback receipt, both display-link timestamps, input/scene sampling,
the presentation API request, presentation-buffer scheduled handler, GPU end,
and the drawable's `presentedTime` for the same frame. Use actual `presentedTime`,
not its callback's delivery time. Keep preferred latency one, three drawables,
GPU limit three, chart, charging and thermal conditions fixed.

- If preparation or scheduling crosses the submission deadline, reduce or move
  work there. Compare presenting on the final render command buffer against the
  separate buffer only if that boundary is implicated.
- If requests are on time and display matches the intended target, but rendering
  finishes much earlier, evaluate later input/scene preparation with a measured
  CPU/GPU budget and safety margin. Waiting only after sampling input can lower
  the HUD number without improving displayed-state age.
- If requests meet the deadline but display occurs after the target, investigate
  presentation/system scheduling rather than adding more GPU waits.

Do not restore the rejected wait-for-visible-presentation experiment, which
reduced device throughput to 60 FPS. Do not substitute `presentAtTime` on a
display-link drawable: Apple's
[`needsUpdate` contract](https://developer.apple.com/documentation/quartzcore/cametaldisplaylinkdelegate/metaldisplaylink(_:needsupdate:))
prohibits timed presentation alternatives in this mode.

A temporary native Mac probe attempted a one-buffer/two-buffer comparison with
real display-link callbacks. It produced only a few callbacks and no usable
`presentedTime` samples. It establishes no performance or buffer-count conclusion.
No production pacing changes, diagnostic hooks, installation or deployment
accompany this source/trace review.
