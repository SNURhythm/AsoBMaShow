# Compact Mobile Result Layout Design

## Goal

Keep every result-screen control reachable on short mobile viewports while retaining the gauge history graph and timing analytics. Apply the same side-by-side visual organization to result photo exports without changing replay-video result screens.

## Root Cause

The UI uses a fixed logical width of 1920 units and derives logical height from the drawable aspect ratio. A modern iPhone in landscape exposes roughly 885 logical height units. The current result layout requires about 1074 units because the 136-unit gauge graph and the timing analytics card, whose minimum height is 236 units, are stacked vertically. Yoga centers the overflowing column, clipping the header and placing the action controls below the viewport.

## Screen Layout

When timing analytics is present, the gauge history graph and the active timing analytics card share one horizontal `resultVisuals` row:

- Gauge graph: 40 percent of the usable row width.
- Timing analytics: 60 percent of the usable row width.
- Regular row height: 250 logical units.
- Short-mobile row height: 236 logical units, which preserves the analytics card's complete controls and labels.

When timing analytics is unavailable, the gauge graph remains full-width at its existing 136-unit height. The screen does not become scrollable and does not hide analytics data.

## Short-Mobile Card Metrics

Compact cards apply only when the target is mobile and the logical viewport height is short. Desktop screens, including short desktop windows, keep their existing card metrics.

The compact layout uses:

- Root padding: 24 instead of 32 logical units.
- Root gap: 8 instead of 12 logical units.
- Summary row height: 184 instead of 198 logical units.
- Information row height: 90 instead of 100 logical units.
- Judgement row height: 96 instead of 108 logical units.
- Reduced internal vertical padding and child heights within those cards, while preserving the existing content, colors, typography hierarchy, and horizontal structure.
- Action buttons remain 64 logical units high.

At an approximately 885-unit iPhone viewport, these metrics keep the action row's bottom edge inside the viewport with the complete 236-unit visual row.

## Photo Export Layout

Photo exports retain all three analytics modes but organize the gauge graph and analytics into a two-by-two visual block:

1. Gauge history graph beside Histogram.
2. Lanes beside Sections.

Regular-size exports use the existing photo analytics card heights. Photos rendered at short-mobile geometry use proportional compact visual-row heights and the compact result card metrics. The exporter recalculates its canvas from the grid instead of adding the current stacked-analytics extra height. Photos without analytics keep the existing full-width gauge graph.

Replay-video result screens are unchanged.

## Components

`ResultLayoutGeometry` owns the pure responsive policy: target kind, short-height threshold, root spacing, card heights, visual-row heights, width split, and photo-grid geometry. `DefaultSkin` consumes those metrics when constructing result cards and creates the shared screen visual row. `ResultScene` attaches the live analytics view to the analytics host and collapses the row to the legacy graph height when no analytics model exists. `ResultImageExporter` consumes the same policy to construct the two-by-two photo visual block and calculate export height.

The gauge graph continues using its placeholder's final Yoga bounds, so its renderer requires no new chart-data path.

## Testing

- Pure geometry tests prove that an approximately 885-unit mobile viewport selects compact metrics and fits the action row.
- The same tests prove that a 1080-unit desktop viewport retains existing card dimensions.
- Photo geometry tests prove the two-by-two ordering and canvas height, including the no-analytics fallback.
- Yoga layout tests build representative result sections and assert that the mobile action row ends within the root viewport.
- Existing practice analytics, view layout, result export audits, and the desktop compile target must continue to pass.

## Non-Goals

- No vertical scrolling on the result screen.
- No analytics data removal.
- No replay-video result-layout change.
- No compact desktop card layout.
