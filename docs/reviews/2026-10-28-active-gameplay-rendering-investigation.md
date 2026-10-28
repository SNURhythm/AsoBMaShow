# Active gameplay rendering investigation — 2026-10-28

At `5091b83a`, this pass found **no new demonstrated bottleneck in normal realtime manual gameplay**. It found a measurable chart-sized CPU cost in custom-skin replay/legacy-practice capture. Two plausible normal-rendering reductions measured too small or inconsistent to prioritize for frame-time improvement. This is an investigation, with no production rendering changes.

The existing [capture 25 analysis](2026-10-28-metal-log25-analysis.md) remains the device evidence: displayed gameplay CPU preparation-to-request median/P99 was 0.782/2.057 ms, and GPU duration was 1.188/1.359 ms. Those elapsed CPU intervals include possible waits. They do not establish a saturated GPU or identify a busy CPU function. Typical rendering fits comfortably inside an 8.333 ms interval; this does not rule out intermittent stalls or prove input latency.

## Scope and evidence

Traced active `GamePlayScene::update`/`renderScene`, visual-state capture, custom-skin planning/submission, touch-hit publication, BGA/video preparation, and fullscreen blur. The earlier [rendering audit](2026-10-28-gameplay-rendering-audit.md) and [optimization value audit](2026-10-28-optimization-value-audit.md) identify already implemented work; those fixes are not counted again.

Measurements are bounded synthetic component probes on an Apple M1 Pro, using existing production test objects compiled at `-O1`. CPU probes use five batches of 2,000 frames following 120 warm-ups. Builds and timed probes ran serially. No private chart library was scanned. GPU probes use real Jukebox/BlurPass/shaders on headless Metal with offscreen output. They do not reproduce the phone, real skin complexity, input activity, OS scheduling, or the complete application frame.

The [machine-readable results](data/2026-10-28-active-rendering-probes.json) include all paired submission runs and GPU conditions. Temporary probes used the existing `playfield_visual_state_tests`, `skin_quad_batch_renderer_tests`, and `gameplay_bga_target_tests` harnesses. Allocation bytes mean requested C++ allocation capacity, not resident memory growth or bytes actually copied.

## 1. Custom-skin replay capture scans every note, including unchanged future notes

**Confirmed scaling cost, limited to the non-realtime presentation path.**

`GamePlayScene::renderScene` calls `capturePlayfieldVisualState`. When a selected skin is active and realtime authority is inactive, the scene allocates a vector for every visual note source, walks the entire chart, performs RTTI/long-note state derivation, then calls `PlayfieldVisualStateStore::setNoteStates`. The store walks the descriptors again and looks up each ID before rejecting unchanged states. Bounded projection happens downstream and cannot remove this scan.

`RealtimeGameplayAuthorityPolicy.cpp` explicitly excludes replay playback and legacy practice. Normal realtime manual play does not enter this branch. Session-backed practice can use realtime authority and must not be grouped with legacy practice.

The probe mirrors the scene descriptor loop over real parser note objects and calls the production store and `captureForPresentation`. All notes remain future, none change, and the prior captured note snapshot is released before each iteration as in the scene. The mixed fixture uses 20% HCN endpoints in paired heads/tails; other notes are ordinary. Visible and changed-note counts stay at zero while total chart size increases. These are lower-bound unchanged-state costs, not representative full replay frame times.

| Total visual note sources | Normal: descriptor / store / total | 20% HCN: descriptor / store / total | Allocated bytes/frame |
| ---: | ---: | ---: | ---: |
| 1,000 | 14.25 / 4.67 / 19.29 µs | 16.27 / 4.71 / 21.36 µs | 24,832 |
| 5,000 | 70.57 / 23.26 / 94.21 µs | 80.55 / 23.34 / 104.36 µs | 120,832 |
| 10,000 | 141.58 / 46.35 / 188.34 µs | 161.50 / 46.54 / 208.57 µs | 240,832 |
| 50,000 | 715.93 / 238.86 / 955.12 µs | 864.35 / 239.84 / 1,105.07 µs | 1,200,832 |

Each measured frame makes two ordinary allocations: the chart-sized descriptor vector (24 bytes/source) and invariant capture storage. Total includes capture and timing overhead; the descriptor/store columns are independently calculated medians and need not sum exactly to total. This does not measure copy-on-write costs on changed frames or snapshots retained elsewhere.

**Priority:** strongest measured remaining candidate if replay performance is in scope. Incremental updates or a bounded active-note update path would address the actual scaling. Merely retaining the descriptor vector removes allocation but still scans every note twice. Any implementation must preserve replay seeking/rewinding, long-note head/tail changes, HCN lane reactivity, misses/deaths, and snapshot ownership. No replacement was implemented or benchmarked here, so the table is current cost, not promised savings.

## 2. Outer custom-skin submission allocates four planning vectors each frame

**Confirmed avoidable allocation, weak frame-time priority.**

The gameplay overload in `Skin2DRendererSubmit.cpp` declares local `quadSegments`, `movieCommands`, `bgaTargets`, and `steps` vectors and reserves against total command count. This happens even with no movie/BGA markers. `SkinQuadBatchRenderer`'s retained scratch sits underneath this planner and does not cover these allocations.

The probe calls the actual gameplay submission overload with a warmed catalog, counting fake batch/BGA backends, and either no BGA marker or two embedded markers. Ordinary quads share a texture/state. A diagnostic copy retains and clears just the four outer vectors; it is deliberately not a production design or lifecycle proof.

| Ordinary quads, no BGA markers | Current allocations / requested bytes | Current submission CPU | Retained-control CPU |
| ---: | ---: | ---: | ---: |
| 64 | 4 / 31,760 | 2.71–2.75 µs | 2.62–4.99 µs |
| 256 | 4 / 126,992 | 10.85–10.89 µs | 10.52–10.64 µs |
| 1,024 | 4 / 507,920 | 43.99–44.26 µs | 44.14–44.35 µs |

