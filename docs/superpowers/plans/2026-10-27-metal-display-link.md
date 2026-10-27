# Metal Display-Link Implementation Plan

> Historical experiment: the CAMetalDisplayLink integration was subsequently
> removed at the user's request after the reported presentation-delay regression.
> See [the follow-up](../../reviews/2026-10-27-metal-hud-followup.md).

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add worker-driven iOS display-link pacing and defer fallback drawable acquisition without new measurement work.

**Architecture:** A scoped native bridge receives one drawable on the application worker's run loop and supplies it through a generated bgfx provider hook. Existing timers remain the compatibility fallback. Offscreen views precede the output clear.

**Tech Stack:** C++23, Objective-C++, CAMetalDisplayLink, generated bgfx patches, CMake/native Metal tests.

**Spec:** `docs/superpowers/specs/2026-10-27-metal-display-link-design.md` and the user's implementation request excluding measurement.

## Global Constraints

- Current checkout/branch; no worktree, merge or deployment. Commit and push verified work.
- Three drawables; iOS 15 minimum, new API guarded at iOS 17/macOS 14.
- Keep UIKit input and audio independent of the worker's pacing wait.
- No new telemetry or submission limiter. Preserve export and prepared-frame retirement.
- No whole-file formatting. Follow repository focused-test guidance.

## Review Focus

- Background/foreground pulses cannot reuse a stale drawable or strand the worker.
- Size/format resets between callback and encoding cannot present mismatched textures.
- Idle iterations, early returns and queued exports release every drawable lease.
- Very low/nondivisor caps and unavailable display-link APIs preserve timer behavior.
- Sustained late rendering retains forward progress without accumulating callback work.

### Task 1: Display-link bridge and pacing integration

Files: new `src/video/MetalDisplayLink{.h,.mm}`, `src/video/MetalDisplayLinkFrame.h`,
`src/video/MetalDrawableProvider.h`; existing Metal patch, main loop, iOS runtime,
frame-pacer tests, iOS setup tests, CMake source/test wiring.

Interfaces: `video::metalDisplayLinkRate(float, uint32_t)` selects a supported
rate or zero. `MetalDisplayLinkFrame` owns the waiting/ready/claimed lifecycle
without native resources. `MetalDisplayLink::beginFrame(void*, float, uint32_t,
uint64_t, std::function<bool()>)` waits for a valid callback and returns whether
this iteration is clocked; `endFrame`, `pause`, and `pauseCurrent` release or stop
that ownership. The optional bgfx provider reports handled separately from the
borrowed drawable, so a rejected managed frame cannot fall back to nextDrawable.

- [x] Extend frame-pacer coverage: 120/60/30 requests, odd/very low caps, invalid refresh, stale callback, duplicate claim, inactive/generation/geometry rejection, late accepted frame and clean next iteration. Build/run to observe missing behavior.
- [x] Implement the pure rate/lease policy and native worker run-loop bridge. Bound waits, back off after missing callbacks, keep at most one retained drawable, and guard API availability.
- [x] Add provider integration in generated bgfx source; exercise handled-nil versus fallback and borrowed drawable ownership with a native Metal fixture. Verify updated patch generation.
- [x] Integrate before input/scene update, scope cleanup across continue paths, suppress timer sleep only for clocked iterations, and pause before actual queued owner work.
- [x] Run focused policy/lifecycle/rendering checks and unsigned iOS compile; fix any failures before committing.

### Task 2: Offscreen-first view order

Files: `src/rendering/RenderPlan.cpp`, existing Metal BGA test and its target sources.

Interface: retain `applyViewOrder` and all view IDs; change only execution order.

- [x] Extend the real Metal BGA probe to assert offscreen passes precede the output clear, output clear precedes final/UI, and positive/zero blur pixel output remains correct. Confirm the old order fails.
- [x] Move BGA/layer/blur passes before clear; keep readback last and final/gameplay/UI ordering.
- [x] Run affected Metal rendering/export characterization checks and desktop app build. Commit the verified change.

### Task 3: Integration verification and review

- [x] Run relevant rendering/platform groups, full iOS setup tests and unsigned iOS build. Simulator launch checked; unlocked visual/background-resume smoke deferred by the user because the Mac is inaccessible.
- [x] Document final behavior, compatibility fallbacks and unavailable physical-device validation. Dispatch one fresh code review focused on the spec's ownership and interruption boundaries.
- [x] Resolve important review findings with focused regressions, update PR #161, and push completed work. Confirm clean task state and PR registration.
