# iOS Metal source audit: verified resize defect, unresolved portrait composition

Audited application commit `743022252a9a0ccddc4d0dfe778c55945406d395`, pinned
bgfx `6d0d15f2a51a4072ea7e4fa476d1b188b47708d8`, SDL
`6cee3b69deb1fbb6047ab560b6e2f591471ab0f9`, and the generated Metal backend.
The user reports that manual display-link recreation did not help and requested
source research instead of further device testing. This change contains research
and a local reproducer only; it does not change application rendering.

## Findings

### 1. Resize can combine a new drawable with stale MSAA attachments

**Verified correctness defect; not established as the portrait HUD cause.**

The application does not maintain one geometry contract across UIKit's Metal
layer, the supplied drawable, and bgfx's render attachments:

- `SDL/src/video/uikit/SDL_uikitmetalview.m:66` writes `drawableSize` during
  layout and emits `SDL_EVENT_WINDOW_METAL_VIEW_RESIZED`.
- `src/platform/IOSApplicationRuntime.mm:107` forwards SDL events from main to
  the application worker. Its viewport publication and event consumption are
  separate from Metal drawable delivery.
- `src/main.cpp:1128` obtains a display-link drawable **before** draining the
  application event queue. The validity closure checks lifecycle generation,
  not the renderer's geometry. A main-thread layout can also occur after the
  worker's event drain.
- `src/video/MetalDisplayLink.mm:79` compares the drawable texture with the
  **live layer**. It does not compare with bgfx's current render-target extent.
- `bgfx/bgfx/src/renderer_mtl.cpp:4974` allocates depth/MSAA attachments from
  bgfx's requested width/height. `setFrameBuffer` at line 2528 pairs these with
  `currentDrawableTexture()`. A new drawable can therefore pass our guard and
  still be incompatible with the old MSAA resolve source.

There is also a missing reconciliation trigger: all three resize-event checks
in `src/main.cpp` (lines 1417, 1483, 1510) omit
`SDL_EVENT_WINDOW_METAL_VIEW_RESIZED`. Ordinary window/pixel-size events normally
cover window rotation, so the omission does **not** mean every rotation fails.
It leaves Metal-view-only changes unhandled, and handling it alone would not
close the concurrent layout race. `SDL_GetWindowSizeInPixels` on UIKit derives
size from view bounds and screen nativeScale, not the actual Metal drawable;
the application's Display Zoom override is another writer of the layer size.

**Local reproduction:** compile the actual `MetalDisplayLink.mm` and link the
existing generated bgfx backend. Initialize 32×16 with 2× MSAA, change the layer
to 16×32 without reconciling the backend, then supply a real drawable through
the existing lease code. Its geometry check returns true. With Metal validation
enabled, rendering aborts with:

```text
MTLRenderPassDescriptor texture and resolveTexture size mismatch.
```

**Rejecting the lease is insufficient.** The provider patch returns handled-nil
to prevent illegal `nextDrawable()` acquisition. bgfx then allocates its dummy
render target using the *live layer size* (`renderer_mtl.cpp:5058`), again pairing
it with stale MSAA attachments. The reject case produces the same assertion.
Reconcile the attachments to 16×32 before rendering and the control exits zero
with a scheduled presentation.

This proves a rendering invariant failure on native macOS Metal, using tiny
fixtures and the production handoff/backend. It does not reproduce UIKit's
event timing, measure failure frequency on iOS, or query Direct/Composited.
The non-MSAA settings screen and the native probe do not exercise this MSAA
failure, so it cannot by itself explain the reported steady portrait symptom.

**Repair direction:** carry the renderer's expected extent/format through
drawable admission, reconcile Metal-view changes before admitting a frame, and
ensure the nil/rejected path uses compatible render attachments. Account for
layout changing after the event drain. Do not just add a last-moment presentation
check: encoder creation already fails earlier. Do not copy Apple's coarse layer
lock blindly: our bgfx resize synchronously dispatches layer configuration to
main, so blocking main on that worker can deadlock. This requires a coherent
geometry handoff, not automatic display-link recreation.

The existing native lease test uses a constant 16×16 surface without MSAA.
It verifies ownership and rejection, but cannot detect this mismatch.

### 2. bgfx forcibly selects Nearest for layer magnification

**Confirmed configuration problem for scaled landscape; not a complete portrait fix.**

`SwapChainMtl::init` (`renderer_mtl.cpp:4885`) sets `magnificationFilter` to
`nearest`. `cmake/IOSBgfxMainThreadLayer.cmake:20` deliberately preserves that
assignment when moving initialization to main. This controls composition of
the entire Metal layer, independently of samplers used to draw game textures.

The user's Nearest → Linear → Nearest comparison with Display Zoom in landscape
reversibly changed Composited → Direct → Composited. That is stronger evidence
than a speculative property tweak. The natural correction is to stop imposing
Nearest on the iOS layer at initialization, preserving normal game-texture
sampling. The main-thread patch's exact-match anchor must be updated coherently;
editing only generated source would be overwritten. There is no need to change
the pinned submodule for an application-specific generated-source correction.

Portrait still initially composed with Linear, and could later become Direct
with either filter after a background interval. The filter finding must not be
reported as a verified solution for that separate behavior or as a measured
latency/FPS improvement.

## Presentation paths checked and not established as defects

- **Command-buffer `presentDrawable`:** Apple's documentation says this calls
  the drawable's presentation method when the command buffer is scheduled.
  However, Apple's current downloadable CAMetalDisplayLink sample explicitly
  uses `[commandBuffer presentDrawable:currentDrawable]` followed by `commit`
  (`Shared/Renderer.mm:385`). The same sequence in our app and native probe is
  not evidence of API misuse. Replacing it with unsynchronized direct `present`
  based only on the deadline wording is not justified.
