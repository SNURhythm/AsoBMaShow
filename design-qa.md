# Song selector design QA

Final result: desktop composition and detail hierarchy verified against both references; deliberate differences are listed below.
Date: 2026-10-11.

Target: the selected revised Pressing Room light mockup and its dark counterpart.
The first delivery retained the old composition and failed fidelity review. This
correction rebuilds the native selector around an 80-unit header, 350-unit
library, aligned 100-unit song rows, 540-unit details column with a 472-unit
artwork frame, and a 120-unit segmented play dock.

## Visual comparison

Reference and actual screenshots were viewed side by side in the same image.
The actual 2560 × 1440 content area was cropped below the macOS title bar and
scaled to the reference dimensions for comparison; source screenshots were not
retouched. Both themes were captured from the final compiled implementation.

Local evidence:

- Light capture: `/tmp/asobmashow-redesign2-light-final.png`
- Light comparison: `/tmp/asobmashow-redesign2-light-comparison.jpg`
- Dark capture: `/tmp/asobmashow-redesign2-dark-final.png`
- Dark comparison: `/tmp/asobmashow-redesign2-dark-comparison.jpg`
- Detail close-up: `/tmp/asobmashow-detail-hierarchy-comparison.jpg`

| Surface | Checked result |
| --- | --- |
| Typography | Bold song/title hierarchy, restrained secondary metadata, readable score captions. Long selected names retain marquee behavior. |
| Spacing and composition | Continuous library/list/details columns, aligned table headings and rows, large artwork, fixed segmented play dock. |
| Colors | Ivory/ink/cobalt light palette; charcoal/off-white/cobalt dark palette; inverse selection and semantic clear lamps. |
| Images | Small square row thumbnails and large contained artwork. Original aspect ratio is preserved without stretching. |
| Copy and controls | Real chart metadata, difficulty-table units, native levels, four-column fact summary, expandable additional metadata and full score details, records/rankings, viewer/reveal and existing utilities retained. |

The initial comparisons resolved clipped bottom facts/actions and undersized
text, but the user correctly identified that the detail hierarchy still differed
from the reference. A further correction puts the artist below a 48-unit title,
adds the divider, places difficulty/native level and BPM/duration on the left,
and stacks grade, percentage and PERSONAL BEST on the right. The facts now form
a single four-column strip below a Chart details disclosure. Expanding it keeps
genre, judge rank, duration, full EX score, next-grade progress and clear state
available. Records/rankings use a flat footer. Final captures include this
correction in both themes.

Intentional differences from the generated reference: real chart artwork and
long names replace fictional content; unplayed charts show no invented grade;
additional native metadata remains available through the disclosure; the dock includes the existing
target option; header utilities reflect actual application actions. The real
4:3 stage image is letterboxed in the square frame. Table data and controls
remain live rather than copying the mockup's fictional library counts. Viewer
and Reveal retain text actions; favorites stay in each chart row rather than
adding duplicate title-bar controls. These are not pixel-identical copies.

## Functional review and verification

Resolved review findings: compact clear lamps stay visible, toolbar widths
restore after rotation, narrow layouts hide incompatible column headings,
row construction batches layout until children exist, and the search hint
lives outside the measured text-input node. Narrow desktop search/toolbar
geometry and portrait-to-landscape restoration have real-Yoga coverage.
The disclosure review also caught and fixed portrait fact-row reordering and a
stale 44-unit records container after rotation; both have regression assertions.

- Full native build passed.
- Full parallel CTest run: 481/482 passed initially; the localization audit found
  two obsolete Start-label entries after the Play-label change. Removed those
  unused entries, reran the failed test successfully, and rebuilt the app.
- After the detail correction, all 11 affected main-menu, chart-view, localization
  and UI-scale tests passed. Native chart-view coverage includes difficulty units,
  compact clear lamps, artwork geometry, disclosure expansion, cached best-score
  summary, and detail metric order after orientation changes.
- `git diff --check` passed.

The desktop app was exercised with a real library and artwork in both themes.
Automated mouse warping exposed cached SDL pointer coordinates on this machine,
so this is not a claim that every action received a complete pointer-driven
smoke test. Portrait and narrow layouts were checked through geometry tests;
no iOS or Android device build was performed. No new shaders or animation
passes were added, and no quantitative performance claim is made.

The original dark theme preference was restored and the corrected preview was
left running. No deployment was performed.
