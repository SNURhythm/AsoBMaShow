# Built-in renderer source inspection

Inspected at `68e17f78`, following capture 27. This is a source audit, not a new
profiling change. The findings below establish executed work and ownership;
they do not assign measured device milliseconds to individual operations.

## First fix: avoid intermediate and invisible HUD texture creation

`BMSRenderer::applyPendingHudText` updates judgement, combo and timing labels
with `setText` followed by `setColor`. `applyPendingPacemakerText` and
`updateJudgementCounterText` use the same pattern. These are ordinary
`TextView` instances, constructed around BMSRenderer.cpp:740–859; no deferred
texture mode is enabled for them. Style replacement at `refreshJudgementFeedbackTextStyle`
also creates fresh ordinary TextViews and must preserve any chosen policy.

`TextView::setResolvedText` (TextView.cpp:460) skips equal strings but otherwise
invalidates the texture, updates metrics and immediately calls `createTexture`.
`setColor` (:1115) skips equal colors but otherwise destroys and recreates the
texture too. `createTexture` (:1250) rasterizes the complete text through SDL_ttf
or its fallback compositor. `sdlSurfaceToBgfxTexture` (rendering/common.cpp:31)
allocates and copies all BGRA pixels and queues creation of a new GPU texture.

Consequently, when both text and color change, the first texture has the old
color and is destroyed before rendering; a second raster/upload is queued for
the actual result. This is conditional on both values changing, not a claim
that every label is rasterized on every frame. Revisions and equality checks
already suppress unchanged text work.

There is also unnecessary hidden work: `renderFrame` calls
`updateJudgementCounterText` regardless of `judgementCounterEnabled`, while
counter drawing is gated later. New count strings can therefore rasterize and
upload even when those counters are not drawn. The loop also updates each
counter's color; unchanged entries are guarded by TextView equality checks.

The narrow remedy is to enable the **existing deferred texture materialization**
for gameplay's frequently changing labels, including replacement views.
`setResolvedText` still calculates metrics, so width/height queries for layout
remain available; `renderImpl` materializes the final texture when actually
drawn. This coalesces text/color changes and avoids hidden uploads. It does not
remove text measurement or the final visible raster/upload. Verify visible
pixels, layout, style replacement, hide/show and judgement color transitions.
No global TextView behavior change or new timing fields are needed.

A glyph-atlas/digit renderer could remove repeated whole-string rasterization
more broadly, but is a larger follow-up with font/fallback/shaping requirements.
Start with the existing deferred path rather than immediately replacing text.

## Second candidate: long-note scratch still allocates nodes each frame

`longNoteLookaheadScratch` is an `unordered_map<LongNote*, LongNoteLookahead>`
(BMSRenderer.h:289). Construction reserves buckets for twice the lane count,
but the parser-backed `renderFrame` branch clears the map each frame and uses
`try_emplace` for orphan and encountered heads; matching tails erase nodes.
Reserve retains buckets, not the individual map nodes. Unchanged visible or
held long notes therefore cause repeated node allocation/deallocation.

A retained flat scratch structure or reusable node storage could remove that
churn. Preserve multiple visible heads on a lane, head/tail pairing, orphan and
dead HCN retention, budget reservation and submission order. Do not assume one
entry per lane or substitute a full-chart scan. This is a concrete allocation
pattern, not evidence that it dominates capture 27.

## Correct interpretation of bgfx submission on iOS

`main.cpp:774` calls `bgfx::renderFrame()` on the application worker before
`runApplication` initializes bgfx. `Context::init` recognizes that latch and
selects single-threaded operation (bgfx.cpp:2376). `renderSemWait` explicitly
does nothing in that mode (bgfx_p.h:6779). `Context::swap` invokes `renderFrame`
synchronously; it executes resource pre-commands, Metal submission and resource
post-commands (bgfx.cpp:2918).

Thus earlier suggestions to attribute this device's `bgfx::frame` interval to
waiting on an internal render thread were incorrect. Texture creation/destruction
queued by eager HUD updates can also add resource work to this interval.
Backend GPU/driver waits remain possible; single-threaded mode does not prove
that all elapsed time is CPU busy time. The generated Metal backend uses the
managed drawable when supplied, with `nextDrawable` as fallback; do not blame
the fallback acquisition path merely because it exists in upstream code.

## Paths checked and not promoted as first fixes

- Future note traversal stops at the relevant viewport boundary; it is not an
  unconditional full-chart scan. Past/orphan long-note behavior is intentional.
- Simple/Tex batch renderer vertex and index vectors retain capacity. Their
  empty flushes return early. Unconditionally removing flush boundaries risks
  changing draw order rather than solving demonstrated allocation churn.
- `layoutCenteredJudgementText` caches geometry/visibility. General HUD layout
  still computes placements, but View's unchanged positions and sizes guard
  actual layout requests. Repeated calls alone do not prove full Yoga rebuilds.
- Main title/metrics panels pass radius zero, so the rounded-rectangle helper
  takes its rectangle fast path there; its trigonometric tessellation is not a
  valid explanation for those panels.
- Built-in preparation allocates a prepared-frame object and copies state,
  but capture 27 already puts that whole stage near 0.014 ms median. It is a
  weaker first target than removing avoidable HUD raster/upload work.

