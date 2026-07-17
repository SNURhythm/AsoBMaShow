# Shared Gameplay Scroll Geometry Design

## Problem

`BMSRenderer` currently uses two positioning strategies. Replay ghosts and miss
markers use chart scroll coordinates, while ordinary timelines switch to a
constant-speed interpolation after their timing passes the judge line. That
late-window interpolation was a temporary workaround. It ignores stops and
`#SCROLL` changes, so late note geometry can move differently from the replay
geometry for the same chart.

Timeline visuals also calculate or gate their positions in several separate
branches. Notes, long-note endpoints, measure lines, landmines, and invisible
notes can therefore diverge even though they belong to the same timeline.

## Goal

Use one chart-scroll-to-render-Y transform for all gameplay timeline geometry
and replay geometry. Hide timeline visuals after they pass the judge line,
while preserving the visible body of an active long note whose head has
already passed.

## Shared Geometry

Introduce a small, pure gameplay scroll geometry helper. Given an item's
precomputed chart scroll position, the current chart scroll position, the
render scale, and the judge-line Y coordinate, it returns:

```text
judgeY + (itemScrollPosition - currentScrollPosition) * rxhs
```

`BMSRenderer` will use this helper for:

- normal note positions;
- long-note head and tail positions;
- measure lines;
- landmines;
- invisible notes;
- replay ghost outlines; and
- replay miss markers.

Timeline items obtain `itemScrollPosition` from the existing
`timelineScrollPositions` cache. Replay items continue to retain the scroll
position captured by `ReplayGhostUtils`. `scrollPositionAtTime` remains the
source of the current scroll coordinate, including interpolation across BPM
segments and stops.

The helper will live at a lightweight, renderer-independent boundary so its
math can be tested without constructing bgfx or a full `BMSRenderer`.

## Visibility and Lifecycle

Ordinary notes, measure lines, landmines, and invisible notes render only while
their timeline has not passed the current render time and their transformed
geometry intersects the visible lane at or above the judge line. This matches
the replay ghost lifecycle: scroll coordinates determine position, while the
item's timing prevents past geometry from re-entering during unusual scroll
sequences.

The constant-speed `latePoorTiming` render branch will be removed. Judgement
windows remain gameplay logic; they will no longer control how far a visual
travels below the judge line.

Long notes are the one lifecycle exception. If a long-note head has passed but
its tail is still current or future, the body remains visible from the judge
line to the tail's shared-geometry Y position. The passed head endpoint is not
drawn. Once the tail passes, the body and tail are removed normally. Played,
dead, and malformed long notes retain their existing safety checks.

Geometry above the lane or below the judge line is culled before adding render
batches. Negative-scroll and stop sequences still use their real chart scroll
coordinates; no constant-speed fallback or clamping changes their position.

## Scope

The change is limited to gameplay render geometry and its focused tests. It
does not alter judgement windows, note state transitions, replay data formats,
chart parsing, lane-cover settings, or playback timing.

## Testing

Add a lightweight regression-test target covering the pure geometry and
visibility rules:

- identical item/current scroll positions map to the judge line;
- positive and negative scroll deltas map symmetrically around the judge line;
- changes in current scroll coordinate, including a stopped coordinate, drive
  item movement instead of elapsed late-window time;
- passed timeline geometry is rejected at the judge line;
- future geometry outside the visible lane is rejected; and
- an active long-note body anchors its passed head at the judge line while its
  future tail keeps the shared transformed position.

After the focused regression test passes, run the existing desktop compile
check with `cmake --build cmake-build-debug --target main -j 6`.
