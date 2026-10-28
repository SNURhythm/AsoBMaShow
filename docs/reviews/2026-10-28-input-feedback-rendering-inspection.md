# Input feedback rendering inspection

Inspected at `2756be6f`, after the user reported approximately 0.5 ms GPU time
without input and 0.7 ms with input. Capture 27 previously recorded 0.766 ms
median whole-command-buffer GPU time and 0.924 ms P99; the new HUD observations
are not a matched capture. This inspection identifies avoidable submitted work,
not the measured cause or recoverable fraction of the 0.2 ms difference.

## Lane beams submit regions hidden by opaque covers

`BMSRenderer::drawLaneBeam` (BMSRenderer.cpp:5135) emits one gradient quad from
`judgeY` to `judgeY + (upperBound - judgeY) * beamScale`. It does not restrict
that quad to the uncovered lane interval. Later, `drawLaneCover` (:5304)
overwrites the top Sudden+ region and applicable bottom Hidden/Lift region
with opaque rectangles. A sufficiently long beam therefore submits covered
geometry. The actual fragments saved by clipping depend on the GPU's handling
of overdraw; source inspection does not measure them.

Clip beams against genuinely opaque cover regions, preserving the original
gradient slope and lane-beam length semantics. Simply replacing `upperBound`
with `noteVisibleUpperBound` would compress the fade and change visible pixels.
Validate cover boundaries, edge decorations, projection and MSAA coverage;
packing a newly interpolated endpoint into 8-bit alpha can also introduce
rounding differences. Do not discard visible beams to reduce cost.

The beam's early return checks floating-point alpha before converting to an
8-bit color. A small positive alpha can become byte zero and still submit a
fully transparent quad. Check the final packed alpha too. Ordinary beams are
only two triangles each; their potential GPU cost is mostly covered area and
blending, not geometry count.

## Keybombs have transparent tails and duplicate fan vertices

`KeybombEffect.h` emits geometry through the retained SimpleBatchRenderer.
The following counts come from executing the production templates with a
counting sink, default appearance, lane width 2, and burst age 80 ms. Line
expansion follows SimpleBatchRenderer's four-vertex/two-triangle path; the
short-line circle fallback is also a rectangle for the dimensions probed.
These are primitive counts, not a device benchmark.

| Effect | Vertices | Triangles | Vertex/index bytes |
| --- | ---: | ---: | ---: |
| PGREAT burst | 504 | 212 | 9,336 |
| GREAT burst | 324 | 124 | 5,928 |
| GOOD burst | 352 | 144 | 6,496 |
| BAD burst | 116 | 42 | 2,108 |
| KPOOR burst | 192 | 80 | 3,552 |
| POOR | 0 | 0 | 0 |
| Hold emitter, sampled at 2 seconds | 144 | 48 | 2,592 |

PGREAT consists of two 32-triangle glows, 64 line segments for two rings
(128 triangles), eight two-triangle sparks, and two cross lines (four
triangles). Seven simultaneous PGREAT bursts submit 1,484 triangles; the
count itself is not evidence of a GPU bottleneck.

Concrete opportunities:

- **Skip zero-alpha components before generating geometry.** The probe at
  PGREAT age 379,900 us emitted 212 triangles, all with zero alpha. At age
  190,000 us BAD emitted 42 and KPOOR 80, also entirely transparent. Lifetimes
  expire at 380,000 and 200,000 us respectively, so these calls pass the current
  lifetime guard. Low opacity settings extend invisible tails. Other pieces
  can disappear earlier than the brightest core; test each component's packed
  alpha rather than applying an arbitrary lifetime cutoff. Hold sparks can
  likewise reach zero alpha. SimpleBatchRenderer does not filter transparent
  triangles/lines, and `fs_simple.sc` just outputs vertex color.
- **Share glow-fan vertices.** Each 32-triangle glow currently appends 96
  vertices. An indexed fan can reuse its center and adjacent perimeter samples
  while retaining the same triangles, color interpolation and index order.
  Keep the separately evaluated 2-pi endpoint if exact float compatibility is
  required: 34 vertices then replace 96 per glow, saving 124 vertices / 1,984
  bytes across PGREAT's two glows. Triangle count and blended coverage remain
  unchanged. Do not replace the glow with `addCircle`: that helper has uniform
  color and a small-radius rectangle fallback.