- **Background-thread display link:** the Apple sample supports a secondary
  run loop (`Shared/GameView.m:232`) and explicitly synchronizes resize against
  rendering. A worker is supported; the missing geometry invariant matters more
  than the choice of main versus worker. Our callback's strong drawable reference
  survives the callback autorelease pool, and bgfx retains it at acquisition.
- **One-frame submission lag:** iOS initializes with
  `BGFX_RESET_FLIP_AFTER_RENDER` and runs bgfx synchronously on the application
  worker. `Context::renderFrame` flips after submission in this mode. There is
  no demonstrated unconditional extra CPU frame before presentation.
- **Illegal acquisition on an attached link:** the provider remains managed
  between leases and after rejection. Fallback/export/lifecycle transitions
  invalidate the link before ordinary acquisition. This ownership logic is
  present; its handled-nil render-target behavior is the separate defect above.
- **Drawable count versus latency:** three allocated layer drawables and
  `preferredFrameLatency=1` are different controls. Apple permits latency one
  or two and describes it as a request. The source does not prove that reducing
  the drawable pool or GPU queue depth will restore Direct mode.
- **Layer shape, opacity and overlays:** the device snapshots already show
  matching steady configurations apart from orientation, and the SDL native
  baseline was Direct in portrait with the same secondary window. No new
  source evidence supports blaming that window, home-indicator preference,
  alpha-channel format, an app-applied rotation transform, or MSAA's offscreen
  texture as though it were an extra Core Animation layer.
- **Deadline handling:** `MetalDisplayLinkFrame::receive` rejects callbacks
  already beyond `targetTimestamp` but does not retain the deadline afterward.
  That is a timing-observability limitation, not proof that steady frames miss
  their deadline. Historical captures 23 and 26 actually reached assigned
  presentation targets during steady play. Adding waits or dropping every late
  prepared frame is not justified by this portrait observation.

Apple's native sample source was downloaded on 2026-10-30. SHA-256:
`GameView.m` = `b08d212895b5d0b11980c1c15353915a83a63c0063409a56d733e5e423afb404`;
`Renderer.mm` = `7ddd12e1f6023649629a95a76bf7ac3bad07514341ca3c117e0070673bd25c28`.
The sample is a reference, not proof that the OS cannot have a defect.

## What remains unresolved

The no-bgfx SDL CAMetalDisplayLink probe also reproduced portrait composition.
It renders on main, continuously, with a minimal triangle and the supplied
drawable. That excludes bgfx's MSAA/worker handoff defect as a necessary cause
of the whole observed symptom. Manual link recreation did not provide a fix.
Backgrounding changes more system state than recreating the link.

No reviewed public API or app-layer snapshot exposes the compositor's eligibility
reason. Apple's Direct-display window article is specifically about macOS and
does not guarantee the equivalent iPad behavior. The remaining possibility is a
system presentation-policy interaction with the Metal display-link path; this
is a hypothesis, not a confirmed iPadOS defect. There is no source-backed basis
here for claiming that an automatic restart, forced transaction flush, larger
latency request, smaller rendering resolution, or pacing delay fixes it.

No further manual device test is requested. Fixes for the independently proven
issues can be evaluated with automated/native coverage without presenting them
as a cure for the unresolved HUD behavior.

## Reproduce the verified failure locally

Host used: macOS 27.0.1 (26A434), Apple M1 Pro, Xcode 27.1 RC. This is a headless
native Metal test with a seeded real drawable, not an iPad simulator claim.
Build the existing `metal_drawable_provider_tests` target first if its libraries
are absent or stale; run only one build in `cmake-build-debug` at a time.

From the repository root:

```sh
c++ -std=c++23 -fobjc-arc -Isrc -Ibgfx/bgfx/include \
  docs/reviews/evidence/metal-resize/resize_harness.mm \
  cmake-build-debug/bgfx/cmake/bgfx/libbgfx.a \
  cmake-build-debug/libasobmashow_localization.a \
  cmake-build-debug/bgfx/cmake/bimg/libbimg.a \
  cmake-build-debug/bgfx/cmake/bx/libbx.a \
  -framework Foundation -framework Cocoa -framework Metal -framework QuartzCore \
  -framework IOKit -framework CoreFoundation -o /tmp/asobmashow-metal-resize-audit
MTL_DEBUG_LAYER=1 /tmp/asobmashow-metal-resize-audit stale
MTL_DEBUG_LAYER=1 /tmp/asobmashow-metal-resize-audit reject
MTL_DEBUG_LAYER=1 /tmp/asobmashow-metal-resize-audit reconcile
```

The first two runs intentionally abort with the resolve-size assertion. The
last exits zero and prints `AUDIT scheduled=1`. All three outcomes were checked
against the final committed reproducer source. In the control, reset executes
before attaching the link: the macOS backend also rewrites maximumDrawableCount
on resize, which QuartzCore forbids while a link is attached. That macOS-only
setter is not on the iOS resize path and is not an explanation of this iPad issue.

## Primary references

- [Apple CAMetalDisplayLink sample and download](https://developer.apple.com/documentation/metal/achieving-smooth-frame-rates-with-a-metal-display-link)
- [CAMetalDisplayLink contract](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink)
- [Command-buffer presentation semantics](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:))
- [Submission deadline](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp)
- [Requested frame latency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency)
- [Metal window setup, macOS scope](https://developer.apple.com/documentation/metal/managing-your-game-window-for-metal-in-macos)
- [Prior device observations](../testing/ios-portrait-metal-diagnostics.md)
- [Capture 23 target/presentation evidence](2026-10-28-metal-log23-analysis.md)
- [Capture 26 active-play evidence](2026-10-28-metal-log26-analysis.md)
