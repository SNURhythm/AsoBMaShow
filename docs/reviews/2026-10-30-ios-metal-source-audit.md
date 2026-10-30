# iOS Metal source audit: verified resize defect, unresolved portrait composition

Audited application commit `743022252a9a0ccddc4d0dfe778c55945406d395`, pinned
bgfx `6d0d15f2a51a4072ea7e4fa476d1b188b47708d8`, SDL
`6cee3b69deb1fbb6047ab560b6e2f591471ab0f9`, and the generated Metal backend.
The user reports that manual display-link recreation did not help and requested
source research instead of further device testing. This change contains research
and a local reproducer only; it does not change application rendering.

## Follow-up fix

The subsequent fix validates each acquired drawable against the renderer-owned
framebuffer extent and format before binding it. Rejected or missing drawables
use a dummy texture with that same extent/format, so old depth/MSAA attachments
remain compatible even if UIKit lays out the layer concurrently. Swap-chain
resize now updates the framebuffer's stored extent. Secondary windows retain
their own dimensions. This closes the encoding race without blocking main or
recreating the display link; a rejected frame is not presented, and a reconciled
frame resumes normal presentation.

The app now coalesces and handles `SDL_EVENT_WINDOW_METAL_VIEW_RESIZED` alongside
window/pixel-size events, and reasserts the intended layer size even when no
bgfx reset is needed. Equal native property values are left untouched. iOS layer
initialization explicitly selects Linear instead of Nearest; game texture
samplers, frame latency, drawable count, resolution policy and pacing are unchanged.

Regression coverage extends the existing native drawable-provider test under
Metal API validation: supplied incompatible drawable, handled-nil fallback,
resize recovery with and without MSAA, sRGB, and a distinct secondary-surface
extent. The new test failed against the old backend and passes with the fix.
The desktop `main` and unsigned iOS build passed, as did all 74 rendering/video
tests and all 55 iOS build-setup checks. The generated-source checks verify the
Linear initialization and unchanged main-thread boundary.

The original reproducer below is retained as audit evidence. With the fix,
`stale` and `reject` now exit successfully with `AUDIT scheduled=0`; `reconcile`
continues to report `AUDIT scheduled=1`. The original abort outcomes refer to
the audited commit above. These fixes do not establish a cure for the separate
steady portrait compositor state described later in this report.

**Device follow-up:** after Firebase build `202610301045`, containing fix commit
`2c868e6f634c63660ec1bd3e200bcb27217a9dad`, the user reports that portrait
remains Composited. The fix therefore did not resolve the reported portrait
symptom. The native regression results establish the attachment repair only;
they must not be treated as validation of Direct presentation on this device.

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

### 3. SDL's implicit refresh clock remains active beside the Metal link

**Verified redundant callback source; portrait causality remains unverified.**

`SDL_uikitviewcontroller.m:109–121` automatically creates a CADisplayLink when
the primary display advertises more than 60 Hz, even without an animation
callback. At 120 Hz it requests the range 80/120/120. `doLoop:` does no application
work when `animationCallback` is null, but the display link still fires. The app
did not stop this clock when its CAMetalDisplayLink took over presentation.
This behavior also exists in the earlier **SDL-surface** native probes, so the
CAMetalDisplayLink probe did not isolate Metal scheduling from SDL's refresh
request. The pure UIKit probe that was Direct used CADisplayLink instead.

The follow-up change keeps CAMetalDisplayLink and uses an owner-thread attachment
notification to disable SDL's implicit clock through the public
`SDL_SetiOSAnimationCallback(window, 1, nullptr, nullptr)` API. On detach, the
same API installs a no-op callback only for a >60 Hz primary display, preserving
the high-refresh request during timer fallback. SDL's restore API requests
120/120/120 at a 120 Hz screen, rather than its startup 80/120/120 range. The
application frame cap still governs fallback rendering; the helper never runs
application work. The Metal rate request, latency one, drawable count three,
resolution and presentation sequence are unchanged. Frame completion does not
toggle the helper, and repeated invalidation does not recreate it repeatedly.

