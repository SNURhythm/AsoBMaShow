# Gimmick Chart Render Traversal Design

## Problem

`BMSRenderer` computes each timeline's Y coordinate from chart scroll geometry,
but its render loop still stops at the first timeline whose Y coordinate reaches
the top of the visible lane. That stopping rule assumes timeline scroll
positions increase monotonically with timeline order.

Aggressive `#SCROLL` charts violate that assumption. A future timeline can be
far above the visible lane while a later timeline reverses direction and
returns to the lane. Stopping at the first offscreen timeline hides every
subsequent re-entry.

The supplied `_00_strange_labyrinth.bms` is the acceptance case. It combines
BPM values of `14500145` and `145000145`, scroll multipliers ranging from
`-10000` to `20000`, and scroll channels with as many as 9,600 subdivisions in
one measure. Its timeline geometry repeatedly leaves and re-enters the visible
lane, so the current early exit produces incorrect rendering throughout most
of the chart.

PR #36 attempted to solve this with non-monotonic traversal and bounds
culling. It also replaced the renderer's late-note and long-note lifecycle
logic. Those bundled lifecycle changes conflict with the current requirement
that alive normal and long notes remain visible after passing the judge line
and with newer long-note behavior.

## Goal

Allow future timelines to leave and later re-enter the visible lane under
negative or very large `#SCROLL` and huge BPM values, without regressing the
current late-note, long-note, measure-line, landmine, or invisible-note
lifecycle behavior.

## Selected Approach

Port only the traversal and geometry-safety parts of PR #36.

`buildTimelineScrollPositions` will retain its existing per-timeline scroll
coordinates and additionally build suffix minimum and maximum arrays. For an
index `i`, these arrays describe the scroll-coordinate range reachable by any
timeline at or after `i`.

At render time, convert the visible Y interval into a visible scroll-coordinate
interval using the current scroll position and `rxhs`. The lower edge includes
one note height so partially visible notes are retained. The timeline loop will
no longer stop merely because the current timeline is above the lane. It may
stop only when the suffix range proves that no remaining timeline can intersect
the visible scroll interval and no open long-note lookahead still needs a tail.

This is deliberately a conservative early-exit test. It can scan extra
timelines when suffix extrema straddle the visible range, but it cannot skip a
later re-entry. It keeps the change close to the previously reviewed PR and
avoids a larger position-index rewrite.

## Geometry and Visibility

Every processed timeline continues to obtain Y from the shared transform:

```text
judgeY + (timelineScrollPosition - currentScrollPosition) * rxhs
```

Normal notes, invisible notes, and landmines will reject draw submissions only
when their rectangle is entirely outside the renderer's lower and upper
bounds. The lower bound is the viewport boundary, not the judge line, so alive
normal notes remain renderable after crossing the judge line.

Long-note bodies will keep the current lifecycle and texture-selection code. A
non-positive body height will be rejected before submitting geometry, which
prevents reverse or extreme scroll coordinates from producing invalid body
rectangles. Long-note heads and tails retain their existing behavior.

Measure lines continue to use `shouldDrawMeasureLine`: a line is rendered only
when its timeline is current or future and its transformed Y is between the
judge line and the visible upper bound. Passed measure lines remain hidden even
if unusual scroll geometry would bring them back.

## Lifecycle Preservation

The following current behavior remains authoritative:

- `latePoorTiming` determines how long an alive normal or long note remains in
  the renderer's processing window;
- `BMSRendererState::orphanLongNotes` and `longNoteLookaheadScratch` preserve
  long-note bodies whose heads have passed or resolved;
- played/dead state continues to determine normal and long-note retirement;
- invisible notes and landmines continue to expire in their existing timeline
  branches; and
- `currentTimelineIndex` remains a timing/lifecycle cursor rather than a visual
  position cursor.

The earlier millisecond render-time quantization experiment is not restored.
The current microsecond clock and `scrollPositionAtTime` remain the timing
authority, including for huge-BPM timelines that collapse near one another.

## Alternatives Considered

### Cherry-pick PR #36

Rejected. The old commit is based on a substantially older renderer and would
reintroduce removal of the late window and replacement of long-note lifecycle
state. Resolving that patch would be noisier and riskier than applying its
small surviving concepts directly.

### Build a scroll-position index

Rejected for this change. Sorting or spatially indexing timelines could make
visible-row lookup more exact, but note expiration, timeline ordering, measure
lines, and long-note pairing would then require separate passes. That is a much
larger renderer redesign than the observed bug requires.

## Testing

Extend the renderer-independent gameplay scroll geometry tests before changing
production code. A compact synthetic sequence will model the acceptance chart:
an early future timeline is far above the upper bound, followed by negative and
positive scroll changes that return a later timeline to the visible interval.
The test must fail under the current first-offscreen stopping rule and pass only
when suffix reachability is available.

Focused tests will cover:

- construction of suffix minimum and maximum values;
- a later visible re-entry after an offscreen huge positive position;
- a later visible re-entry after a negative position;
- rejection when the remaining suffix range cannot touch the visible range;
- conversion from render bounds to scroll-coordinate bounds;
- note rectangles that are partially visible below or above a boundary; and
- the existing rule that passed measure lines stay hidden.

The attached chart will be used as the full acceptance input without adding the
entire third-party chart to the repository. Final verification will build the
desktop target, run the focused test, run the complete CTest suite, and check
the resulting diff.

## Scope

This change is limited to renderer traversal, draw bounds, focused pure helpers,
and regression tests. It does not change the BMS parser, chart timing,
judgement, input, replay formats, lane-cover configuration, note expiration,
or audio playback.
