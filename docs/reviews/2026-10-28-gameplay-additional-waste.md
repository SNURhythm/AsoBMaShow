# Additional gameplay source findings

Inspected at `df303fcb`, after deferred HUD texture materialization. No new
metrics or application changes. These are source-confirmed repeated operations;
their device frame-time impact is not measured. The prior long-note map finding
remains valid but is not counted as a new discovery.

## 1. Virtual-controller rendering discards its batch buffers every frame

`GamePlayScene.cpp:600`, `renderVirtualControllerOverlay`, constructs a local
`rendering::SimpleBatchRenderer` after validating the layout. The caller in
`renderScene` invokes it every rendered manual-play frame when the realtime
touch router is available. The constructor reserves 4,096 `PosColorVertex`
elements and 6,144 uint16 indices (SimpleBatchRenderer.cpp:17). At 16 bytes per
vertex that requests at least **77,824 bytes (76 KiB)** of vector capacity per
valid overlay invocation. Both vectors are freed at function exit. This is
allocation capacity, not bytes uploaded or net resident-memory growth.

The main renderer already retains its batches. Give the overlay an instance
owned by the scene, and reset its batch state with `begin()` before drawing.
Keep ownership on the rendering thread; avoid a process-global static that
would mix scene/export lifetimes. Invalid/disabled controller layouts return
before the local batch is constructed, so this applies to valid enabled layouts.

The same caller rebuilds `VirtualControllerLayout` from unchanged configuration,
key mode and UI dimensions each frame. Its builder allocates lane and element
vectors. Cache layout with those inputs; pressed colors and platter rotation
remain dynamic and should not force a layout rebuild. Buffer retention is the
smaller first change, independent of a geometry cache.

## 2. Scalar Hi-Speed/green-number calculations allocate lane-order vectors

`BMSRenderer::builtInProjectionTraversal` (BMSRenderer.cpp:2184) always reserves
and populates a new `playableLaneOrder` vector. Actual projection needs it, but
the ordinary `renderFrame` caller reads only `hispeed`, `noteVisibleUpperBound`
and `rxhs`. `effectiveVisibleTimeGreenNumber` and `laneCoverVisibleTimeLabel`
only read `configuredHispeed`.

In milliseconds mode, `laneCoverVisibleTimeLabel` first computes a green number
that it never uses, then repeats the duration calculation. When the label is
laid out this can build the lane-order vector twice just for its displayed
number, in addition to the ordinary rendering call.

Factor scalar speed/traversal geometry calculation away from projection's
owned lane list. Compute only the selected number format. Preserve one source
of truth for speed, lane cover, playback rate and scroll multipliers; do not
duplicate the formulas. This is small bounded work but plainly unnecessary.

## 3. Full touch-layout construction just to compare unchanged lane order

`GamePlayScene::refreshGameplayPresentationGeometry` (:2185) calls
`presentation->touchLayout().lanes` every built-in render with an input handler.
`BMSRenderer::touchLayout` (:4316) projects four corners, builds a lane vector
and a scratch vector, and returns the full structure. The consumer,
`RhythmInputHandler::setTouchLaneOrder` (:472), then immediately returns if the
lane order is unchanged. Its equality guard is too late to avoid the upstream
work. The earlier realtime guard in `refreshLegacyTouchLayout` does not guard
this separate call.

Use the existing layout revision plus presentation identity to skip this
legacy refresh, or expose the lane order without constructing all geometry.
Preserve first initialization, skin/built-in transitions, scratch hiding and
lane-side changes. A revision value alone must not accidentally reuse state
across two presentation instances. The separate realtime layout refresh has
its own input requirements and should not be removed wholesale.

## 4. Start-lane lists are recopied and normalized while hidden

Every captured authority copies `preparationPlan.laneIndicator.lanes`.
`PlayfieldPresentationCoordinator::prepareFrame` copies it into its pending
frame even for built-in presentation, although that pending copy is consumed
only by the selected-skin overlay. `BMSRenderer::prepareFrame` separately calls
`setStartLaneIndicators`, passing the vector by value; the setter sorts,
deduplicates and replaces its retained list unconditionally. This runs before
setting the visibility flag and continues after the indicators are hidden.

Keep a canonical retained list, update only on list changes, and avoid the
skin-only pending copy in built-in mode. Do not merely discard hidden lists:
they must still be correct if preroll/practice/restart makes them visible again.

## Lower priority and rejected candidates

- Built-in preparation clears and reconstructs its touch-visualization maps
  from captured touches even when visualization is disabled. Drawing alone
  checks `touchVisualizationEnabled`. Skip/reuse that visual representation
  with explicit toggle/reset handling; do not suppress judgement input or
  replay capture. Cost is bounded by captured touches.
- Authority capture repeatedly builds/copies maps, static names and course
  titles, including skin metadata in built-in play. A lean built-in snapshot
  or immutable shared metadata can reduce this, but requires a broader snapshot
  contract change. Existing capture timings were small; prioritize the narrow
  work above.
- The coordinator allocates owned failure diagnostics in successful frames.
  This is intentional preflight for allocation-safe failure handling, including
  skin identity. Do not lazily allocate those diagnostics inside an already
  failing submission; any reuse needs to preserve that guarantee.
- `SyntheticReplayGhostFrameInput::events` is a span, so assigning the replay
  event collection into it is **not** a per-frame full replay copy.
- `updateSkinGameplayGraph` returns immediately under realtime authority; it
  does not repeat that legacy graph update during normal realtime play.
- `visibleTimeReferenceBpm` can recompute the entire chart BPM summary, but
  normal gameplay supplies `configuredHispeed` and bypasses that fallback in
  its traversal calculation. Do not claim this as the active-play bottleneck.

Recommended next implementation: retain the virtual-controller batch, then
remove lane-vector creation from scalar speed/label calculations. Source and
diff review suffice for this documentation-only investigation; no runtime
performance improvement is claimed and no tests/builds were rerun.