- **Cache fixed-angle samples.** Glow endpoints, ring endpoints and burst
  spark directions recompute sine/cosine from fixed angles every frame. The
  rounded-rectangle cache introduced previously does not cover this code.
  Retain the original float expressions in a shared sample table. This is
  primarily CPU work; caching it alone does not reduce GPU triangles or fill.
  Hold rotation varies with time and needs separate treatment.

Both the burst and hold emitter may be visible on a held long note. Their
overlapping glows are intentional visuals; deleting either is a design change.
Likewise, replacing the rings with connected strips, reducing tessellation or
using textured/shader effects needs visual validation, not just fewer counts.

## Feedback text still replaces whole-string textures

`applyPendingHudText` (BMSRenderer.cpp:4048) updates judgement/combo, score and
optional timing labels on HUD revisions. A changed visible string invalidates
its texture in TextView.cpp:460; `renderImpl` materializes the final value via
`createTexture` (:1250), SDL_ttf/fallback rasterization and
`sdlSurfaceToBgfxTexture`. That copies BGRA pixels and creates a new texture.
Color changes also invalidate the raster. Deferred materialization now avoids
intermediate and hidden uploads, but each final changed visible label still
incurs this work. Unchanged text/color has equality guards.

With combined judgement/combo enabled, each combo increment rerasterizes the
whole label, including unchanged `PGREAT` text. Separate labels avoid that
particular repetition, but the changing digits still get new textures.
Distinct TextView textures also prevent adjacent labels from merging under
UiBatchRenderer's texture-identity batch key. These draw boundaries exist for
steady visible labels too; they are not all newly introduced by input.

A retained digit/glyph atlas for frequently changing gameplay numbers is a
larger follow-up that could remove recurring raster/upload work. Preserve
font choice, fallback glyphs, kerning/shaping, scale, color, alignment and
style invalidation. A cache of every complete combo/score string would grow
with play history and is not an appropriate substitute. CPU rasterization
and GPU resource work must not be conflated with GPU execution milliseconds.

## Paths that already avoid obvious input-dependent amplification

- BMSRenderer.cpp:3774–3831 batches all beams together, then all keybombs
  together with a separate depth. There is no submit per particle or per lane.
  Empty flushes do not submit. These effects use colored geometry, with no
  per-hit texture, blur or offscreen bloom pass.
- Each lane retains only its newest keybomb event. Dense jacks replace it;
  they do not grow a particle queue. Hold geometry is bounded as well.
- Touch markers are two circles per point and skip packed zero-alpha layers
  already (BMSRenderer.cpp:2571). Resolved snapshot consumption and cached
  rounded-corner angles are already implemented. The two overlapping circles
  create the intended appearance; they are not duplicate submissions.
- Judgement timing history is bounded to 96 samples. Each visible marker is
  one quad; zero-alpha markers are skipped. CPU rendering still reads the
  retained samples under a mutex and rebuilds the timestamp-aware average;
  this is distinct from GPU work and preserves out-of-order sample behavior.
- The virtual-controller overlay (GamePlayScene.cpp:601) submits the same
  shapes when pressed and unpressed, changing colors and scratch decoration
  position. It already reuses its batch capacity and layout. Its overlapping
  border/fill shapes are baseline work, not an input-only geometry increase.

Start with packed-alpha culling and indexed keybomb fans, then cover-aware
beam clipping with pixel checks. Cache fixed-angle samples alongside the fan
work for CPU savings. Consider a gameplay numeric atlas separately. None of
these source findings establishes that the entire input-related increase is
waste: visible feedback necessarily adds work.

Only this report changed. Verification was call-chain and draw-state inspection
plus the small production-template counting probe described above; no runtime
instrumentation, application rebuild, deployment or new device timing claim.

## Implemented effect follow-up

The bounded effect optimizations are now implemented:

