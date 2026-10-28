# Metal Presentation Timing Experiment

> **For agentic workers:** Use superpowers:executing-plans for this cohesive measurement experiment.

**Goal:** Distinguish late preparation/scheduling from early rendering and late display without changing pacing.

**Architecture:** Retain a bounded rolling set of frame-correlated timestamps in memory. Display-link and Metal callbacks carry only monotonically identified frame tokens; stale callbacks cannot update reused slots. Append the snapshot to the existing Parsing Logs → Export Log action. No per-frame text logging, disk writes, GPU waits, drawable retention, or new settings.

**Tech Stack:** C++17 bounded storage, Objective-C++ Metal callbacks, generated bgfx patch, existing native CMake tests and unsigned iOS build.

**Spec:** User approved the next controlled experiment in `docs/reviews/2026-10-28-metal-lookback-analysis.md` with “let's try”. Measurement precedes choosing a pacing change. Three drawables, GPU limit three, preferred latency one and existing fallback/lifecycle behavior remain unchanged.

## Scope and decisions

Capture the latest 8,192 accepted display-link callbacks (about 68 seconds at 120 Hz), including partial/idle frames. Use CACurrentMediaTime's clock, Metal GPU timestamps and drawable presentedTime; do not treat callback delivery time as display time. Record callback/deadline/target, application preparation start/end, render-submit, GPU start/end, presentation-request/scheduled-handler, and frame-scope end. Gameplay-active tags distinguish menus; zeros identify missing stages. Snapshot/export is explicit; there is no background writer. Capture is enabled for this experimental iOS display-link path only. Native tests may seed records on Mac.

Use nonblocking per-slot try-locks, with dropped updates counted in the export. Export can omit a busy slot; never make the render or callback thread wait for export. Storage lives for the process so callbacks arriving after link teardown remain safe. No chart paths or input contents are recorded.

## Steps

- [x] Add behavioral coverage for correlated events, rollover/stale callbacks, missing timestamps, concurrent updates/export, and exported clock/column semantics. Implement bounded `MetalPresentationTiming` storage and text export.
- [x] Add optional token callback to `MetalDrawableProvider`; record native render completion and presentation scheduling in the generated backend. Extend real Metal provider coverage to verify same-frame GPU/request/scheduled records, keeping native presentation optional in headless tests.
- [x] Retain callback/target times and register drawable presentedTime capture in `MetalDisplayLink`; mark application preparation around input/update/draw construction. Append records to the existing performance-log export.
- [x] Run focused native rendering/iOS integration tests, desktop main and unsigned iOS builds. Independently review, document usage and limitations, commit/push current branch. No deployment.

## Review focus

Stale callbacks after rollover/teardown; export contention; no false complete data for nil/idle/rejected drawables; clock-domain compatibility; no callback-driven pacing or retained app/scene/drawable pointers. Validate records on a device before claiming a latency cause or benefit.

Validation and usage: [experiment report](../../reviews/2026-10-28-metal-presentation-timing-experiment.md). Final commit/push completes this plan. Device timing conclusions remain pending a capture.
