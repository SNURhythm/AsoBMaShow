# Render pipeline audit — 2026-10-28

Audited `74e7bb69727f54248e4fb7554194efa1682b292f` on `fix/metal-hud-rendering-overhead`. The findings describe that baseline. All four are now addressed; implementation and verification results follow the original investigation below.

## Findings

### 1. High: replay export can write beyond a short transient allocation

`renderTextureRegionTint` in `src/rendering/common.cpp:148–163` requests four vertices and six indices, then writes all of them without checking capacity or the returned allocation sizes. `renderFullscreenTextureTint` delegates to it. Replay export still uses this helper after rendering gameplay and BGA composition/blur (`src/ReplayVideoExporter.cpp:3655–3659`, with equivalent playlist/result-tail paths).

The transient pools belong to the whole bgfx frame. An earlier consumer can leave less than one quad available. bgfx's allocator clamps the allocation to available space; it does not grow the pool. Debug builds report a check failure, while code that continues writes past the returned region. This is a memory-safety boundary, not just a missing draw.

A production-bgfx Noop probe used the app's 16 MiB vertex/4 MiB index limits, consumed all but two textured vertices and three indices, and requested the same final quad:

```text
requested vertices: 4; available: 2; returned bytes: 40; helper writes: 80
requested indices:  6; available: 3; returned bytes:  6; helper writes: 12
bgfx debug checks: 2
```

The probe recorded the short allocations without performing the unsafe writes. This establishes the allocator behavior and the source-level overrun; it is not a reproduced production export crash or evidence that an ordinary chart reaches the limit.

The live fullscreen path now uses `submitFullscreenComplete`, `executeComplete`, and checked `drawOutput`. Export still calls the void wrappers and the unchecked final helper. It also queues a readback after those calls without checking their completion, so a failed composition/blur can become an encoded frame containing stale or missing pixels.

**Direction:** make the shared fullscreen path return an explicit outcome in both live rendering and export. Reserve/check its complete geometry requirement before submission, propagate failures to export, and remove unchecked allocation assumptions from the remaining common helpers. Local preflight in a skin or blur pass does not account for every later consumer in the frame.

### 2. Medium: silently dropped UI draws can be acknowledged as a completed presentation

`BgfxUiBatchBackend::submit` calls the void `bgfx::submit` API and returns `true` (`src/rendering/UiBatchRenderer.cpp:247–249`). That API can discard a draw when the frame's render-item limit is full. The backend cannot detect this through its return value.

After `bgfx::frame`, the main loop checks draw demand for `FullscreenBgaCache`, then unconditionally calls `uiPresentationSchedule.didPresent` (`src/main.cpp:1978–1984`). The corresponding UI revision can therefore be acknowledged even when its draw was dropped. A static UI has no immediate retry and can retain the incomplete output until another invalidation or its one-second maintenance redraw. Continuous gameplay retries naturally; the idle UI is the important distinction.

A Metal readback probe used the actual `UiBatchRenderer` and `PresentationSchedule`. It filled the render-item pool with `touch` submissions, then requested an opaque red quad:

```text
normal:    append=true flush=true next-frame retry=false red=255 peak=1
saturated: append=true flush=true next-frame retry=false red=0   peak=65536
recovery:  append=true flush=true next-frame retry=false red=255 peak=65536
render-item capacity: 65535
```

The saturated frame was black despite successful batch return values. Explicitly drawing another frame recovered the red output. This is a forced capacity-boundary reproduction, not a claim that normal menus submit 65,536 items.

Ordinary dynamic UI upload failures already call `ui::invalidate`, and the pre-draw revision capture preserves that invalidation. The missing signal here is bgfx's silent rejection. `SimpleBatchRenderer`/`TexBatchRenderer` also clear their queued geometry on transient exhaustion without returning a frame outcome.

**Direction:** give the frame owner a shared completion/failure result and acknowledge idle presentation only when the required work was accepted. Prefer frame-specific draw accounting or a submission budget; the existing public `numDrawCallsPeak` is a lifetime peak, so using it as a permanent UI retry condition would create a busy loop after one overload. Export needs the same outcome before accepting a frame for encoding.

