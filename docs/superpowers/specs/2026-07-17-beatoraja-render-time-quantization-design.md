# Beatoraja Render-Time Quantization Design

## Status

The user authorized this follow-up correction after reporting subtle shaking in
the otherwise-correct labyrinth rendering. This design extends the existing
Beatoraja-compatible gimmick visibility work without changing chart parsing or
gameplay timing.

## Problem

`BMSRenderer` currently receives the audio visual clock in microseconds and
uses every sub-millisecond change when calculating chart scroll geometry.
Beatoraja's lane renderer receives a millisecond clock and multiplies it by
1,000 before doing its otherwise-double-precision geometry calculations. Its
lane positions therefore change only on whole-millisecond boundaries.

On `_00_strange_labyrinth.bms`, a 60 Hz diagnostic comparing the two clocks
found 76 frames with different visible note sets and position differences of
up to 1.91 lane units. Extreme BPM and `#SCROLL` values magnify a time delta of
less than one millisecond into a visible displacement, producing the reported
shake.

## Goal

Sample all chart visibility and lane geometry on the same whole-millisecond
clock as Beatoraja while keeping AsoBMaShow's raw microsecond clock for visual
effects that are unrelated to chart traversal.

## Approaches Considered

### Quantize the global gameplay clock

This would make lane geometry stable, but it would also reduce the precision of
audio synchronization, input judgement, replay touch effects, and other
gameplay consumers. The problem is renderer-local, so a global clock change is
too broad.

### Change lane calculations from `double` to `float`

This could mask some movement through rounding, but it would not match
Beatoraja: its timing and Y calculations use `double`. It would also make the
result depend on magnitude instead of defining a stable time sampling rule.

### Split raw and geometry render times

Selected. A pure helper returns both the unchanged input time and a geometry
time truncated to a whole millisecond. `BMSRenderer` uses the geometry time for
timeline classification, scroll integration, future traversal, note and
measure-line visibility, renderer-side expiration, and replay ghost geometry.
It uses the raw time for long-note texture animation, pending HUD updates, lane
beams, and the judgement indicator. Replay touch rendering keeps its existing
separate raw clock.

## Quantization Semantics

The geometry clock is calculated as:

```cpp
(rawMicros / 1'000LL) * 1'000LL
```

C++ signed integer division truncates toward zero, matching Java's behavior
for negative preroll times as well as normal positive playback. The renderer
does not round to the nearest millisecond because Beatoraja receives an integer
millisecond value.

## Testing

A pure regression test will establish that:

- positive sub-millisecond input truncates to the containing millisecond;
- negative preroll input truncates toward zero, matching Java;
- the raw clock remains unchanged beside the quantized geometry clock; and
- exact millisecond values remain unchanged.

The focused geometry test, complete CTest suite, desktop build, and supplied
chart diagnostic will verify the integration. No parser files or chart assets
are modified.
