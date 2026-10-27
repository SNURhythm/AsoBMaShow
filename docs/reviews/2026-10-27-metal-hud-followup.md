# Metal HUD follow-up

The initial captured M5 iPad frame sustained 120 FPS with 3.32 ms GPU work and
16.36 ms Present Delay. A single frame does not establish a leak or an avoidable
queued frame. Later user comparisons are recorded separately below.

## Current changes

- Zero fullscreen blur samples the scene target directly, preserving final
  brightness/opacity composition. It skips two draws and releases two
  intermediate RGBA8 targets. Positive blur still uses both passes.
- Apple UI batches reuse the existing bounded upload cache for unchanged small
  vertex/index streams. Changed geometry/opacity and buffer growth still upload;
  large streams do not retain a second full CPU copy.
- Metal uses three CAMetalLayer drawables. The experimental buffering setting
  and custom reset flag are removed. iOS configures the UIKit-owned layer once
  on main; drawable acquisition remains on the worker.
- Offscreen BGA/blur views execute before the screen clear, acquiring the drawable
  later. Clear still precedes composition, gameplay and UI; readback stays last.
- Corrected iOS timer accounting is restored: frame work, including drawable
  acquisition and submission, consumes the frame budget before any remaining
  sleep. Explicit caps, idle pacing and export bypass remain covered by tests.
  Desktop and Android scheduling are unchanged.

CAMetalDisplayLink is completely removed, including its drawable provider,
worker-loop cleanup, export/lifecycle hooks and dedicated tests/build targets.
bgfx directly calls `nextDrawable` and `presentDrawable`. SDL's existing
refresh-rate CADisplayLink and bgfx GPU synchronization are unchanged. SDL's
animation callback is unset in this application, so that callback neither
renders nor presents.

All telemetry added by this branch is now removed: drawable-wait clocks,
presentation-timestamp callbacks, observer checks/registration, the two extra
latency stages, and process-memory/renderer-resource logging. The pre-existing
telemetry remains unchanged. The generated Metal patch only fixes the drawable
pool; it no longer wraps acquisition or presentation, even with telemetry off.
No submission limiter or new measurement is added.

## Direct zero-blur BGA path

Eligible prepared fullscreen BGA frames on Metal now draw directly into the
final view: zero/negative effective blur and one opaque base or miss surface,
with no layer. Images use conservative decode metadata (1 or 3 channels);
alpha-bearing/unknown images keep the old path. Videos use cached tinted
software-YUV or native-NV12 shaders, clamping conversion before brightness.
Missing resources, stale frames or unavailable transient capacity decline the
fast path without consuming the media lease. Same-frame fallback stays available.

The direct draw preserves display-mode geometry and viewport mapping, applies
brightness once, and clears letterbox bars to opaque black. It avoids the
intermediate write/read and final texture-composition draw. Removing the
half-resolution intermediate can make unblurred detail sharper. Its target
remains allocated for fallback/blur changes; no memory saving is claimed here.
Blurred/layered content, replay export and other renderers retain their existing
paths. No timer, display-link or telemetry change accompanies this optimization.

## Pause/resume observation

After the telemetry removal, the user reported about 16.15 ms Present Delay.
They then observed lower delay after pausing/resuming while blur was enabled:
initially around 10 ms, later about 13.7 ms persisting instead of returning to
16 ms. They explicitly withdrew the earlier suspected connection to input;
input is not an established trigger.

Source inspection finds no drawable-pool or frame-pacer reset in the ordinary
pause/resume handlers. Pause suspends the realtime gameplay worker and playback
clock. Built-in paused scenes without active video may also stop submitting
frames through PresentationSchedule; skin scenes and active video can keep
presenting. Resume restarts those workers/clocks. A change in queue occupancy or
refresh phase is plausible, but neither is proven by this report. No pause or
pacing behavior is changed. The zero-blur fast path does not address this blurred
scene observation.

## Device comparisons

The user reported the following HUD Present Delay values:

1. Before the branch: about 16.00 ms.
2. CAMetalDisplayLink enabled (`5804f12c`): about 23 ms.
3. Only display-link activation disabled (`e6329d76`): 16.66 ms.
4. Display-link integration completely removed (`620a1046`): about 16.5 ms.
5. Pre-branch timer accounting restored (`49e55e65`): still about 16.5 ms.

The last comparison did not show an observable change from timer accounting.
At the user's request, the latest candidate restores the corrected timer and
removes the branch-added telemetry. Blur bypass, upload reuse, view order and
three drawables remain unchanged. Compare this candidate with `620a1046` to
hold timer accounting constant. The user subsequently reported about 16.15 ms after telemetry removal.

The small residual difference from the pre-branch baseline has not been traced
to a cause. Scene/device conditions were not independently verified, and the
reported values do not establish a GPU-time or input-latency change. Compare
FPS and Present Delay under the same chart, skin, BGA, cap, refresh and thermal
conditions; local behavioral tests do not establish an on-device improvement.

## Verification

Telemetry-removal candidate: all 72 rendering/iOS platform checks, the desktop build and
unsigned iOS device build pass. The generated-Metal regression failed with the telemetry observer
present, then passed after removing observer checks, presentation callbacks and
clock reads. The pre-existing telemetry tests and frame-start timer regressions
are retained.

The frame-start regression covers 120 frames each blocked for one 120 Hz
interval in submission: the corrected timer consumes 120 refresh intervals,
whereas submission-end accounting consumes 240. Fast frames, budget overruns,
lower explicit caps and export are also covered. This explains the accounting
correction, but does not identify the cause of the reported HUD difference.

Before telemetry removal, the complete display-link removal passed all 72
rendering/iOS platform checks, all 54 iOS build-setup checks, and desktop and
unsigned iOS builds. The timer-accounting A/B also passed the existing frame-pacer
tests and both builds. Earlier display-link verification included 74 group
checks, native callback/provider smoke coverage and simulator compilation; those
bridge-specific checks were removed with that experiment. Historical plans and
specs are retained as implementation records, not current behavior.

## Direct-path verification

Focused native Metal and BGA tests pass, including one-draw output, brightness,
opaque bars, Fit/Fill/NoExpand, viewport offsets, blur/layer/alpha rejection,
stale/nonfinite inputs, transient-capacity recovery, and same-frame lease
ownership. Existing native NV12/software YUV fixtures verify tinted pixels.
The image test first failed before direct submission existed; the video test
failed without the new shaders, then passed with the compiled shaders.
All 87 rendering/video/iOS platform checks pass, along with the desktop and
unsigned iOS device builds. Final independent review is pending.
