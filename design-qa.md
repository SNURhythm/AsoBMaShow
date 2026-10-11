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

The detail level badge also uses its natural text width with 12-unit horizontal
insets, growing right from its existing left edge. Long labels leave at least
96 units for the adjacent key-mode value. Regression checks cover growth,
neighbor spacing and restoring the short-label width. The desktop build and
chart-details/UI-scale CTest targets passed; no new visual capture was made.

Download and Unzip now share Play's primary-button constructor: bold 44-unit
labels, 42-unit icons, identical spacing and theme colors. Download uses the
download icon and Unzip uses the archive icon. Desktop compilation and the UI
scale regression target passed. No new visual capture was made.

## Selector translucency

The Play/Course selector backdrop now has alpha 200/255 in both themes.
Unselected chart rows and the selector header inherit that one backdrop instead
of stacking opaque fills. The dock wrapper is transparent; the options button
adds a light translucent dark tint, with stronger hover/pressed states. Text,
selection highlights and primary action buttons retain their contrast. Utility
tabs retain their opaque header. Existing BGA brightness/blur settings still
apply beneath the selector.

Desktop compilation and chart-list, chart-details and UI-scale CTest targets
passed. No fresh visual capture was made for this change.

Unselected tabs and secondary control buttons now use a translucent neutral
fill. Search, utility action cards and both jacket placeholders share a
translucent surface fill. Normal fills use alpha 64/255, hover 96/255 and
pressed 128/255 over the existing selector backdrop, preserving visible
interaction feedback. Desktop build and the three affected CTest targets
(chart-list, chart-details and UI-scale) passed. No fresh visual capture.

Chart-list level badges now use the details panel's blue action fill and white
foreground in dark mode as well as light mode, including selected rows. The
existing theme regression expectation was updated to white text. Desktop build,
chart-list and UI-scale tests passed. No fresh visual capture.

## Square artwork and clear-status badge

Restored 1:1 detail artwork frames and placeholders (112 units compact, up to
472 units desktop). ImageView retains Contain mode so wide/tall images are
letterboxed without cropping or changing the surrounding layout after decode.
The clear status now fills its score-row rectangle with the clear-lamp color,
centered bold text, 12-unit horizontal insets and a colored glow. Black or white
text is chosen using relative luminance. Missing clear status resets the fill,
glow and text color. The separate thin clear bar was removed.

Desktop build and chart-details/UI-scale tests passed, including wide/tall
image transitions, stable square placeholders and clear-status geometry/text.
No fresh visual capture was made.
