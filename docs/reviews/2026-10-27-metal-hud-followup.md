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

iOS uses the corrected timer pacing. The CAMetalDisplayLink experiment has
been removed completely, including its drawable provider and lifecycle/export
hooks. bgfx calls `nextDrawable` and `presentDrawable` directly, with the existing
optional presentation observer retained. UIKit event delivery and audio remain
independent of the presentation wait.
The fixed count is applied in the generated Metal source without changing the
upstream submodule. The iOS pool is set once at layer initialization; ordinary
resize and replay-export restoration retain it. No custom reset flag or
configurable buffering state is needed.

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

## Display-link experiment and removal

The worker-owned CAMetalDisplayLink experiment supplied one drawable to bgfx
and suppressed the post-submit timer wait. The user reported about 23 ms HUD
Present Delay with that path enabled (`5804f12c`). Disabling only its application
call site (`e6329d76`) reduced the delay to 16.66 ms; the user clarified that the
pre-branch baseline was about 16.00 ms. Thus the first A/B recovered most of the
increase but left a reported difference of about 0.66 ms.

The latest candidate removes the integration entirely: native bridge and frame
policy, provider callbacks in bgfx, worker-loop setup/cleanup, export/lifecycle
hooks, and the dedicated tests/build targets. The corrected timer accounting,
offscreen-first view order, zero-blur bypass, Apple UI upload reuse, fixed
three-drawable pool and optional telemetry remain unchanged. The simulator
presentation-timestamp availability guard is retained.

Source inspection found SDL already registers a CADisplayLink to request high
refresh rates. Its animation callback is unset here, so it neither renders nor
presents. SDL's refresh callback and bgfx's existing GPU synchronization remain
unchanged. Offscreen BGA/blur views still execute before the backbuffer clear
to acquire drawables later.

The physical result of complete removal is pending. The exact scheduling cause,
GPU-time change and input-latency effect of the reported differences have not
been established; scene and device conditions were not independently verified.
No new telemetry or submission limiter is added.

## Removal verification

All 72 rendering/iOS platform checks pass, along with the desktop build and
unsigned iOS device build. All 54 iOS build-setup checks also pass.
The generated-Metal regression first failed because acquisition still used the
provider, then passed after restoring direct `nextDrawable` acquisition and
removing the provider. Display-link-specific tests were removed with the code;
existing timer-accounting and real Metal BGA/view-order coverage remain.

## Earlier verification (before removal)

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

Decisions recorded during the earlier display-link review:
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
