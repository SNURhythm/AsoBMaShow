# Raw Gimmick Render-Time Restoration Design

## Status

This design supersedes
`2026-07-17-beatoraja-render-time-quantization-design.md`. The user reported
that the millisecond stabilization caused parts of `_00_strange_labyrinth.bms`
to render incorrectly and had already authorized corrections to proceed after
diagnosis without another approval checkpoint.

## Problem

The renderer-time quantization change assumed that replacing the raw visual
clock with a whole-millisecond clock would only stabilize note coordinates.
That assumption was wrong. The same time value controls:

- whether a timeline is future or past;
- which incremental-Y branch handles a stop or travel segment;
- the Y value used by the first-top traversal exit;
- whether a zero-duration segment forms a non-finite traversal boundary; and
- note, measure-line, invisible-note, landmine, and replay-ghost visibility.

On the supplied chart, comparing raw and millisecond clocks across 8,251
60-Hz samples changes visible note membership on 76 frames and changes the
visible count on 72 frames, by as many as 21 notes. This is content selection,
not harmless position rounding.

The initial Beatoraja comparison was also incomplete. Both parsers produce
23,348 timelines with matching section, BPM, scroll, and note-bearing flags,
but their accumulated integer timing differs on 19,110 timelines. The maximum
timestamp difference is 151 microseconds, and 208 stop durations differ.
Quantizing AsoBMaShow's current clock to Beatoraja's millisecond grid while
retaining AsoBMaShow's independently accumulated chart timestamps therefore
mixes two timing grids. Extreme BPM and scroll magnify those small differences
into different traversal boundaries.

## Goal

Restore the correct Labyrinth content and traversal behavior that existed
before renderer-time quantization, without changing gameplay, parser, replay,
or note-lifecycle semantics.

## Approaches Considered

### Special-case zero-duration traversal after quantization

Rejected. The regression is not limited to zero-duration rows. Ordinary huge
scroll segments can move across the lane boundary within a fraction of a
millisecond, so patching only non-finite segments would leave content changes.

### Use raw time for traversal and quantized time for drawing

Rejected. This creates two Y paths that disagree about stops, non-finite
boundaries, viewport intersection, and long-note endpoints. A row selected by
one path may have invalid or offscreen geometry on the other, recreating the
same missing-content failure in a less obvious form.

### Restore one raw chart-render clock

Selected. `BMSRenderer` will again use the raw visual microsecond clock for
scroll integration, future/past classification, incremental Y, traversal,
measure lines, renderer-side lifecycle checks, and replay geometry. HUD,
animations, beams, judgement effects, and replay touches remain unchanged.
The quantization helper and its contract tests will be removed.

## Data Flow

`GamePlayScene` continues to pass:

1. visual chart time in microseconds as the renderer's chart clock; and
2. gameplay time in microseconds as the independent replay-touch clock.

`BMSRenderer` uses the chart clock directly throughout chart traversal. It
does not round, truncate, or otherwise translate it. `replayTouchTimeMicros`
remains independent.

## Testing

Before changing production code, the geometry test will be changed to require
that a sub-millisecond chart-render time remains exact. It will fail against
the current quantization helper. The minimal implementation will make the
clock contract preserve the raw value, after which the renderer will be
simplified to use its input directly and the obsolete helper will be removed.

Acceptance checks are:

- a red/green focused regression cycle;
- the supplied-chart diagnostic showing no raw-versus-restored content delta;
- the desktop `main` target;
- the complete CTest suite; and
- a clean diff and independent code review.

## Scope

This correction restores pre-quantization render timing only. It does not edit
the parser amalgamation, change judgement or audio timing, add chart assets,
or attempt a replacement anti-shake algorithm. Any future stabilization must
operate on final spatial presentation and prove that it does not change the
traversed or visible chart rows.
