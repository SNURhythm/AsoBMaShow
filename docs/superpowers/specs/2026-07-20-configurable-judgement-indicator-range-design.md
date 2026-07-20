# Configurable Judgement Indicator Range

**Date:** 2026-07-20

**Status:** Approved design

**Implementation branch:** `feature/bokutachi-ir`

## Context

The judgement timing indicator currently derives one symmetric display range
from every available judgement window. LR2's early KPoor window extends to
-1000 ms, so an LR2 indicator uses a +/-1000 ms scale. That scale preserves the
full miss window, but compresses the timing information players use most:
PGREAT, GREAT, GOOD, and the nearby part of BAD.

The display range is a presentation preference rather than a gameplay rule.
Changing it must not change judgement results, recorded replay timing, score
verification, timing statistics, or the arithmetic used for the average
marker.

## Goals

- Make the judgement indicator's symmetric timing range configurable.
- Use +/-180 ms by default so all standard LR2 GOOD windows and some BAD range
  remain visible.
- Accept any positive whole-millisecond value through a hard cap of 1000 ms.
- Clamp rendered markers and judgement-window segments without changing their
  underlying timing values.
- Keep the setting consistent across gameplay, settings preview, replay video
  export, and profile persistence.

## Non-goals

- Changing gameplay judgement windows or timing calculations.
- Adding separate FAST and SLOW ranges.
- Adding a dynamic range that changes with the chart rank, ruleset, or judge
  scale.
- Displaying numerical timing values on the indicator itself.
- Expanding the supported maximum beyond the existing LR2 KPoor-scale view.

## Chosen approach

Add one persisted, symmetric `Judgement Indicator Range` value expressed in
milliseconds. The renderer receives the configured value and uses it as the
only horizontal display extent. It no longer derives the extent from the
widest judgement window.

A fixed configured scale is preferred to an automatically selected GOOD
window because a fixed timing error should appear in the same place across
charts and judge ranks. A free numeric value is preferred to presets because
the user explicitly wants every positive range through the hard cap to remain
available.

## Settings model and persistence

Add an integer setting for the indicator range with these constants:

- default: `180` ms;
- minimum meaningful value: `1` ms;
- hard maximum: `1000` ms.

The setting is sanitized with the rest of `AppSettings`. Missing values in old
settings files use 180 ms. Zero, negative, malformed, or non-finite input is
replaced by the default where applicable; parsed integers above 1000 are
clamped to 1000. Persisted output always contains the sanitized whole-number
value.

This is a per-profile display preference, matching the existing judgement
indicator enabled, position, width, and render-mode settings.

## Settings UI

Add `Indicator Range` beside the existing judgement-indicator controls. The
summary and detailed control display the value as `+/-180 ms` (rendered with
the UI's established plus/minus typography where supported).

The detailed control follows the existing numeric indicator-control pattern:

- `-10` and `-1` buttons;
- a numeric text input;
- `+1` and `+10` buttons;
- a Reset action that restores 180 ms.

Button and text-input changes are sanitized to 1 through 1000 ms, saved using
the existing settings flow, and reflected by the gameplay preview immediately.
No preset-only selector is introduced.

## Rendering behavior

Convert the configured millisecond value to microseconds before mapping
timing offsets to horizontal positions. The display spans exactly the selected
negative range at the left edge and selected positive range at the right edge.
Zero remains centered.

For individual judgement samples:

1. retain the original recorded timing difference;
2. map it against the configured display range;
3. clamp only the rendered position to the left or right edge.

For the average marker:

1. include the same samples as today;
2. sum and average their original, unclamped timing differences;
3. map that raw average against the configured display range;
4. clamp only the final rendered position to the indicator edge.

This distinction prevents an outlier from being reduced to the configured
boundary before it contributes to the average. For example, samples of
+300 ms and 0 ms average to +150 ms, not +90 ms under a +/-180 ms display.

Judgement-window background segments use their real window boundaries and are
visually intersected with the configured range. Segments entirely outside the
range are omitted. Partially visible BAD or other segments end at the bar
edge. The underlying timing windows are not modified.

The same range is supplied to live gameplay, the settings preview renderer,
and replay video export so all three presentations agree.

## Compatibility and failure handling

Older profiles need no explicit migration: absence of the new field resolves
to 180 ms and the next normal save persists it. The previous broad view remains
available by setting the range to 1000 ms.

If a renderer is ever given an invalid range despite settings sanitization, it
uses the 180 ms default. This avoids division by zero and keeps rendering
deterministic.

## Verification

Automated coverage will verify:

- default, serialization, deserialization, and old-settings fallback;
- sanitization at 1 ms and 1000 ms, including invalid and excessive input;
- settings-label formatting and numeric control conversion;
- renderer mapping at zero, both boundaries, and values beyond both
  boundaries;
- average computation from raw samples followed by final-position clamping;
- judgement-window segment clipping at the configured extent;
- propagation to gameplay, preview, and replay export call sites.

The implementation will finish with the relevant test targets and the local
desktop compile check documented in `AGENTS.md`.
