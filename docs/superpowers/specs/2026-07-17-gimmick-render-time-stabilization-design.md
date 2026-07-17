# Gimmick Render-Time Stabilization Design

## Status

This design supersedes
`2026-07-17-raw-gimmick-render-time-design.md`. The user clarified that the
apparent Labyrinth rendering regression was caused by the chart requiring a
specific gameplay setup, including lane-cover length, rather than by
millisecond render-time sampling. The user explicitly requested that the
stabilization be restored and that chart-setup handling remain out of scope.

## Goal

Stabilize extreme-BPM and extreme-`#SCROLL` chart geometry by sampling the
chart-render clock on whole-millisecond boundaries, while preserving raw
microseconds for non-chart effects.

## Approaches

### Quantize the clock in `GamePlayScene`

Rejected. This would expose the rounded value to consumers beyond chart
geometry and risks reducing HUD, judgement-effect, or replay-touch precision.

### Restore the earlier raw/geometry time structure

Viable, but unnecessary. The renderer now has a dedicated
`chartRenderTimeMicros` policy boundary and already keeps effect and touch
clocks separate.

### Quantize the existing chart-time policy

Selected. `chartRenderTimeMicros` truncates visual time to a whole millisecond
using signed integer division:

```cpp
(visualTimeMicros / 1'000LL) * 1'000LL
```

This matches Java truncation toward zero during negative preroll. Every chart
geometry, traversal, visibility, renderer-side lifecycle, and replay-ghost
consumer already uses this policy. `currentRenderMicros`, pending HUD text,
lane beams, judgement effects, and `replayTouchTimeMicros` remain raw.

## Testing

The existing raw-time assertions will first be changed to require:

- positive sub-millisecond values collapse to their containing millisecond;
- values within one millisecond share one chart sample;
- negative preroll truncates toward zero; and
- exact millisecond values remain unchanged.

The test must fail against the current identity policy before the helper is
changed. Final verification uses the focused geometry test, desktop build,
the supplied Labyrinth diagnostic, the complete CTest suite, and independent
review.

## Scope

This change only restores chart-render time stabilization. It does not inspect
or enforce lane-cover length, chart options, or other setup requirements. It
does not modify parser, audio, input, judgement, replay formats, or the bounded
gimmick traversal behavior.
