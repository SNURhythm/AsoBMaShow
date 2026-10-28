# Gameplay rendering follow-up audit — 2026-10-28

This investigation accompanies the six earlier findings in [the broader audit](2026-10-28-performance-audit.md). Those earlier findings were implemented separately. All three rendering findings below are implemented on the same branch; before/after component measurements are recorded with each finding.

## Scope and method

Traced `GamePlayScene::renderScene` through authority capture, built-in/custom-skin presentation, bounded projection, skin-state capture, Jukebox prepared surfaces, and the main-loop fullscreen post-process. Checked current caches and earlier rendering fixes before retaining a candidate. No private chart library was scanned.

Measurements use temporary synthetic probes linked to the existing production test objects on this Apple M1 Pro Mac. CPU objects use the native-test `-O1` configuration. GPU measurements use the actual `BlurPass`, shaders and fullscreen-tint path on headless Metal, with offscreen output. These are component costs, not a full-game profile, phone benchmark, FPS prediction or input-to-display latency result.

CPU probes ran without concurrent builds/tests; the retained baseline binaries were also rerun beside the new binaries to expose host timing variation. GPU values are medians of 300 samples after 120 warm-up frames, in alternating recompute/reuse runs. The first BGA table uses the original diagnostic reuse control; the second uses the implemented production cache path.

## 1. Recompute fullscreen BGA composition and blur only when its content changes

**Confirmed repeated GPU work; broadest candidate when fullscreen blur is enabled.**

At the audited baseline, `src/main.cpp`'s fullscreen branch submitted the prepared image/video surfaces and calls `s_postProcess.apply()` on every rendered gameplay frame when the zero-blur direct path does not apply. `BlurPass::execute()` runs both blur passes whenever strength is nonzero. No content-generation check guards these submissions. The default blur strength is 2.0.

Static images could therefore be composed and blurred at the gameplay presentation rate despite identical pixels. A lower-frame-rate video could also repeat this work between decoded frames. Existing video upload deduplication and the zero-blur direct path do not cache the positive-blur composite. Custom-skin embedded BGA destinations are a different path and are not covered by this claim.

A static 256×256 image, strength 2, downsample factor 2, and brightness 0.5 produced:

| Offscreen output | Recompute GPU median | Reuse final blurred texture | Avoidable component work |
| --- | ---: | ---: | ---: |
| 1280×720 | 161–165 µs | 51–57 µs | about 104–114 µs/frame |
| 2560×1440 | 258–260 µs | 100–102 µs | about 156–160 µs/frame |

API submission medians changed from about 12.4–12.8 µs to 4.5–4.7 µs. bgfx submission counts changed from 5 to 1 (including the layer `touch`). Full output readbacks matched byte-for-byte between recompute and reuse, and the probe asserted a nonzero rendered image.

**Implemented:** retained fullscreen output is keyed by stable adopted-image and accepted decoded-frame identities, Jukebox lifetime/seek epoch, composition roles, viewport/display mode and BlurPass target/settings generation. A reuse still validates and consumes its prepared lease. Brightness stays in the final draw. Composition, blur and final draws return completion status; failed, cancelled, export-boundary or interrupted presentation work cannot populate the cache. Before frame submission, `FrameSubmission` checks the current encoder's rejected draws and application-side allocation/preflight failures. Only a complete accepted frame can populate the cache; a later successful frame can recover after saturation. This replaced the original lifetime-peak guard in `c71bcd76` and does not wait for GPU completion or prove successful display. Unknown legacy content conservatively recomposes.

The final probe, including the draw-capacity publication guard, exercises the actual `Jukebox` prepared-frame path, `FullscreenBgaCache` and checked `BlurPass` methods. With the same output sizes/blur settings and an opaque 256×256 still image, alternating forced recomposition and enabled-cache runs produced:

| Offscreen output | Forced recomposition GPU | Production cache GPU | API submission CPU |
| --- | ---: | ---: | ---: |
| 1280×720 | 159–165 µs | about 49 µs | 13.3–13.5 → 4.9 µs |
| 2560×1440 | 256 µs | 99–102 µs | 13.3 → 4.9–5.0 µs |

Draw submissions fell from six to one; this probe includes both input-view `touch` submissions. Readbacks matched byte-for-byte at both resolutions, nonblack output was asserted, and every frame released its lease. This measures unchanged still-image reuse; moving video recomposes only when its accepted decoded content changes.

## 2. Stop the PMS `dst2` cursor walk at the relevant timeline boundary

**Confirmed full-suffix scan in a specific custom-skin path.**

Before this change, `src/scene/play/PlayfieldProjection.cpp::advancePmsPoorCursor` looped from its retained cursor to the end of `orderedTimelines`. Future timelines and timelines blocked by a retained earlier row used `continue`, so the loop visited the untouched future suffix each frame. The normal captured lane traversal is bounded; this optional PMS late-POOR descent path bypasses that advantage.

The real `PlayfieldProjection::project` probe used a captured traversal, six examined visible note descriptors, all future regular notes, and warm immutable indexes. Each value is the median of five 2,000-frame runs:

