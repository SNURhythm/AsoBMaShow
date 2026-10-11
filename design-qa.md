# Song selector action and detail QA

Date: 2026-10-12.

## Requested changes

- Play, Download and solid-archive Unzip now share one primary action slot.
  Download takes priority while available, then Unzip, otherwise Play. Rotation
  moves the whole slot; the landscape slot matches the 540-unit detail panel.
- Records, Rankings, Viewer and Reveal occupy a pinned 2×2 grid. Both rows use
  matching column widths/gaps, 76-unit landscape buttons and 64-unit portrait
  buttons. Existing action guards and Reveal's file/folder choices remain.
- Genre sits immediately below artist. Desktop chart facts omit the duplicate
  BPM and length, which remain in the summary. Portrait retains its sole values.
- Next-rank difference is at the score card's upper right; a larger bold clear
  rank sits below it at the lower right. A glowing vertical bar accompanies the
  plain clear-rank text. Row lamps use a rounded bright core, close glow and wider subtle halo.
- Summary grade and percentage align right, matching the difficulty and tempo
  rows. The redundant PERSONAL BEST caption and its extra height are removed.

## Verification

The desktop app and affected native view targets built successfully. Native
view tests cover score-card positions, genre order, metadata deduplication,
artwork aspect ratios, score ownership and rotation. Real Yoga tests exercise
Play/Download/Unzip replacement and priority at multiple portrait/landscape
sizes, dock width, bottom safe area and aligned action-grid columns.

Read-only review found no remaining material issue in ownership, action
availability, rotation or glow rendering. All 483 CTest tests passed. After
matching both grid-row gaps, the desktop app rebuilt and all 10 UI-scale
checks passed again.

## Limits

The Mac locked during the fresh native visual check, so no screenshot of this
revision was captured. Earlier screenshots predate this revision. Glow
appearance still needs a visual check on an unlocked desktop or device.
No mobile build, download, extraction, score upload or deployment was performed.

## Portrait title and fact spacing fix

Portrait titles now use the same single-line marquee behavior as landscape.
Fact rows keep their natural height, with labels and values adjacent; extra
portrait height is distributed between rows instead of inside each pair.
Regression checks cover long-title metrics and pair/row spacing at 520- and
720-unit panel heights. The desktop build and the three affected CTest targets
(chart details, UI scale and text input) passed. No new visual capture was made.

## Glow correction

Removed the glyph-shaped glow entirely. Clear-rank lettering stays plain and
bold; its adjacent rounded status bar glows instead. Folder clear lamps now
use the same bright core and soft colored glow, clearing it when a recycled
folder has no clear rank.

The chart-list level badge now fits its text with 12 units of padding on each
side and remains anchored to the right of its column. Long values still respect
the existing narrow-row width cap and marquee behavior.