A standalone Simulator harness links the actual built SDL framework and the
new application helper. It observes real SDL and Metal callbacks, disables SDL
from a worker through the main-thread bridge, verifies 20 further Metal frames
with no additional SDL ticks, then verifies SDL ticks resume after Metal is
invalidated. The helper stub fails the stopped-clock assertion; the implemented
helper passes. The simulator's advertised screen capability is overridden to
exercise SDL's >60 Hz branch: this is **not a physical 120 Hz throughput test**.
The native drawable-provider regression also fails without attachment
notifications and passes with them, covering attach, frame completion, detach
and repeated pause. See the [reproduction harnesses](evidence/metal-clock/README.md).
The 60 Hz capability case also passes without creating a fallback clock. The
desktop `main` build, unsigned iOS build, native drawable-provider test and
frame-pacing test all pass after the ownership change.

The coexistence finding suggested an interaction between Core Animation and
Metal refresh requests, but the local tests establish clock ownership only.
It must not be presented as a proven portrait fix. The old SDL-surface probes
retain their original behavior as historical controls; the ownership change
applies to normal app rendering.

**Device follow-up:** after distribution of build `202610301116` (commit
`552f60b2`), the user again reports portrait Composited. Removing the duplicate
clock therefore did not resolve the reported symptom. This is a user report,
not a new captured trace proving attachment state or installed-build identity.
Further source review found that SDL's Metal-view replacement reuses
`data.viewcontroller`, and normal resize/rotation does not call `startAnimation`
or create another controller. There is no source evidence that rotation silently
undoes the suppression. Do not present this cleanup as progress demonstrated
toward Direct mode, or use the failed result to declare an iPadOS bug proven.

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

The existing SDL-surface probe comparison makes the scheduling/acquisition path
the strongest remaining lead: CADisplayLink with `nextDrawable` was Direct,
whereas CAMetalDisplayLink with `update.drawable` reproduced portrait composition
without bgfx. The probe uses the same drawing/submission routine in both modes.
This comparison changes both scheduling and drawable acquisition; its callback
clock also differs. It does not isolate an internal OS cause or prove that
replacing the application's pacing API preserves gameplay latency. A production
CADisplayLink alternative is consequently an unvalidated workaround, not a
verified performance-neutral fix. Rechecking the probe against Apple's sample
found no new submission-sequence defect that explains the orientation split.

The subsequently discovered implicit SDL clock (finding 3) further limits this
comparison. Suppressing it while retaining CAMetalDisplayLink did not resolve
the reported symptom. The user explicitly requires CAMetalDisplayLink;
replacing it with CADisplayLink is not the chosen direction. No additional
source-backed portrait fix has been established. A
[technical handoff draft](2026-10-30-ios-metal-portrait-handoff.md) records the remaining evidence boundary
without asserting an OS defect or requesting further manual device tests.

An additional Simulator experiment uses the production `MetalDisplayLink.mm`
delegate and lease/provider code with real OS callbacks on a worker. In both
the app-style nested main-run-loop pumping mode and an ordinary UIKit run loop,
120 frames were submitted, all observed scheduled handlers preceded the supplied
deadline, and four nonanimated layer-position changes reached the presentation
tree without a forced transaction flush. The largest observed convergence delay
was approximately 16.4 ms on that Simulator. This does not reproduce transaction
starvation or establish a benefit from forcing CATransaction flushes. It is a
small clear-pass harness, not the full application's workload; Simulator does
not provide physical iPad scanout or the drawable presented-time API here.

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

## Second source pass: lifecycle, queue ordering and drawable storage

This pass audits application `4bf2c04b` with the same pinned SDL/bgfx revisions
listed above, including the generated-source patches. It adds a real-callback
storage experiment. It does not introduce another rendering workaround.

### Link recreation does not recreate the Metal surface

The manual action in `src/video/MetalDisplayLink.mm:205` eventually calls
`invalidate` at line 69 and constructs another link against the **same** incoming
layer at line 146. Clearing `state.layer` releases that owner's reference; it
does not destroy the UIView's backing layer. `src/main.cpp:685` keeps the native
layer used for subsequent frames. bgfx retains that layer in
`SwapChainMtl::init`; it does not replace it on an ordinary reset.

Apple's installed iOS 27.1 `CAMetalLayer.h` explicitly assigns the texture pool
to the layer. The `CAMetalDisplayLink.invalidate` contract removes the link's
run-loop associations; it does not promise replacement of layer-owned storage.