- Burst/hold components and lane beams skip geometry when their final packed
  alpha is zero. Visible components retain their original fade equations.
- The two PGREAT glow fans share center/perimeter vertices, including the
  separately evaluated seam endpoint. Default PGREAT uses 380 vertices instead
  of 504, saving 1,984 transient vertex bytes per visible burst. Its 212
  triangles and visible coverage are unchanged. Hold glows share vertices too.
- Glow/ring samples and fixed burst-spark directions share immutable tables
  with the original float expressions. Animated hold directions remain dynamic.
- The appearance preview uses the same sampled, indexed fans through its
  existing UI batching and coordinate conversion.
- Active beams retain their original geometry and gradient, with a conservative
  drawable-pixel scissor behind opaque covers. Four projected corners, a
  one-pixel guard and explicit reset protect visible edges and later draws.
  There is no projection work for absent beams or uncovered lanes. Invalid
  projection/viewport bounds fall back to the original unclipped draw.

Regressions reproduced the original invisible-tail submissions and PGREAT
vertex-upload excess. Rendering checks compare indexed fans with the original
triangle stream across batch rollover, and covered beams with uncut references
under top/bottom/overlapping covers, pitched/flat cameras and viewport offsets.
The checks also cover zero-alpha beam uploads and later draws outside the clip.

Non-MSAA fan comparisons and the 41 exported gameplay/preview images are
byte-identical. A new 4x-MSAA stress comparison exposed small differences with
shared indices. Temporary buffer inspection confirmed all 67,200 expanded
vertex records were byte-identical; repeated renders were deterministic, and
matching batch boundaries did not remove the difference. The fan check allows
one channel step for isolated MSAA fans and three for hundreds of overlapping
glows. Beam comparisons remain exact with and without MSAA. Diagnostic buffer
dumping was removed; no production instrumentation remains.

All four focused suites pass: lane effects, built-in Metal characterization,
button-enabled/preview behavior and UI batching. Desktop main and the unsigned
iOS build pass. Independent read-only code review found no correctness issues.

Device GPU-time savings have not been measured, and visible input feedback
still adds rendering work.

## Implemented numeric text follow-up

Changing built-in HUD labels now opt into a shared glyph atlas. SDL_ttf still
supplies glyph selection, kerning and positions; layout metrics and the existing
alignment, shrinking, transforms and clipping remain in TextView. After warming
the digits, changing a supported number or its color reuses coverage textures
and submits glyph quads instead of rasterizing/uploading a new string. Views
using the same live font and generation share pages. Style replacements opt in
again and release their previous text/font resources in order.

Each font atlas is limited to 128 glyphs and four 512×512 BGRA pages (4 MiB at
the limit), allocated on demand and released when its last view goes away.
Only the current layout is retained per view; there is no score/combo history
cache. Append-only slots preserve queued draws. Atlas initialization uses
mutable bgfx textures, since textures created with initial pixel data are
immutable and cannot receive glyph updates.

This is intentionally an opt-in fast path for single-line printable ASCII.
Wrapped, fallback-font, unsupported image-type, edge-touching, overlapping or
capacity-exceeding text uses the existing deferred surface renderer. In
particular, some tightly kerned judgement words still rasterize as strings;
the atlas does not eliminate all judgement texture uploads. Glyph quads also
increase vertex count relative to a single string quad. This change establishes
texture reuse, not a measured overall CPU/GPU or input-latency improvement.

The missing-atlas and gameplay style-replacement regressions failed before
implementation and pass afterward. The Metal fixture compares 120 combinations
across the two HUD fonts, regular/bold weights, colors (including SDL_ttf's
alpha-zero behavior), inherited opacity, rotation, shrinking, and unsupported
text. Maximum permitted channel difference is one 8-bit step. A recording
backend verifies a ten-digit label uses one shared texture submission with
40 vertices, and reordering digits, changing color, a second view, and returning
from wrapped text reuse that texture. Hidden text submits nothing.

All five focused suites pass: text Metal rendering, font lifetime, text input,
built-in renderer characterization, and UI batching. Desktop main and unsigned
iOS builds pass. Independent code review found no actionable issues.