| Total timelines | Without `dst2` | With `dst2` |
| ---: | ---: | ---: |
| 100 | 0.80 µs/frame | 0.88 µs/frame |
| 1,000 | 0.82 µs/frame | 1.34 µs/frame |
| 10,000 | 0.79 µs/frame | 11.83 µs/frame |
| 50,000 | 1.04 µs/frame | 63.71 µs/frame |

The visible set stays fixed, so this is a clear scaling target. It is a small absolute cost on ordinary-sized charts and applies only when a skin supplies the secondary destination.

**Implemented:** stop at the first future timeline or retained barrier, preserving the previous-row cursor convention, long-head/tail retention, strict time boundaries, `showpastnote`, rewind/reset and descent ordering. The new fixtures assert one examined cursor row for an all-future chart and two for a retained barrier, at both 32 and 4,096 rows, plus release, equality, reset, replacement and catch-up behavior.

The post-change production probe measured 0.86/0.82/0.89/0.95 µs per `dst2` frame at 100/1,000/10,000/50,000 timelines, still with six visible descriptors. A paired rerun of the retained baseline binary measured 46.64 µs at 50,000 timelines versus 0.95 µs after the change; the original baseline above was 63.71 µs. Host timing varies, but both comparisons and the deterministic work counters show that future-chart-size scaling is removed.

## 3. Share immutable pacemaker replay progressions across presentation snapshots

**Confirmed chart-sized copying; lower priority in ordinary charts.**

Previously, `pacemaker::Target` owned a `scoreAfterNotes` vector. Each `GamePlayScene::capturePlayfieldVisualState` constructs authority with both target values, `PlayfieldVisualStateStore::applyAuthorityUpdate` copies them again, and `capture(..., false)` copies authority into the frame despite omitting note states. A custom skin adds `PlaySkinStateBridge::beginFrame`'s state copy. The already shared note snapshots do not cover these vectors.

A production-backed probe mirrored the scene target aggregate and called the actual store assignment and capture methods. Both BEST targets held valid-size replay progressions; other dynamic authority fields were empty to isolate this cost. The store was warmed so its existing vector capacity could be reused. Medians are five 10,000-frame runs:

| Notes per replay progression | Aggregate/store/capture | New allocations per frame | Allocated bytes per frame |
| ---: | ---: | ---: | ---: |
| 0, empty progression control | 0.20 µs | 0 | 0 |
| 1,000 | 0.68 µs | 4 | 16,016 |
| 5,000 | 3.13 µs | 4 | 80,016 |
| 10,000 | 9.74 µs | 4 | 160,016 |
| 50,000 | 25.40 µs | 4 | 800,016 |

These timings exclude the custom-skin bridge and most ordinary authority work. Only targets with replay progression pay the vector cost; rank-only/fallback targets do not. At 5,000 notes and 120 frames/s the measured four allocations represent about 9.6 MB/s of allocation traffic, but only a few microseconds per frame on this host.

**Implemented:** `pacemaker::ScoreProgression` shares immutable owned storage, with no mutable backing handle. Producers move a completed vector into it once. Snapshot copies share the storage; equality retains value semantics with a pointer fast path. Scalar fields, indexing and malformed/empty fallback behavior remain unchanged.

The paired production aggregate/store/capture probe measured 0.19 µs for the empty control and 0.23 µs for each nonempty size above, with **zero progression allocations and zero allocated bytes per frame**. The retained baseline binary measured 25.90 µs and 800,016 bytes per frame at 50,000 notes in that run. Tests exercise the actual store and both capture paths, retained frames after target replacement/reset, separate-but-equal values, indexing/fallback, and the skin bridge retaining its old progression after producer replacement. These isolate progression overhead; other gameplay and skin data can still allocate.

## Verification

- Independent task reviews and final whole-branch review are clean. The draw-saturation publication finding was corrected and independently re-reviewed before completion.
- Desktop `main` and all affected targets build; the unsigned iOS device build passes through `scripts/ios_firebase_deploy.sh --build-only --skip-init`.
- All 75 rendering/video checks, nine CPU-path checks, the additional Jukebox restoration check and the Noop BGA target check pass (85 distinct tests because the built-in renderer test is shared).
- Real Metal coverage includes unchanged/replaced stills, repeated/new software and native frames, seeks, layered/miss/blank transitions, live brightness, target/settings recreation, transient exhaustion at each stage, independent render-item saturation and next-frame recovery, discarded submissions, pixel equivalence and prepared-lease release.
- No deployment or physical-device measurement was performed. The measurements above establish reduced component work and allocation, not a demonstrated FPS or input-latency improvement.

## Other inspected paths

The normal custom-skin projection already uses captured bounded lane traversal; note snapshots, long-note lane aggregation and graph state already have reuse mechanisms. Earlier changes already addressed zero-blur fullscreen rendering, native-video upload reuse, transient-allocation behavior and selected HUD/skin work. They were not counted as new findings. No further general gameplay rendering optimization with stronger evidence was established in this pass.
