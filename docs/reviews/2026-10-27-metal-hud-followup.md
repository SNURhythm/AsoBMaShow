# Metal HUD follow-up

The captured M5 iPad frame sustained 120 FPS with 3.32 ms GPU work and a
16.36 ms present delay. That single frame does not establish a leak or an
avoidable queued frame.

## Changes

- Zero fullscreen blur now samples the scene target directly, preserving the
  final brightness/opacity composition. It skips two draws and releases two
  intermediate RGBA8 targets. Positive blur still uses both passes.
- Apple UI batches use the existing bounded upload cache for unchanged small
  vertex/index streams. Dynamic geometry, changed opacity and buffer growth
  still upload; large streams do not retain a second full CPU copy.
- Metal always uses three CAMetalLayer drawables. The experimental buffering
  setting is removed; old saved values are ignored and omitted on the next
  save. iOS configures the UIKit-owned layer on main, while drawable acquisition
  remains on the worker.
- Telemetry builds observe `nextDrawable` wait and the interval from the
  presentation request to the drawable's `presentedTime`. Callback arrival
  time is not used as the display time. Invalid/unavailable timestamps are
  discarded. Telemetry also logs process resident bytes, renderer texture and
  render-target bytes, and transient upload usage every five seconds.

The display-refresh policy and worker-thread timer remain in use. A follow-up
fix changes the iOS timer's frame-budget accounting as described below; it does
not switch to display-link-driven rendering.
The fixed count is applied in the generated Metal source without changing the
upstream submodule. Ordinary resize and replay-export restoration retain it;
no custom reset flag or configurable buffering state is needed.

## Two-buffer pacing follow-up

A device report found 60 FPS with Triple buffering Off and Frame cap Unlimited.
Unlimited still uses the automatic iOS presentation limiter at the display's
refresh rate. The loop previously advanced that limiter after synchronous
`bgfx::frame()` returned. A drawable wait that already consumed a refresh could
therefore be followed by another software wait, even with inexpensive GPU work.

The iOS loop now budgets from the frame's start and sleeps only the remainder
after event handling, update, drawable acquisition and submission. This also
applies to explicit iOS caps; a lower selected cap still adds the required wait.
Idle frames retain their timer, export keeps its existing bypass, and desktop
and Android scheduling are unchanged. There is no explicit 60 FPS application cap in this path; drawable
availability can still limit system presentation throughput.

A deterministic regression failed under the old accounting and passes with the
fix: 120 frames each spending one 120 Hz interval in submission consume 120
refresh intervals, instead of 240. Tests also cover fast frames, a budget
overrun, a lower explicit cap and export. This establishes the pacing defect;
it does not establish that it is the only cause of the reported on-device drop.
The M5 iPad was unavailable for local physical measurement. The user's Off/On
comparison and the matching observation in [this discussion](https://stackoverflow.com/questions/51138426/ios-metal-double-buffering-at-120-fps)
support retaining three drawables for throughput. Two-drawable mode and its
setting have therefore been removed; the frame-budget correction remains.

## Further present-delay investigation

These are candidates for a separate measured experiment, not changes implemented
in this PR. Three drawables remain available throughout.

1. **Drive the render worker with CAMetalDisplayLink.** On iOS 17+, request
   `preferredFrameLatency = 1.0` and a sustainable frame-rate range, then render
   into the drawable supplied by the callback. Apple defines this as a requested
   render budget in frames, separately from the drawable pool size; actual system
   latency can be greater. Use `targetTimestamp` as the presentation-request
   deadline and `targetPresentationTimestamp` for display-time prediction.
   bgfx must consume the supplied drawable instead of independently calling
   `nextDrawable`. Keep UIKit event delivery and audio independent, handle
   pause/resize/export, and retain a fallback for iOS 15–16. Do not add the
   existing software frame sleep after a display-link callback. This is the
   strongest scheduling experiment, but neither 120 FPS nor a particular latency
   reduction is guaranteed. See Apple's [frame latency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency),
   [deadline](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp),
   and [delegate contract](https://developer.apple.com/documentation/quartzcore/cametaldisplaylinkdelegate/metaldisplaylink(_:needsupdate:)).

2. **Acquire the drawable later.** `RenderPlan::applyViewOrder` currently puts
   the backbuffer clear before BGA and blur passes. In the Metal backend that
   clear can acquire the drawable before offscreen encoding. Move offscreen
   views ahead of the clear while preserving clear-before-final/UI ordering,
   then verify gameplay, idle/loading screens and export. Apple's
   [drawable guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/Drawables.html)
   recommends late acquisition and prompt release. This can reduce drawable
   ownership time and acquisition stalls; it does not by itself prove a reduction
   in the HUD's present-delay metric.

3. **Measure queueing and refresh phase before limiting frames in flight.**
   The current timer controls frequency without synchronizing to the display's
   presentation deadline. Correlate frame start, input snapshot, drawable wait,
   command submission, GPU completion and actual presentation for each frame.
   A three-drawable pool does not require three queued presentations. bgfx's
   Metal command-queue semaphore uses `BGFX_CONFIG_MAX_FRAME_LATENCY`, separately
   from `resolution.maxFrameLatency`; changing the latter is not a CPU queue
   limiter. A GPU-completion fence alone also cannot detect a completed frame
   waiting for display. Only test a separate submission limit if measurements
   show excess queued work, and check throughput and tail latency together.

The earlier [display-link experiment](2026-10-10-ios-presentation-pacing.md)
regressed input delivery with rendering on UIKit main. It does not evaluate a
CAMetalDisplayLink callback on the later worker-thread architecture. Normal bgfx
presentation already registers `presentDrawable` before committing, promptly
releases its references and uses a per-frame autorelease pool. Adding a GPU
completion wait is therefore not an identified missing optimization. Timed
presentation is not a shortcut either: Apple's CAMetalDisplayLink delegate
contract explicitly disallows presentation methods targeting a specific time.

For the first experiment, compare the existing timer against worker-driven
CAMetalDisplayLink with a requested one-frame budget on the same M5 iPad at
120 Hz. Require sustained throughput, fewer missed presentation deadlines and
improved input-to-visible feedback, not just a smaller present-delay number
from submitting an otherwise identical frame later.

## Device comparison

Build with `ASOBMASHOW_ENABLE_PERF_TELEMETRY=1` (the CMake option is available on
desktop; the same preprocessor definition enables the iOS path). Compare builds
using the same chart, skin, BGA, refresh rate, frame cap and thermal state, with
the drawable pool fixed at three. Check present-request-to-display,
drawable-wait, frame submission and frame-time tails together. These cumulative stage histograms are not an
end-to-end input latency measurement and their percentiles must not be added.

For memory, record the same settled screen before and after repeated song
changes, including both static and video BGAs. Resident memory and bgfx resource
counters describe different quantities from Metal HUD footprint; do not add
them or treat a one-frame peak as a leak. Inspect blit transfers in a GPU
capture if copies remain expensive; transient geometry and software-decoded
video still legitimately need uploads.

No physical M5 iPad comparison was available during implementation. The local
GPU regressions establish output/resource behavior, not a measured on-device
latency or total-memory improvement.
