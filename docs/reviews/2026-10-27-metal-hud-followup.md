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

iOS currently uses the corrected timer pacing for the pacing-only A/B build
described below. The iOS 17+ worker-owned CAMetalDisplayLink implementation is
retained but disabled at the application call site. UIKit event delivery and
audio remain independent of the presentation wait.
The fixed count is applied in the generated Metal source without changing the
upstream submodule. Ordinary resize and replay-export restoration retain it;
no custom reset flag or configurable buffering state is needed. The iOS pool
is set at layer initialization, because CAMetalDisplayLink forbids assigning
maximumDrawableCount after attachment, even to the same value.

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

## Display-link and acquisition follow-up

The user approved the two implementation candidates and deferred measurement.
Three drawables remain available throughout; no submission limiter or new
telemetry is added.

On iOS 17+, `MetalDisplayLink` waits on the existing application/render worker's
run loop before the scene loop drains input and updates. The callback retains
one supplied drawable. bgfx borrows that drawable instead of calling
`nextDrawable` again, and schedules its normal untimed presentation before
committing. The requested `preferredFrameLatency` is 1.0; this is a rendering
budget, not a guaranteed physical latency. See Apple's [frame latency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency)
and [delegate contract](https://developer.apple.com/documentation/quartzcore/cametaldisplaylinkdelegate/metaldisplaylink(_:needsupdate:)).

Unlimited and caps at/above display refresh request the display rate. Exact
divisors at 30 Hz or above can request a lower rate; very low/nondivisor caps
retain timer pacing. iOS 15–16 and a missing native callback also use the timer.
Callback waits check lifecycle/quit in short slices, time out after at most
three requested refresh intervals (at least 50 ms), and back off for one second
after a missing callback. A successful display-link wake suppresses the old
post-submit timer sleep, including idle service iterations.

The bridge rejects stale callbacks, inactive/lifecycle-invalid leases and
mismatched drawable geometry/format. It checks validity again before scheduling
presentation. Rejected managed frames cannot acquire a replacement drawable;
the scene remains dirty for a fresh frame. Scope cleanup releases unused or
submitted leases on every loop exit. The link pauses before queued owner work,
including export, and at every export reservation (covering inline photo export).
Pausing invalidates the native link: a merely paused CAMetalDisplayLink still
forbids nextDrawable. The next supported scene iteration recreates it.
It is invalidated before renderer shutdown. Frames accepted
on time may still present after an encoding overrun, preserving forward progress
under load. Gameplay/audio clocks are unchanged.

`RenderPlan::applyViewOrder` now encodes BGA base/layer and both blur passes
before the output clear. Clear still precedes final composition, gameplay and
UI; readback remains last. This delays drawable acquisition on the fallback
path, following Apple's [drawable guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/Drawables.html).
Display-link frames receive their drawable from the callback, so this ordering
does not defer the OS's delivery of that drawable.

The earlier [display-link experiment](2026-10-10-ios-presentation-pacing.md)
regressed input delivery while rendering on UIKit main. The new implementation
keeps rendering on the later worker-thread architecture. Existing presentation
telemetry is unchanged except that simulator builds omit the unsupported
`MTLDrawable` presented-timestamp callback.

Queue-depth/refresh-phase correlation and physical latency comparisons are
deferred as requested. A pool of three drawables does not require three queued
presentations, and a GPU-completion fence alone cannot detect a completed frame
waiting for display. Any future submission limit should follow device evidence.

## Pacing-only A/B follow-up

The user reported visible output with HUD Present Delay increasing from about
16 ms to 23 ms after enabling the display-link path. GPU time for that comparison
is unknown, so neither extra GPU work nor an exact input-latency increase has
been established.

The next candidate disables only application display-link pacing through
`useMetalDisplayLinkPacing = false` in `src/main.cpp`. No native display link is
created, bgfx acquires its drawable normally, and the corrected timer accounts
for all frame work before any remaining sleep. Three drawables, offscreen-first
view order, zero-blur bypass and UI upload reuse remain unchanged.

Source inspection found SDL already registers a CADisplayLink to request high
refresh rates. Its animation callback is unset here, so it neither renders nor
presents. bgfx still owns GPU completion synchronization and presentation; the
new provider replaces drawable acquisition rather than duplicating it. SDL's
refresh callback and bgfx's synchronization are unchanged for this comparison.

Compare this candidate with `5804f12c` using the same chart/scene, skin, BGA,
refresh rate, cap and thermal conditions. Record HUD Present Delay, FPS and GPU
time. A return near 16 ms would implicate the display-link path without yet
identifying its exact scheduling cause. No new telemetry is required.
The timer candidate passes the four focused pacing/provider/export/BGA checks
and an unsigned iOS device build. The on-device A/B result is still pending.

## Local verification

Rendering/iOS platform groups pass all 74 checks, including native Metal
provider handoff, timer/export reacquisition after display-link invalidation,
frame-cap/lease policy, and inline result-export ownership. The BGA test checks
actual view execution order and zero/positive blur pixels; the partial image
export test also passes. The 54 iOS build-setup checks pass, with the revised
initialization-only drawable-pool check rerun after the simulator exposed the
layer setter restriction. Desktop, unsigned iOS device and simulator builds pass.
A worker-thread native CAMetalDisplayLink smoke submitted three successive
frames. Simulator launch reached the Metal worker and display-link handoff,
but screenshots were black on both display-link and timer paths while the host
Mac was locked. The user deferred the unlocked visual/background-resume smoke
check and requested pushing the source-level fixes. Physical device latency
measurement remains deferred. Independent read-only review of
`8302bbe6..42f0fcef` found no critical, important or minor source defects.

Implementation decisions retained during review:
- No new telemetry or submission limiter: measurement was excluded, so no
  measured latency/queue-depth improvement is claimed.
- Accepted late frames may present to preserve forward progress; they can miss
  their intended refresh.
- Very low/nondivisor caps use timer pacing; they do not gain display-link
  scheduling.
- One combined unsigned iOS build verified the bridge and view ordering after
  implementation; this delayed platform feedback rather than duplicating builds.
- Two existing Android test fixtures gained missing boundary stubs so the
  affected group could compile. Production Android behavior is unchanged;
  those stubs do not validate the unrelated reminder/notification features.

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
