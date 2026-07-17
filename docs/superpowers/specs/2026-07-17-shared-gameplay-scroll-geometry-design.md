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

Use the replay ghost chart-scroll-to-render-Y transform for normal and long
notes and for measure lines. Normal and long notes remain visible after they
pass the judge line for as long as their existing renderer lifecycle keeps
them alive. Measure lines disappear after passing the judge line.

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
- replay ghost outlines; and
- replay miss markers.

Landmines and invisible notes already share their containing timeline's Y
coordinate, so they inherit the corrected transform without new lifecycle
logic. Their existing expiration behavior remains authoritative.

Timeline items obtain `itemScrollPosition` from the existing
`timelineScrollPositions` cache. Replay items continue to retain the scroll
position captured by `ReplayGhostUtils`. `scrollPositionAtTime` remains the
source of the current scroll coordinate, including interpolation across BPM
segments and stops.

The helper will live at a lightweight, renderer-independent boundary so its
math can be tested without constructing bgfx or a full `BMSRenderer`.

## Visibility and Lifecycle

The timeline renderer continues to use its existing `latePoorTiming` window
and note state for lifecycle decisions. A normal or long note that has crossed
the judge line is still drawn when it is alive; only its Y position changes
from elapsed-time interpolation to the shared chart-scroll transform. Played,
dead, active, released, and malformed long notes retain their current state
handling.

Measure lines use the same transformed timeline Y but render only while their
timeline timing is current or future and their anchor is within the visible
lane at or above the judge line. This prevents a passed measure line from
re-entering during unusual scroll sequences.

Landmines and invisible notes receive the corrected timeline Y automatically.
No new visibility gate is added for them: their existing expiration branches
continue to hide or retire them.

Negative-scroll and stop sequences use their real chart scroll coordinates;
the post-judge constant-speed Y fallback is removed without removing the
existing late-note lifecycle window.

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
- a passed normal or long note retains its shared transformed Y rather than
  being hidden or moved by elapsed late-window time;
- a passed measure line is rejected;
- future measure-line geometry outside the visible lane is rejected; and
- landmine and invisible-note expiration code remains unchanged.

After the focused regression test passes, run the existing desktop compile
check with `cmake --build cmake-build-debug --target main -j 6`.
