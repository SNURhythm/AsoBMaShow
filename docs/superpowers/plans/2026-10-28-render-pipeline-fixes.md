# Render pipeline fixes implementation plan

> **For agentic workers:** Use superpowers:executing-plans or superpowers:subagent-driven-development to implement each task. The user approved the audited fixes on 2026-10-28.

**Goal:** Fix the four findings in the render pipeline audit, preserving performance and reporting measured benefits separately.

**Architecture:** Keep existing bgfx rendering and pass order. Add checked export completion and current-frame draw acceptance, clamp screen-space textures, and remove redundant output clears while retaining the initial attachment clear and direct-BGA letterboxing.

**Tech Stack:** C++20, bgfx, CMake/Xcode, native Noop and Metal readback tests.

- [x] Export/common helpers: regress short transient allocations, add checked allocation and shared fullscreen export completion, reject failed frames before readback/encoding. Own `common.*`, exporter, focused export tests.
- [x] Blur: extend existing Metal BGA tests with horizontal/vertical edge oracles, confirm failure, clamp intermediate textures and propagate geometry failures through checked blur methods.
- [x] Frame acceptance: expose current CPU-side bgfx drop status through a wrapper compiled instead of upstream `bgfx.cpp`, preserving the submodule. Wire desktop/Android/iOS. Verify saturation, recovery and frame reset with real bgfx.
- [x] UI: latch upload failures for each frame, combine with current draw acceptance before acknowledging presentation. Extend native batch tests and the Metal saturation/recovery test; ensure an accepted recovery returns to idle.
- [x] Output clears: remove later color/depth clears for ordinary composition, preserve initial clear and direct-BGA opaque letterbox. Verify pixels and rerun the component timing probe.
- [x] Integrate: serialize builds in each build directory, run affected rendering/video/UI/export checks, desktop main and unsigned iOS builds; review and update the audit. Commit and push with this plan. No deployment.

Final verification: 80 distinct affected tests, desktop main and unsigned iOS builds pass. Independent review's legacy batch failure gap was reproduced and fixed using scoped frame failure accounting; follow-up review is clean. Final Metal clear comparison preserves pixels and saves 5.75–7.29 µs at 720p and 16.13–17.54 µs at 1440p. Details and limits are in `docs/reviews/2026-10-28-render-pipeline-audit.md`.
