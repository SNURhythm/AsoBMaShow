# Frame-correlated Metal presentation timing experiment

This build measures the current iOS CAMetalDisplayLink path. It does not change
pacing, preferred frame latency (one), the drawable pool (three), or the GPU
frame limit (three). The purpose is to distinguish late preparation/scheduling
from an on-time frame that finishes early, or one displayed after its target.
It implements the [lookback analysis follow-up](2026-10-28-metal-lookback-analysis.md#next-controlled-experiment).

## Capture on a device

1. Use a display-compatible frame cap (for example 120 on a 120 Hz iPad), keeping
   the chart, charging and thermal conditions consistent with the comparison.
2. Play for 15–30 seconds while the unwanted delay is visible. Return to the main
   menu promptly, open **Parsing Logs**, and choose **Export Log**.
3. Save/share `AsoBMaShow-performance-log.txt`. The appended section begins
   `Metal presentation timing v1` and contains CSV rows.

Capture is a rolling in-memory window of the latest 8,192 accepted display-link
updates: about 68 seconds at 120 callbacks/second. Menu/idle frames also occupy
the window, so delaying the export can overwrite the gameplay interval. A restart
clears it. There is no per-frame text logging or file writing. Snapshot copying
and CSV formatting happen only on explicit export; that export frame can itself
be slow and should be excluded from analysis.

Capture requires the active iOS 17+ display-link path. Timer fallback, unsupported
frame caps, background operation and video export do not produce comparable
rows. An empty section is not a successful measurement of those paths.

## Columns and clock

All timestamps are seconds in the Core Animation host-time domain. GPU times
come from the completed render command buffer; display time is the drawable's
`presentedTime`, not when its callback happens to execute. Zero is a missing
stage, never a measured zero duration. Frame identifiers increase for the process;
`generation` is the iOS presentation/lifecycle generation.

| Column | Boundary |
| --- | --- |
| `gameplay` | Gameplay-active flag at preparation start; transitions can straddle it. |
| `callback` | Accepted display-link callback receipt, before returning to the frame loop. |
| `deadline` | Display-link `targetTimestamp`, the drawable presentation-call deadline. |
| `target` | Display-link `targetPresentationTimestamp`, the intended display time. |
| `preparation_start` | Frame-loop boundary before draining input and updating the scene. |
| `preparation_end` | Immediately before `bgfx::frame`; bgfx rendering/encoding follows. |
| `render_submit` | Just before the final render command buffer is committed. |
| `gpu_start`, `gpu_end` | GPU interval for that render command buffer, read after completion. |
| `present_request` | Before the command-buffer `presentDrawable` convenience-method call. |
| `present_scheduled_handler` | Host time observed by that buffer's scheduled handler. |
| `displayed` | Actual `MTLDrawable.presentedTime`. |
| `frame_end` | Application frame-scope release, not GPU or display completion. |

The scheduled handler is an observation of scheduling, not the exact internal
`MTLDrawable.present` call. A late handler alone does not prove a missed deadline.
The render GPU interval is not a sum of every command buffer/resource upload in
the frame. Preparation start is an application boundary, not an input-device
arrival timestamp; display minus preparation start is not physical input-to-photon
latency.

## Interpreting a capture

Filter to the steady gameplay interval and require nonzero stages used in each
comparison. In particular, do not interpret idle, rejected, discarded, or
in-flight rows as displayed frames. Compare:

- `present_request - deadline`: a positive value means the CPU presentation
  request was already late. Preparation/encoding timestamps locate earlier cost.
- `present_scheduled_handler - deadline`: a scheduling clue, with the callback
  observation limitation above.
- `displayed - target`: whether display met the intended slot.
- `displayed - gpu_end` and `displayed - preparation_start`: post-GPU waiting and
  age since the application's preparation boundary.
- Consecutive `displayed` differences: cadence, alongside the target difference.

Near-target display with a large post-GPU gap motivates an experiment that moves
input/scene preparation later. A late CPU request motivates reducing or moving
preparation/encoding work. On-time requests with late display motivate studying
command/system presentation scheduling. Do not treat a lower HUD Present Delay
alone as proof of more responsive input.

## Loss, lifetime and verification limits

Each slot uses a nonblocking try-lock. Callback or frame updates that contend
are dropped and counted as `dropped_updates`; export may omit busy slots. The
counter is cumulative for the process. A snapshot is best effort, not one global
instant. Missing stages and rollover must be reported when analyzing the data.
Monotonic frame tokens prevent late callbacks from writing into reused rows.
The fixed storage survives display-link/renderer teardown; callbacks retain only
the token, not application, scene or drawable ownership.

Native tests exercise correlation, rollover, concurrent updates/snapshots, loss
accounting, rejected presentation, and real Metal command-buffer scheduling/GPU
completion. Headless native tests do not prove native display-link callback or
on-screen `presentedTime` delivery. A device export is still required before
claiming the cause of the current delay or any latency improvement.

## Local verification

- Desktop `main` and unsigned iOS device builds passed.
- All 62 `area_rendering` checks passed. The four timing/provider/native-hook
  tests passed again after making snapshot/counter evaluation order explicit.
- iOS setup checks passed for 54 unchanged cases and the repaired fixture's
  focused rerun. The fixture now includes the existing bgfx frame-submission
  source, validates its wrapper, and checks both new timing-hook calls.
- Independent review found no critical/important issue. Its minor counter-read
  ordering observation was corrected. No device install or deployment was run.
