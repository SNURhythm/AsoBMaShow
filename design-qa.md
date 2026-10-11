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
  rank sits below it at the lower right. The clear text has a soft glyph-shaped
  halo. Row lamps use a rounded bright core, close glow and wider subtle halo.
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