### 3. Medium: blur targets wrap across opposite screen edges

`BlurPass` creates its scene and intermediate textures with `BGFX_TEXTURE_RT` alone (`src/rendering/BlurPass.cpp:191–240`). Its blur draws bind them without sampler overrides (`:295`, `:315`). This uses repeating texture addressing, while the blur shaders sample beyond `[0, 1]` near an edge.

A normal fullscreen blur can consequently mix pixels from the opposite edge. This affects changed output before caching; cached/fresh pixel-equivalence tests preserve the same defect in both paths.

A Metal probe rendered a 64×32 opaque image with a red strip in the leftmost 16 columns and black elsewhere, using the production `Jukebox` composition and `BlurPass`, strength 2, downsample 1:

```text
current targets: left-edge red=150, right-edge red=105, right-interior red=0
clamped targets: left-edge red=255, right-edge red=0,   right-interior red=0
```

The comparison changed only the three render-target texture flags to include `BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP` in a temporary copy of `BlurPass.cpp`. Production files were not edited. The same addressing issue applies vertically. The fixture amplifies the boundary for inspection; it does not imply a 64-pixel production render target.

**Direction:** define clamp addressing explicitly for screen-space intermediates and add an edge-color oracle. Compare against the expected edge behavior, in addition to comparing cached and recomputed output.

### 4. Low: repeated output clears add avoidable fullscreen work

The main loop configures an initial color/depth clear, another color clear for the final BGA view, and depth clears for both gameplay and UI (`src/main.cpp:921–934`). Current application draw states do not enable depth testing or depth writes; the gameplay view's `DepthAscending` is a submission sort mode.

In the checked-in Metal backend, consecutive views sharing a framebuffer normally share a render encoder. A subsequent view clear is then executed by `clearQuad` (`bgfx/bgfx/src/renderer_mtl.cpp:6030–6032`, `:6214–6217`), adding raster work. The view count alone should not be interpreted as a count of separate Metal render passes.

An isolated Metal probe reproduced the four output views, a color/depth framebuffer, one opaque fullscreen draw and two small overlay draws. It retained the initial color/depth clear and alternated the three later clears on/off. View profiling was disabled to preserve normal encoder merging. Medians are 300 GPU samples after 120 warm-up frames in each run:

| Output size | Repeated clears, two runs | Initial clear only, two runs | Paired reduction |
| --- | ---: | ---: | ---: |
| 1280×720 | 59.7 / 55.4 µs | 53.4 / 50.3 µs | 6.3 / 5.0 µs |
| 2560×1440 | 99.2 / 92.2 µs | 77.0 / 77.1 µs | 22.2 / 15.1 µs |

Full RGBA readbacks were identical. These are small component savings on this Mac, not a demonstrated app FPS improvement. The probe used an offscreen RGBA8/D32F target without MSAA, not an iPhone swapchain.

Keep the initial attachment clear: a separate exploratory variant removing that depth clear made Metal load the attachment and was slower. Simplification needs to preserve attachment load behavior, direct-BGA letterboxing/alpha, and frames without a fullscreen producer.

## Architectural assessment

The main structural weakness is that frame resource accounting and completion are distributed across renderers. A custom skin preflights its own geometry, blur checks its own quads, the BGA cache checks draw demand, and the UI scheduler separately acknowledges presentation. Export duplicates fullscreen composition and still uses older unchecked calls. These contracts do not add up to a frame-wide guarantee.

A useful refactor is a shared frame plan/outcome for live and export paths: identify the required passes and their resource needs, account for the remaining frame budget, submit, then acknowledge presentation/cache reuse or encoding from the same outcome. The unchecked helper needs an immediate bounded fix independently of that refactor. Explicit sampler boundaries and clear ownership are smaller, localized changes.

