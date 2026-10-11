# Song selector refinement QA

Date: 2026-10-12.

## Requested changes

- Removed the redundant folder title and chart count beside search/sort.
- Portrait hides the wordmark in both Yoga layout and the render visibility flag.
- Level/key-mode text aligns right and expands left within the available row.
- Removed the duplicate detail favorite, Chart details fold and overflow button
  beside Rankings. Metadata stays open; the title's Viewer/Reveal menu remains.
- Stagefile artwork uses decoded aspect ratio, with a 4:3 missing/loading
  placeholder. Wide artwork saves vertical space; compact tall artwork fits.
- The dock replaces key mode with ASSIST, uses uppercase captions without the
  Gauge colon, and enlarges PLAY OPTIONS without its left separator.
- The bottom bar and landscape Play button ignore the bottom safe-area inset.
  Portrait also excludes that inset from its content sizing and padding.

## Validation

The full CTest suite passed all 483 tests. After the narrow-row review fix, the
desktop app was rebuilt and all five affected CTest targets passed again
(chart rows, details, UI scale, localization and catalog). Regression
checks cover wide/tall decoded artwork, artwork arriving after initial layout,
unchanged-frame layout stability, always-visible metadata, right-aligned labels,
wide portrait wordmark hiding, rotation and bottom safe-area geometry.

Review identified unbounded difficulty text that could push trailing controls
outside narrow rows. The final implementation caps it to the row's available
space and updates it when the row is resized.

## Limits

A fresh native screenshot could not be captured because the Mac was locked.
Portrait and safe-area behavior are covered by real Yoga layout tests; no mobile
build or device visual check was performed. Screenshots from the earlier tab
redesign predate these refinements. No deployment was performed.

## Clear lamp, favorite feedback and score-card follow-up

The clear lamp now uses an eight-unit glow in its clear-state color, with no
border; recycling to a row without a lamp clears the glow. Favorite hover and
pressed feedback use subtle foreground-color overlays that adapt to selected
and unselected rows in either theme.

The landscape EX-score/next-rank card now follows the BPM/personal-best summary,
before chart facts. Its duplicate grade/rate is hidden, while the sole portrait
rank/rate remains visible. Rotation and external portrait score ownership remain
covered by native detail-view tests.

The desktop build and three affected CTest targets passed. A native desktop
visual check confirmed the score-card placement using an unplayed chart
(`/tmp/asobmashow-score-polish.png`). This library did not provide a scored chart
for checking the glow and score values visually; scored geometry is covered by
fixtures. Native pointer automation still did not reliably select/hover rows,
so hover/down colors were checked in code rather than captured visually.
