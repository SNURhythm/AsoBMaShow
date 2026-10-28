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