The CPU submits scene/UI before the fullscreen BGA tail, but `RenderPlan` orders BGA and blur before output on the GPU. This ordering itself is valid. The inspected ownership/lifecycle paths already serialize export on the renderer owner and handle pending iOS submissions; this pass did not establish another ownership or lifecycle defect. A wholesale renderer replacement is not supported by these findings.

## Scope and verification

Reviewed live frame scheduling, view ordering/state, transient and draw-item capacity, UI batching, BGA composition/cache/blur, iOS presentation gates, and export/readback boundaries. Diagnostic programs linked the current production test objects and actual bgfx backend. The allocation probe used Noop; UI, blur and clear probes used headless Metal on this Apple M1 Pro Mac. No private chart archive was scanned.

All initial diagnostic runs completed successfully with the expected assertions/observations. The initial blur comparison and clear experiment were temporary probe variants; that investigation changed only this report. Physical-device behavior, actual menu capacity pressure, and end-to-end frame/input latency were not measured.

## Implemented fixes and results

Common quad writers now preflight both transient pools and validate returned pointers and byte ranges before writing. All five export fullscreen paths share checked preparation, composition, blur and final output; a failure releases its prepared lease and cannot reach readback/encoding. Both single-chart and course export gates also reject incomplete UI uploads, bgfx draw drops and legacy batch allocation failures, returning their reserved encoder/readback storage.

The live frame owner now acknowledges UI revisions only after the required submissions succeed. A small wrapper around the pinned bgfx translation unit exposes the current encoder's drop state before `frame()`, without modifying the submodule or waiting for the GPU. Additional encoders are conservatively unsupported. Scoped application failure accounting retains `SimpleBatchRenderer`/`TexBatchRenderer` allocation failures across batch resets and later successes. UI uploads retain their own per-frame failure latch. Fresh accepted frames recover normally: idle UI rendering can stop, and fullscreen BGA caching can resume even while the lifetime draw-demand peak remains high. These signals establish CPU submission acceptance, not GPU completion or successful display.

The scene and both blur intermediate textures explicitly clamp U/V addressing. Ordinary output retains the initial color/depth clear and removes the later final-color and gameplay/UI-depth clears. The direct BGA path retains its opaque letterbox clear, and offscreen composition/blur clears remain intact.

The final isolated Metal comparison uses the same M1 Pro, RGBA8/D32F target, four output views, 120 warm-up frames and 300 GPU samples as the audit probe:

| Output size | Repeated clears, two runs | Initial clear only, two runs | Paired reduction |
| --- | ---: | ---: | ---: |
| 1280×720 | 51.75 / 43.00 µs | 44.46 / 37.25 µs | 7.29 / 5.75 µs |
| 2560×1440 | 93.38 / 93.38 µs | 77.25 / 75.83 µs | 16.13 / 17.54 µs |

Full RGBA readbacks remain identical. This is a small measured reduction in component GPU work, not evidence of an app FPS or phone latency improvement. The other three fixes address correctness; they are not presented as speedups.

Verification:

- Guarded allocation regressions reproduce the original short-write boundary and now pass for vertex/index exhaustion, short returned ranges, null storage and success. Legacy batch regressions reproduce the missed failure and verify rejection, safe writes and subsequent recovery.
- Both actual export submission lambdas are exercised for render, UI, draw-drop and legacy allocation failures, resource release and successful recovery. Fullscreen stage failures also verify exactly-once prepared-lease release.
- Native Noop coverage checks exact draw capacity, saturation, subsequent recovery despite a high historical peak, additional encoders and initialization/shutdown. Real Metal coverage checks UI saturation/retry/recovery, independent horizontal/vertical blur edge colors, and BGA output/cache/lease behavior.
- Desktop `main` and unsigned iOS device builds pass. All 77 rendering/video checks plus three additional distinct UI/export checks pass: **80 distinct tests**. The focused extra run has six tests, three overlapping the rendering/video group. No Android build or physical-device run was performed.
- Independent integration review found a legacy batch failure-propagation gap; the regression was reproduced and fixed. Follow-up review reports no findings. No deployment was performed.
