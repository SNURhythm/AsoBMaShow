# Song selector design QA

final result: passed with documented differences
Date: 2026-10-11.

Target: the selected revised Pressing Room light mockup and its dark counterpart.
The user rejected the first palette-only pass, then the detail hierarchy, then
remaining full-screen mismatches. This report covers the final corrective pass.

## Comparison method and evidence

Reference and actual screenshots were viewed together in normalized side-by-side
images, in both themes. The native 2560 × 1440 content area was cropped below the
64-pixel macOS title bar and resized to the reference dimensions. Screenshots
were not retouched. Final captures come from the final compiled native app and
show the user's real 4UE1 difficulty-table library and Aeventyr chart.

- Light capture: `/tmp/asobmashow-redesign2-light-final.png`
- Light comparison: `/tmp/asobmashow-redesign2-light-comparison.jpg`
- Dark capture: `/tmp/asobmashow-redesign2-dark-final.png`
- Dark comparison: `/tmp/asobmashow-redesign2-dark-comparison.jpg`

## Findings resolved

| Surface | Correction verified |
| --- | --- |
| Typography | Roboto Condensed regular and real bold faces replace the wider Latin typography in the selector. Existing Noto fallback retains CJK coverage. Removed accidental synthetic thickening from the new bold asset's metadata. |
| Composition | 80-unit header, 350-unit library, aligned 100-unit chart rows, 540-unit details column, 472-unit artwork frame and pinned 120-unit dock. Closed filter/sort panels no longer reserve layout gaps. |
| Sidebar | Difficulty-table section heading and rule, bold roots, music/favorite icons, disclosure carets and full-width selected background. Table units and clear-state children remain live. |
| Header and list | Search icon and hint, Filter/Sort carets, live chart count, uppercase column labels and outline favorite stars. Filled stars indicate actual favorites. |
| Details | Artwork, title/artist hierarchy, working title favorite/overflow actions, divider, difficulty/native level and tempo at left, personal best at right, four-column facts, expandable metadata and Records/Rankings footer with icons. |
| Dock and color | Segmented icon/value/caret controls and Play action, sampled ivory/cobalt light values and matching dark palette. Non-default assist/ruleset/LN settings retain a visible status. |

Earlier iterations fixed clipped facts/actions, undersized detail typography,
portrait metric order, records-container height restoration, compact clear
lamps, and narrow toolbar/search geometry. This final pass addressed the wider
font, sidebar, icon, separator and control mismatches previously accepted too
quickly.

## Deliberate differences and limits

This is not a pixel-identical reproduction of fictional data. Real chart art is
contained with its original aspect ratio, so the captured 4:3 jacket is
letterboxed inside the square frame. Real names, table units, chart counts,
missing-chart states and unplayed scores replace the mockup's invented library.
The captured table has several missing charts, shown with the existing warning
color, and no fabricated personal best. Long names retain marquee behavior.

The native header keeps actual application utilities and supported tabs. The
dock retains the existing target option; its normal randomization state remains
labeled NORMAL. Viewer and Reveal are accessible from the overflow menu, while
additional metadata and full score details remain in the Chart details
expansion. The dark theme keeps the same navigation structure as the light
theme. These preserve native functionality while following the reference's
composition.

Mouse automation on this machine sometimes sends cached SDL pointer positions
instead of the requested location. Keyboard navigation was used to reach the
final real-chart captures and confirmed crossing the sidebar section heading.
This is not a complete pointer-driven smoke test of every action. Existing
native callback paths were reviewed. The app's existing arrow-key dispatch can
update both library and chart selection during one key press; the final capture
therefore highlights the library level while retaining the chart detail.
Portrait and narrow layouts were verified through geometry tests, not a mobile
device build. No new shader/animation passes were added; no quantitative
performance claim is made.

## Functional verification

- Final desktop main target and affected native test targets built successfully.
- All 14 affected tests passed: main-menu lifecycle/library/settings audits,
  chart details/list, localization, UI scale, input controls and font lifetime. The initial localization run found one unused
  old Search label; removed it, reran that audit successfully and rebuilt main.
- New input regression coverage verifies skipping a section heading in both
  keyboard directions, wrapping, ignoring pointer selection on the heading,
  and an all-nonselectable list.
- New font coverage verifies real bold selection, CJK fallback, and native
  rendering dimensions against the bundled static font.
- Review findings fixed: headings no longer trap keyboard navigation, section
  labels resolve the current language, and active assist/ruleset/LN settings
  are no longer hidden from the pre-play summary.
- `git diff --check` passed.

The original dark theme preference and original window size were restored.
The final dark preview remains running. No deployment was performed.
