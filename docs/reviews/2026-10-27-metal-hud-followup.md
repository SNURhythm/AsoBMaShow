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
- Display settings exposes Triple buffering on Metal. Off (default) requests
  two CAMetalLayer drawables; On requests three. The preference persists and
  participates in display preview, confirmation and rollback. iOS updates the
  UIKit-owned layer on main; drawable acquisition remains on the worker.
- Telemetry builds observe `nextDrawable` wait and the interval from the
  presentation request to the drawable's `presentedTime`. Callback arrival
  time is not used as the display time. Invalid/unavailable timestamps are
  discarded. Telemetry also logs process resident bytes, renderer texture and
  render-target bytes, and transient upload usage every five seconds.

The software pacing policy remains unchanged. Previous display-link experiments
had touch-latency regressions; compare on hardware before replacing that policy.
The drawable limit is carried in an app-owned bit in the pinned bgfx reset
flags, with a configuration-time collision check. This keeps ordinary resize,
display rollback and replay-export restoration consistent without modifying
the upstream submodule.

## Device comparison

Build with `ASOBMASHOW_ENABLE_PERF_TELEMETRY=1` (the CMake option is available on
desktop; the same preprocessor definition enables the iOS path). Compare the
same chart, skin, BGA, refresh rate, frame cap and thermal state with Triple
buffering Off and On. Check present-request-to-display, drawable-wait, frame
submission and frame-time tails together: a shorter queue is not a win if it
causes missed refreshes. These cumulative stage histograms are not an
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