Ranges are the medians of the two alternating runs, including the noisy first retained 64-quad result. All retained controls made zero allocations after warming. Two BGA markers increased requested capacity by 992 bytes; the 256-quad submission medians were 10.78–10.88 µs current versus 10.65–10.71 µs retained. Submitted count/texture/BGA ordinal/lifecycle checksums matched; this is not a complete order/fallback/pixel-equivalence test.

At 256 commands/120 Hz, requested allocation capacity is about 15.2 MB/s, but this should not be mistaken for expensive copying or a similar increase in live memory. The paired timings show only fractions of a microsecond in most cases, with noise and no consistent benefit at 1,024 quads. The full 44 µs submission time is not attributable to these four allocations.

**Priority:** reasonable small cleanup if allocation reduction itself is wanted; not evidence for a material FPS or latency improvement. Retained scratch would need instance ownership, clearing on every path, reentrancy/lifetime analysis, and existing atomic preflight/fallback/lease behavior preserved.

## 3. Opaque blurred output does not need alpha blending, but the GPU gain was negligible

`BlurPass::drawOutput` enables alpha blending. Both positive-blur shaders write alpha 1, and final tint alpha is 1, so this particular final output is opaque. This does not authorize disabling blending for every zero-blur input composition.

A temporary output-state toggle compared the current blend with blending disabled at strength 2, downsample factor 2, and brightness 0.5. The input is a patterned opaque 256×256 still image. Each condition uses 120 warm-up and 300 measured frames; blend on/off runs alternate twice within forced-recomposition and production-cache controls. View profiling is disabled to retain normal encoder merging. These are whole-frame GPU durations, not isolated blend-unit timings.

| Output / mode | Blend enabled GPU median range | Blend disabled GPU median range |
| --- | ---: | ---: |
| 1280×720, recompose | 121.25–124.54 µs | 119.54–121.54 µs |
| 1280×720, cached | 47.88–48.88 µs | 48.42–48.50 µs |
| 2560×1440, recompose | 291.38–293.00 µs | 289.88–291.17 µs |
| 2560×1440, cached | 99.75–102.58 µs | 99.21–100.25 µs |

Draw counts stayed six for recomposition and one for cached output. CPU submission stayed approximately 13.6–14.0 µs and 4.8–5.2 µs respectively. RGBA readbacks were byte-identical across blend modes and recompute/cache at each resolution; nonblack output and released prepared leases were asserted outside timing. The bounded probe assumes draw-capacity acceptance and does not exercise saturation. The existing Metal test-harness reference-count warnings appeared on shutdown, without a rendering/readback failure.

**Priority:** low. The small, overlapping differences do not establish a material consistent improvement. These GPU numbers should not be directly compared with the older audit's different source image/profiling setup, much less treated as a phone benchmark. No shader or blending change was made.

## Other remaining candidates, not established bottlenecks

- **Touch-hit publication during realtime custom-skin play.** Every successful `PlaySkinSession::render` advances `touchHitRegionsRevision_`. The next refresh can rebuild transformed control regions and publish a newly allocated immutable snapshot even when geometry did not change. This requires an active realtime session with a touch router. It is not a lane-topology rebuild every frame and does not prove mutex contention. Actual control counts, publication cost and the cost of detecting equality were not measured. A useful next probe should compare unchanged/moving/visibility-changing controls and native overlays, preserving input correctness.
- **Native-video texture-cache flush policy.** `HardwareVideoFrameApple.mm::makeViews` flushes `CVMetalTextureCache` for each imported accepted frame, then the wrapper creates/retire external texture handles. This occurs on new video content, not each presentation frame. Its CPU cost and necessity under decoder-pool reuse are unmeasured. Changing it requires bounded-pool memory/lifetime evidence; removing ownership or retirement steps is not justified.

## Reclassify the suspicious 66.9 ms interval before optimizing it

Capture 25's 66.868 ms preparation stall occurs about 2.067 seconds before the last gameplay-tagged frame. Subtracting the long preparation interval leaves approximately two seconds. In `GamePlayScene::update`, chart completion sets `state->isEnding`, stops/transfers realtime authority, finishes replay capture, and calls `scheduleResultTransition`. That function captures and persists results synchronously before scheduling the deferred scene switch. The ordinary fallback delay is two seconds; selected skins derive the delay from their authored finish/fade timing.

This is consistent with terminal persistence followed by the result delay. It is **not proof**: the recording lacks `isEnding`, selected skin timings, scene identifiers, and token-correlated operation spans. A gameplay scene flag also remains true while finishing/animating. The 66.9 ms event therefore cannot confidently be called an active note-playing/rendering stall. The 360.9 ms post-request interval and 42.5 ms renderer interval around the transition likewise remain outside a demonstrated steady-play bottleneck.

The useful next real-session profile should mark active play versus ending and correlate bounded CPU spans for scene update, snapshot capture/projection, skin evaluation/submission, BGA preparation, and `bgfx::frame` with the existing frame token. Capture terminal persistence separately so it cannot dominate the active-play result. Keep CAMetalDisplayLink, input-worker ownership and immediate preparation unchanged; moving timestamps or adding deliberate waits does not reduce rendering work.

## Outcome

No production code or shaders changed. The current capture and measured components do not justify another broad rendering/pacing rewrite. The replay scan is the strongest concrete scaling target; ordinary live-play optimization should wait for stage-level evidence from active play rather than adopting the small controls above as assumed wins. Documentation and measurement data were reviewed with `git diff --check`; no new application build or full test-suite run is needed for these documentation-only changes.
