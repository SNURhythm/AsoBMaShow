# Judgement Timing Millisecond Display Design

## Goal

Display gameplay FAST/SLOW timing magnitudes in milliseconds with exactly two
decimal places, rounding upward whenever the microsecond value is not already
on a hundredth-of-a-millisecond boundary.

The direction label, timing criteria, colors, and lingering behavior remain
unchanged.

## Display Rule

FAST and SLOW use the absolute timing magnitude. Ten microseconds equal one
hundredth of a millisecond, so the displayed hundredths are calculated with
integer ceiling division:

```text
hundredths = absolute_microseconds / 10
           + (absolute_microseconds % 10 != 0)
```

The result is rendered with a two-digit fractional part and the existing `ms`
suffix.

Examples:

- `12,340 us` becomes `12.34ms`.
- `12,341 us` becomes `12.35ms`.
- `1 us` becomes `0.01ms`.
- `-12,341 us` also becomes `12.35ms`; FAST/SLOW continues to communicate the
  sign separately.

## Implementation Boundary

Add a small header-only formatter beside the gameplay timing code under
`src/scene/play`. It accepts a signed microsecond difference and returns the
complete magnitude label. `BMSRenderer` delegates only the numeric timing text
to this formatter.

Integer arithmetic is preferred over floating-point `ceil` and stream
rounding. It represents the source microseconds exactly, avoids floating-point
edge cases, and keeps the formatting logic independently testable without
constructing the renderer.

The magnitude calculation must be valid for every `long long` input, including
the minimum signed value. Ceiling division must avoid adding to the magnitude
before division so the maximum magnitude cannot overflow.

## Testing

Extend the focused gameplay timing test target with checks for:

- positive and negative inputs producing the same magnitude;
- an exact hundredth remaining unchanged;
- a remainder rounding upward;
- a sub-hundredth nonzero value displaying as `0.01ms`; and
- zero displaying as `0.00ms` when the formatter is tested directly.

The production renderer continues to suppress timing text for a zero timing
difference, so the zero formatter result does not change gameplay visibility.

## Out of Scope

- Changing FAST/SLOW direction rules or colors.
- Changing which judgements show direction or millisecond feedback.
- Changing timing calculations, playback-rate conversion, or stored replay
  precision.
- Changing result-screen FAST/SLOW counters.