The new [storage experiment](evidence/metal-pool/README.md) tests this with actual
Simulator callbacks through the production link/provider. Forty frames before
and forty after the manual recreation used exactly the same three IOSurfaces,
while the link serial increased from 1 to 2. Swapping drawable dimensions and
then restoring them produced different surface sets. All 160 frames completed,
without begin-frame retries or drawable rejection in the retained final run.
Retained IOSurface references prevent ID recycling from faking identity;
textures and drawables are not retained by the collector across frames.

**Implication:** the failed manual recreation experiment only tested replacement
of the scheduler attachment, not replacement of the Metal surface or its pool.
Identical CALayer property snapshots also do not show IOSurface allocation or
system presentation state. Layer-owned state remains an untested boundary on
the physical iPad. However, this experiment **does not establish stale storage
as the cause**: resizing replaced buffers locally, and no physical-device pool
trace establishes what backgrounding changed. Do not promote this finding into
an automatic layer-recreation, size-toggle or device-reset workaround.

### SDL has no ordinary foreground swap-chain rebuild

The pinned scene callbacks in `SDL_uikitappdelegate.m:518–549` forward lifecycle
events. `SDL/src/video/SDL_video.c:6223–6256` sends minimized/restored and app
events and changes keyboard focus. These functions do not construct another
Metal view, reassign its device, rotate a drawable, or rebuild a bgfx swap chain.
The exceptional scene-replacement path at `SDL_uikitappdelegate.m:418–438`
reattaches the existing window to a new scene; it is not the normal
background/foreground callback path.

`SDL_uikitview.m:181–235` swaps a controller's root view when a Metal view is
created/destroyed; that path is distinct from rotation. Rotation's Metal work is
`SDL_uikitmetalview.m:62–79`: layout calculates bounds × contentsScale, updates
the drawable extent if different, and emits the Metal-view resize event. It
contains no portrait-specific pixel copy or presentation mode. UIKit owns the
orientation transform outside this method. This supports investigating the
system's lifecycle/presentation state, but does not prove an operating-system bug.

### bgfx's separate presentation buffer does not imply an extra frame

`renderer_mtl.cpp:7135–7141` commits the rendering command buffer;
`flip` at lines 2039–2076 then allocates a presentation command buffer from the
same `CommandQueueMtl`. `alloc` at line 5246 and `kick` at line 5269 use the same
`m_commandQueue`; `kick(false, false)` commits without a CPU completion wait.
`bgfx.cpp:2967–2975` calls this flip after submission when
`BGFX_RESET_FLIP_AFTER_RENDER` is enabled, as this app enables it on iOS.
The generated provider patch gates the drawable but preserves this ordering.

There is no portrait-specific queue, offscreen compositor surface, or extra
application frame in this path. MSAA resolve textures are GPU render attachments,
not additional Core Animation layers. This does not claim zero scheduling cost:
the separate present buffer still has to be scheduled before the Metal link's
deadline. The minimal no-bgfx probe already shows that this bgfx buffer split is
not necessary for the reported portrait symptom.

### Fresh guidance checked, with platform boundaries retained

Apple's current [drawable presentation guidance](https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/presenting-metal-drawables/references/frame-pacing.md)
emphasizes that the link pre-acquires a drawable and later layer changes apply
to subsequent drawables. That supports the existing resize-compatibility guard;
it does not mean the current supplied texture changes size when the layer does.
The old geometry race was real, but its fix has already failed to cure steady
portrait composition on the device.

The same repository's direct-display discussion is explicitly macOS-oriented.
The installed iOS header marks `displaySyncEnabled` unavailable on iOS, so
macOS advice to toggle it is not an available public iPad fix. Metal 4
`waitForDrawable`/`signalDrawable` guidance also cannot be pasted into bgfx's
existing `MTLCommandBuffer` presentation path. Apple's guide explicitly
distinguishes those queue-level APIs from command-buffer presentation.

The remaining evidence gap is now more precise: scheduler attachment, drawable
storage, and system presentation state are different things. The existing
physical results and new local source/identity checks still do not identify
which state changes when portrait becomes Direct after backgrounding. No new
source-backed cure is established, and no further manual test is requested.

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