Only documentation changed in this pass. Verified the call chains, guards,
construction/replacement behavior and generated Metal acquisition branch;
reviewed the diff. No application tests or new device capture were needed to
establish these source findings. Recommended implementation order: deferred
HUD materialization first, long-note scratch allocation second.

## Implemented HUD follow-up

Dynamic judgement, score/combo, pacemaker, gauge, lane-cover and counter TextViews
now enable deferred texture materialization immediately after construction.
Judgement font/style replacements preserve the policy. Metrics still update at
mutation time; drawing creates the final texture. Static title, option and
autoplay labels retain their existing behavior. No global TextView semantics,
frame pacing or input handling changed. Long-note scratch is a separate,
unimplemented candidate.

The new production-renderer regression failed before the change for eager
initial combo and replacement uploads, then passed with nonzero pre-draw metrics
and successful next-draw materialization. Desktop main and the selected built-in
Metal characterization, Metal text, and text-input suites pass. No device FPS
or frame-time improvement is claimed from these correctness checks.
The unsigned iOS build also passes via the repository's build-only helper.

## Further source inspection after the HUD fix

Inspected at `df303fcb`. These are additional source-confirmed opportunities,
not measured frame-time savings. No new instrumentation or runtime changes
were added in this pass.

### Temporary batch renderers discard their capacity every frame

`renderVirtualControllerOverlay` (GamePlayScene.cpp:600, called at :7275)
constructs a local `SimpleBatchRenderer` after its valid-layout guard. Its
constructor (SimpleBatchRenderer.cpp:16) reserves 4,096 `PosColorVertex` entries
and 6,144 uint16 indices. With the 16-byte vertex in rendering/common.h:34,
that requests at least 77,824 bytes (76 KiB) of CPU vector capacity each call,
then frees it on return. This is allocation churn, not a claim that all of
those bytes are written or uploaded. It occurs with a valid virtual-controller
overlay in manual play, not with the controller disabled or autoplay enabled.

`renderCoursePauseHoldRing` (GamePlayScene.cpp:7597) uses another local instance
while the course pause button is visible, even when hold progress is zero
because the base ring is still drawn. The same reservation cost applies.
Retain these buffers in scene-owned renderers, preserving view and draw order.
The main BMSRenderer's retained batches already reuse capacity; the earlier
observation about their efficiency does not cover these local instances.

### The virtual-controller layout is rebuilt for drawing unchanged controls

The render call above also invokes `currentVirtualControllerLayout` (:562),
which unconditionally calls `makeVirtualControllerLayout`
(VirtualControllerLayout.cpp:94). For an enabled, supported configuration it
builds scratch/key-lane vectors and element geometry. The builder constructs
elements for requested bounds and again for the fitted size. Pressed state and
scratch rotation are passed separately to rendering and do not require this
layout reconstruction. Cache by controller configuration, key mode and canvas
dimensions, while retaining invalidation for resize and configuration changes.

### A lane-order update builds and discards the entire touch layout

`renderScene` calls `refreshGameplayPresentationGeometry` every rendered frame
(GamePlayScene.cpp:7223). In built-in mode with an input handler, that calls
`presentation->touchLayout().lanes` (:2190). BMSRenderer::touchLayout (:4316)
projects four corners, normalizes coordinates, copies the lane vector and
builds scratch flags. Only the lanes survive the expression. The unchanged
lane-order guard in RhythmInputHandler.cpp:474 runs after all this work.
Guard by the relevant presentation revision before constructing the layout,
or expose the already-retained displayed lane order without constructing the
rest. Preserve invalidation when presentation mode or displayed lanes change.
The legacy/realtime touch-layout refresh paths have their own guards; this
finding concerns the extra unconditional lane-order request.

### Scalar Hi-Speed queries allocate a traversal lane vector

`effectiveVisibleTimeGreenNumber` (BMSRenderer.cpp:2242) calls
`builtInProjectionTraversal().configuredHispeed`. That helper reserves and
fills `playableLaneOrder` (:2213), although this caller immediately discards
it. `laneCoverVisibleTimeLabel` (:2256) calls it again in milliseconds mode,
after already computing an unused green number. The visible cover-label path
calls this while laying out labels (:5209); hidden/insufficient cover bounds
return earlier. Extract the shared scalar Hi-Speed calculation and select the
label mode before doing its calculation. Keep projection's lane-order payload
for actual projection callers.

### Start-lane indicators are recopied and normalized after they disappear

Capture copies `preparationPlan.laneIndicator.lanes` into authority
(GamePlayScene.cpp:6440). Built-in preparation unconditionally passes those
lanes by value to `setStartLaneIndicators` (BMSRenderer.cpp:2728), which sorts,
deduplicates and replaces its retained vector (:5051), before separately
setting visibility. A nonempty preparation list therefore keeps producing
copies and normalization even when its visibility is false. The coordinator
also copies the list into its pending frame. Treat lane content and visibility
as independent updates; normalize only when content changes, preserving later
practice/count-in reappearance.

Priority: retain temporary batch buffers first, cache virtual-controller and
touch-layout work next, then remove scalar-query and indicator churn. The
previously identified long-note map node churn remains another candidate.
A suspected full-chart BPM summary was excluded from normal-play priorities:
the scene supplies `configuredHispeedOverride`, bypassing the fallback that
would call it. Verification for this documentation-only pass was call-site and
guard inspection plus diff review; no build or device measurement was needed.
