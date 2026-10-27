# Gameplay rendering follow-up audit — 2026-10-28

This investigation accompanies the six earlier findings in [the broader audit](2026-10-28-performance-audit.md). Those earlier findings were implemented separately. The three rendering candidates below are **investigation findings, not implemented changes**.

## Scope and method

Traced `GamePlayScene::renderScene` through authority capture, built-in/custom-skin presentation, bounded projection, skin-state capture, Jukebox prepared surfaces, and the main-loop fullscreen post-process. Checked current caches and earlier rendering fixes before retaining a candidate. No private chart library was scanned.

Measurements use temporary synthetic probes linked to the existing production test objects on this Apple M1 Pro Mac. CPU objects use the native-test `-O1` configuration. GPU measurements use the actual `BlurPass`, shaders and fullscreen-tint path on headless Metal, with offscreen output. These are component costs, not a full-game profile, phone benchmark, FPS prediction or input-to-display latency result.

The CPU probes were rerun after builds and the test suites finished. GPU values are medians of 300 samples after 120 warm-up frames, in alternating recompute/reuse runs. The reuse control is a diagnostic experiment, not a proposed complete cache implementation.

## 1. Recompute fullscreen BGA composition and blur only when its content changes

**Confirmed repeated GPU work; broadest candidate when fullscreen blur is enabled.**

`src/main.cpp`'s fullscreen branch submits the prepared image/video surfaces and calls `s_postProcess.apply()` on every rendered gameplay frame when the zero-blur direct path does not apply. `BlurPass::execute()` runs both blur passes whenever strength is nonzero. No content-generation check guards these submissions. The default blur strength is 2.0.

Static images can therefore be composed and blurred at the gameplay presentation rate despite identical pixels. A lower-frame-rate video can also repeat this work between decoded frames. Existing video upload deduplication and the zero-blur direct path do not cache the positive-blur composite. Custom-skin embedded BGA destinations are a different path and are not covered by this claim.

A static 256×256 image, strength 2, downsample factor 2, and brightness 0.5 produced:

| Offscreen output | Recompute GPU median | Reuse final blurred texture | Avoidable component work |
| --- | ---: | ---: | ---: |
| 1280×720 | 161–165 µs | 51–57 µs | about 104–114 µs/frame |
| 2560×1440 | 258–260 µs | 100–102 µs | about 156–160 µs/frame |

API submission medians changed from about 12.4–12.8 µs to 4.5–4.7 µs. bgfx submission counts changed from 5 to 1 (including the layer `touch`). Full output readbacks matched byte-for-byte between recompute and reuse, and the probe asserted a nonzero rendered image.

**Potential implementation:** retain the composite/blur output across unchanged content and redraw only the final fullscreen composite. Key invalidation to actual image/decoded-frame content, base/layer/miss composition, viewport/target recreation, display mode, blur strength, and renderer lifetime. Preserve prepared-frame lease consumption even when composition is reused. Brightness is applied in the final draw and can remain live. A prepared frame serial or texture handle alone is not a sufficient content identity for mutable video textures. Resource replacement, seeks, blank/miss transitions, device resets and interrupted frames need explicit tests.

## 2. Stop the PMS `dst2` cursor walk at the relevant timeline boundary

**Confirmed full-suffix scan in a specific custom-skin path.**

`src/scene/play/PlayfieldProjection.cpp::advancePmsPoorCursor` loops from its retained cursor to the end of `orderedTimelines`. Future timelines and timelines blocked by a retained earlier row use `continue`, so the loop visits the untouched future suffix each frame. The normal captured lane traversal is bounded; this optional PMS late-POOR descent path bypasses that advantage.

The real `PlayfieldProjection::project` probe used a captured traversal, six examined visible note descriptors, all future regular notes, and warm immutable indexes. Each value is the median of five 2,000-frame runs:

| Total timelines | Without `dst2` | With `dst2` |
| ---: | ---: | ---: |
| 100 | 0.80 µs/frame | 0.88 µs/frame |
| 1,000 | 0.82 µs/frame | 1.34 µs/frame |
| 10,000 | 0.79 µs/frame | 11.83 µs/frame |
| 50,000 | 1.04 µs/frame | 63.71 µs/frame |

The visible set stays fixed, so this is a clear scaling target. It is a small absolute cost on ordinary-sized charts and applies only when a skin supplies the secondary destination.

**Potential implementation:** stop at the first future timeline or first retained barrier, preserving the source's previous-row cursor convention, long-head/tail retention, exact timing boundaries, `showpastnote`, rewind/reset behavior and descent ordering. Existing PMS projection fixtures provide the behavioral baseline; add work-count coverage to ensure a fixed visible set does not visit the future suffix.

## 3. Share immutable pacemaker replay progressions across presentation snapshots

**Confirmed chart-sized copying; lower priority in ordinary charts.**

`pacemaker::Target` owns `scoreAfterNotes`. Each `GamePlayScene::capturePlayfieldVisualState` constructs authority with both target values, `PlayfieldVisualStateStore::applyAuthorityUpdate` copies them again, and `capture(..., false)` copies authority into the frame despite omitting note states. A custom skin adds `PlaySkinStateBridge::beginFrame`'s state copy. The already shared note snapshots do not cover these vectors.

A production-backed probe mirrored the scene target aggregate and called the actual store assignment and capture methods. Both BEST targets held valid-size replay progressions; other dynamic authority fields were empty to isolate this cost. The store was warmed so its existing vector capacity could be reused. Medians are five 10,000-frame runs:

| Notes per replay progression | Aggregate/store/capture | New allocations per frame | Allocated bytes per frame |
| ---: | ---: | ---: | ---: |
| 0, empty progression control | 0.20 µs | 0 | 0 |
| 1,000 | 0.68 µs | 4 | 16,016 |
| 5,000 | 3.13 µs | 4 | 80,016 |
| 10,000 | 9.74 µs | 4 | 160,016 |
| 50,000 | 25.40 µs | 4 | 800,016 |

These timings exclude the custom-skin bridge and most ordinary authority work. Only targets with replay progression pay the vector cost; rank-only/fallback targets do not. At 5,000 notes and 120 frames/s the measured four allocations represent about 9.6 MB/s of allocation traffic, but only a few microseconds per frame on this host.

**Potential implementation:** shared immutable progression storage or chart-lifetime target snapshots, keeping scalar current-score state separate. Preserve captured-frame immutability, replay/course transitions, target replacement and indexing semantics. Optimize this for allocation reduction without describing it as a demonstrated frame-time bottleneck.

## Other inspected paths

The normal custom-skin projection already uses captured bounded lane traversal; note snapshots, long-note lane aggregation and graph state already have reuse mechanisms. Earlier changes already addressed zero-blur fullscreen rendering, native-video upload reuse, transient-allocation behavior and selected HUD/skin work. They were not counted as new findings. No further general gameplay rendering optimization with stronger evidence was established in this pass.
