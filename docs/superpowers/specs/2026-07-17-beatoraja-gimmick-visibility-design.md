# Beatoraja-Compatible Gimmick Visibility Design

## Status

This design supersedes the suffix-re-entry traversal in
`2026-07-17-gimmick-chart-render-traversal-design.md`. The user authorized the
correction to proceed without another approval checkpoint after diagnosis.

## Problem

The suffix-re-entry renderer searches every later chart timeline whenever any
later absolute scroll position could return to the viewport. That behavior is
geometrically conservative, but it is not compatible with Beatoraja. On
`_00_strange_labyrinth.bms`, distant note groups repeatedly share or revisit
visible absolute scroll positions, so the suffix search submits far too many
notes at once.

Beatoraja uses a different visibility model:

- it removes timelines that contain only background/BGA work;
- it advances future Y incrementally through the retained timeline sequence;
- it stops both measure-line and note traversal after the first Y value above
  the lane top; and
- equal-microsecond rows produced by enormous BPM values yield a non-finite
  increment, which also ends the forward traversal because the loop condition
  is no longer true.

The supplied chart has 23,348 parsed timelines. Beatoraja's retention rule
keeps 23,109 and drops 239, including 238 BGA-only rows. Among the retained
rows, 18,732 adjacent pairs have zero travel duration. The current suffix
search deliberately crosses those boundaries; Beatoraja does not.

## Goal

Match Beatoraja's bounded future-content visibility on charts with negative or
huge `#SCROLL` and huge BPM, while preserving AsoBMaShow's current requirement
that alive normal and long notes continue along chart scroll geometry after
crossing the judge line.

## Approaches Considered

### 1. Keep absolute geometry and restore only the first-top exit

This removes the overdraw with the smallest patch and keeps replay ghosts
aligned. It is not sufficiently compatible: sampled traversal on the supplied
chart differs from Beatoraja by as many as 1,455 retained timelines because
Beatoraja's incremental formula and zero-duration boundaries stop much sooner.

### 2. Port Beatoraja's renderer wholesale

This gives the closest future-note behavior, but Beatoraja normally hides past
normal notes. A wholesale port would undo the late-note behavior explicitly
requested for AsoBMaShow and would replace newer long-note lifecycle handling.

### 3. Use a split future/past geometry model

Selected. Future timelines use Beatoraja's retained sequence, incremental Y,
and first-top stopping rule. Past timelines that remain inside the existing
late lifecycle use the absolute chart-scroll transform already shared with
ghost rendering. This isolates compatibility behavior to future visibility and
preserves the recently fixed late-note behavior.

## Timeline Retention

The renderer will retain a timeline when at least one of these is true:

- BPM differs from the preceding model timeline;
- stop duration is positive;
- scroll differs from the preceding model timeline;
- the timeline starts a measure;
- it contains a playable note or long-note endpoint;
- it contains an invisible note; or
- it contains a landmine.

Previous BPM and scroll values are updated for every model timeline, including
discarded rows, matching Beatoraja. The retained timeline list and grouped note
list remain index-aligned. Absolute scroll positions are built from the
retained list; omitting BGA-only rows with unchanged timing state does not alter
the integrated path.

## Rendering Data Flow

Each render begins with the existing absolute current scroll position. The
timeline loop handles two regions:

1. Past timelines use
   `renderY(timelineScrollPosition, currentScrollPosition, rxhs, judgeY)`.
   Existing `latePoorTiming`, dead/played state, invisible expiration,
   landmine expiration, and orphan-long-note state remain authoritative.
2. Future timelines start from `judgeY` and advance with Beatoraja's retained
   predecessor formula. The first retained row has no predecessor, so it is
   seeded from the shared absolute transform; this preserves ordinary-chart
   movement and ghost alignment during negative preroll. Before processing the
   next future row, traversal ends when the previous future Y no longer
   satisfies `y <= upperBound`. This exact comparison also stops on `NaN` and
   positive infinity while retaining Java's behavior for negative infinity.

A zero-duration incremental segment returns `NaN` explicitly. This documents
the compatibility boundary instead of relying on an incidental C++
divide-by-zero expression. The current row is still processed after its Y is
calculated, as in Beatoraja; viewport tests reject its non-finite geometry, and
the next iteration terminates.

Measure lines use only future incremental Y and keep their existing timing and
judge-line visibility rule. Normal, invisible, and landmine rectangles retain
viewport culling. Alive past normal notes and long notes remain eligible below
the judge line until their existing lifecycle retires them.

## Long Notes

The existing lookup and orphan-long-note state remain in place. A past head is
seeded with its absolute late-note Y; a future head or tail uses incremental Y.
If traversal ends at the finite lane top before a tail is encountered, the
existing leftover-body behavior clips the open body to the top. If traversal
ends on a non-finite compatibility boundary, newly encountered future geometry
is not submitted.

No judgement, hold-state, charge-note, or early-release behavior changes.

## Replay Ghosts

Replay ghost and miss-marker data continue using absolute chart-scroll
geometry. This change is intentionally limited to chart timeline visibility;
it does not change replay data formats or event indexing. Normal charts remain
mathematically aligned, while extreme compatibility charts favor Beatoraja's
authored note visibility.

## Testing

Renderer-independent tests will define:

- Beatoraja-compatible retention of timing, section, playable, invisible, and
  landmine timelines and rejection of BGA-only rows;
- incremental future Y during ordinary travel and stops;
- explicit non-finite Y for a zero-duration retained pair;
- termination at the first value above the lane top even when later scroll
  values would return; and
- termination on `NaN` while preserving the direct `y <= upperBound`
  comparison semantics.

The existing late-note geometry, measure-line visibility, and viewport tests
remain. Final verification uses the focused test, the complete CTest suite, the
desktop target, and a diagnostic parse of the supplied chart.

## Scope

This change is limited to timeline retention, future render traversal, pure
geometry helpers, and tests. It does not modify the parser, judgement,
expiration deadlines, replay formats, input, audio, lane-cover configuration,
or the supplied chart.
