# Gameplay Chart-Entity Render Budget Design

## Goal

Prevent pathological charts from exhausting bgfx's shared per-frame transient
buffers or draw-submit capacity and breaking the rest of the screen. Excess
chart and replay geometry may be omitted, but the gameplay HUD and application
UI must retain their rendering headroom.

## Budget

`BMSRenderer` will own one frame-local budget of 60,000 rectangles. The budget
resets at the beginning of every render call and counts the rectangles that
would be submitted by chart-derived and replay-derived world geometry.

The cap is below bgfx's 65,535 draw-call ceiling even in the pessimistic case
where every accepted rectangle causes a separate submit. It leaves about 5,500
submits for the rest of the frame. In the larger textured-note vertex format,
60,000 quads require approximately 4.6 MiB of vertex data and 0.7 MiB of index
data, leaving substantial room in the configured 16 MiB vertex and 4 MiB index
transient pools.

## Covered Geometry and Priority

The budget covers:

- measure lines;
- normal notes;
- long-note bodies, heads, and tails;
- landmines;
- enabled invisible notes and their outline segments;
- replay ghost outlines;
- replay miss-marker blocks.

Chart entities consume the budget first in the renderer's existing chart-time
traversal order. Earlier chart rows therefore survive before later chart rows.
Replay ghosts and miss markers retain their existing placement after chart
entities and use only the remaining budget.

The lane background, judge line, lane beams, lane cover, start-lane indicator,
judgement display, gauge, HUD, touch visuals, and application UI do not consume
this budget. They remain renderable after pathological chart geometry is
truncated.

## Atomic Shapes

Geometry that visually forms one entity reserves its entire rectangle cost
before submitting any part:

- a long-note head reserves three rectangles for its possible body, head, and
  tail and carries that reservation through the existing lookahead entry;
- a normal invisible note reserves all visible outline segments;
- an invisible long-note marker reserves its solid rectangle;
- a replay ghost reserves its four outline rectangles;
- a replay miss marker reserves all fourteen blocks forming its X.

If the remaining budget cannot fit the complete shape, that shape is omitted.
This prevents half outlines, headless long notes, and incomplete miss markers.
Single-rectangle notes, landmines, and measure lines reserve one rectangle.

Long-note capacity is reserved at the head row rather than when traversal
later reaches the tail. This preserves chart-time priority and the existing
rule that deferred long-note geometry follows its head. A long note may use
fewer than its three reserved rectangles because of clipping or state; unused
capacity is intentionally not returned during that frame. Long notes whose
heads precede the current traversal window reserve before current chart rows.

## State and Ordering

Budget exhaustion affects geometry only. Timeline traversal, note expiration,
orphan long-note tracking, long-note lookahead, gameplay simulation, and replay
state continue normally. The renderer does not break out of chart traversal
when the budget is exhausted.

Budget checks happen before selecting a note batch depth or adding geometry.
Rejected entities therefore do not create empty depth transitions or submits.
Accepted entities continue to use the existing row-based depths and layer
ordering.

## Structure

Add a small header-only `GameplayChartEntityRenderBudget.h` beside the gameplay
renderer. It will provide the 60,000-rectangle constant and a budget object
with reset, atomic consume, remaining, and exhaustion queries. `BMSRenderer`
will share one instance across every covered draw path.

The budget object is independent of bgfx so its boundary behavior can be unit
tested without initializing a renderer.

## Tests

Focused unit tests will verify that the budget:

- starts with 60,000 rectangles;
- accepts complete requests within the remaining capacity;
- accepts an exact-boundary request;
- rejects an over-budget request without partially consuming it;
- stays exhausted after rejection until reset;
- restores the full capacity on the next frame reset.

Existing geometry tests will continue to cover outline segmentation. The
desktop target and complete CTest suite will be run after integration.

## Scope

This change does not alter timing, scroll geometry, note clipping, note
appearance, chart parsing, row-based layering, replay contents, or the bgfx
pool sizes. It only bounds chart/replay rectangle submission in gameplay.
