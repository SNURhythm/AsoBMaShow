# Chart Viewer Invisible-Note Outline Design

## Goal

Match gameplay's invisible-note appearance in the chart viewer when the
existing "Show Invisible Notes" setting is enabled. Normal invisible notes
become hollow orange outlines; invisible long notes remain solid orange
rectangles.

## Geometry

Add a small chart-viewer geometry helper that describes an invisible note as
content-space rectangles:

- a normal note produces four border rectangles around an empty center;
- a long note produces one rectangle covering the full note marker.

The normal-note border thickness uses the same rule as gameplay: 12 percent of
the marker height, clamped so it cannot exceed half the marker width or height.
For the chart viewer's current six-unit marker height, this is a 0.72-unit
border before zoom is applied.

`ChartViewerScene` will pass every produced rectangle through its existing
`drawRectClip` path. Scrolling, zoom, viewport clipping, the existing orange
color, and the current layer order therefore remain unchanged.

## Structure

Create a header-only `ChartViewerNoteGeometry.h` beside the chart viewer. It
will expose the rectangle description and the function that selects solid or
outline geometry. Keeping the pure geometry outside `ChartViewerScene.cpp`
makes the visual rule independently testable without constructing the full UI
or renderer.

The gameplay geometry helper will not be moved or refactored. The two renderers
use different clipping coordinate systems, and sharing that helper would make
this small viewer parity change affect established gameplay code.

## Tests

Add a lightweight geometry test executable that verifies:

- a normal invisible note produces four border segments;
- the normal note's center remains uncovered;
- a long invisible note produces one full solid rectangle;
- border thickness is clamped for markers too narrow to contain the nominal
  border.

The test will be added before the geometry helper so its initial build fails
because the requested interface does not yet exist. After implementation, run
the focused test, compile the desktop `main` target, and run the complete CTest
suite.

## Scope

This change does not alter gameplay rendering, chart parsing, note ordering,
the visibility setting, invisible-note expiration, or any long-note body
rendering. It only changes enabled invisible normal-note markers in the chart
viewer from solid to border-only.
