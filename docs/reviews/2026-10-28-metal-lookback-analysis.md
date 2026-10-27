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
this analysis. The application remains at the one-frame experiment `469a7f9c`.
