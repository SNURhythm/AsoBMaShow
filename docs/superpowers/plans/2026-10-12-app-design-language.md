# Extend selector design language across application UI

User objective: apply the same design language to all remaining application
parts, including gameplay title, score and pause chrome, except the lane viewport. The selector is the approved reference, including the
latest square letterboxed jackets, blue/white level badges, translucent neutral
surfaces, Roboto Condensed typography and square controls.

## Requirements and coverage

- Introduce an application-only theme sharing selector colors and font paths.
  Keep UiTheme and lane viewport rendering unchanged; preserve user-authored
  gameplay and result skins and semantic graph/judgement colors.
- Apply the theme to settings and every settings tab, profile and skin
  management, intro, music playback, IR uploads, background tasks, chart viewer,
  chart filtering, tutorials, loading, downloads and archive dialogs.
- Apply it to built-in results, score records, replay dialogs, rankings and
  practice analytics/configuration.
- Update shared application buttons, popovers, dropdowns, search/text fields,
  sliders, checkboxes and play-option controls. Keep icons and CJK fallback.
- Audit remaining direct UiTheme usage, font literals, decorative radii,
  gradients, shadows and static old palette colors. Classify every exception.
- Preserve behavior, responsive geometry, localization and accessibility.

## Verification

Build main and affected native test targets once files are stable, then run
full CTest in parallel. Fix fixture assumptions only when they encode styling
that the user explicitly changed; investigate actual behavior failures.
Inspect native screens in both themes and representative narrow/portrait
layouts when desktop control is available. Record any unavailable visual
coverage explicitly and keep it outstanding, rather than claiming a complete
visual audit. Confirm gameplay source edits are limited to surrounding HUD/pause chrome and UiTheme is unchanged. Commit and push only
related changes, leaving the existing unrelated iOS project change untouched.

## Implementation and inspection

Added `AppTheme` sharing selector typography, neutral translucent surfaces,
square controls and cobalt primary actions. Applied it across built-in app
screens, shared controls, result exports and outside-lane gameplay chrome.
Semantic judgement, gauge, rank and lane-order colors remain distinct.
Square button borders now draw only the edges so their opaque border does not
obscure the translucent interior; rounded rendering retains its previous path.

Native visual samples: dark Intro, Settings/Profile and selector; light Intro,
Music library and IR empty state. Text, CJK fallback, selected buttons and panel
hierarchy were readable in those samples. Restored the original dark setting.
Pointer actions in the desktop SDL window did not reliably reach controls;
therefore this is not an exhaustive visual inspection of every modal, result,
portrait layout or gameplay overlay. Those paths retain automated layout,
interaction and renderer regression coverage.

Renderer golden inspection: 52,322 changed pixels are confined to title,
score, autoplay and counter rectangles. Outside those four HUD regions,
including lane content, gauge and judgement effects, all RGBA values are
identical to the prior golden (maximum channel delta zero). Updated the
reference only after this comparison.

Final verification: full desktop build passed. Full parallel CTest passed
481/483; the two affected tests passed on focused reruns after the reviewed
HUD golden update and Guided Access instruction theme migration (483/483
verified across the full run and focused reruns). `git diff --check` passed.

## Structural alignment

Extended the selector's composition across the remaining built-in screens:

- Landing screen uses a large left wordmark and a flat right action menu,
  stacking vertically in portrait with safe-area padding.
- Settings uses a compact title band, flat left navigation and content-height
  sections separated by rules. Portrait section navigation is one dropdown.
- Music and IR share the selector's rail/workspace boundaries without duplicate
  embedded headers. Music uses inverse selected rows, square artwork and a
  full-width primary action; player and playlist workspaces scroll and stack
  at narrower widths. IR separates provider controls from the upload workspace.
- Results prioritize the current score and use open ruled sections. Records
  separate the title, tools and list. Practice keeps Start outside the scroll
  area; analytics places charts ahead of attempt navigation.
- Play options and rankings use compact headers, ruled sections and fixed
  action footers.

Added a localized settings density/layout check and updated result hierarchy
assertions and lifecycle fixtures. Removed four unused translations belonging
to the redundant labels. Native samples for this pass cover dark landing,
Settings/Profile, Music library and IR, plus light IR. The IR status message
wraps within its provider rail. Restored the original dark theme after QA.
Portrait layouts and every modal/result state still need hands-on visual
coverage; desktop pointer automation remains unreliable. The gameplay lane
viewport is untouched by this structural pass.

Structural-pass verification: desktop build passed; a fresh full parallel CTest
run passed all 483 tests after the fixture/catalog fixes. `git diff --check`
passed.

## Palette and divider polish

Moved application chrome to charcoal/slate and cool white with indigo-blue
actions, coordinated hover/pressed states, and cool neutral text. Light-mode
backdrop opacity is 232/255 to avoid a muddy gray canvas; controls and artwork
placeholders remain translucent. Primary action white-label contrast is 5.76:1
in light mode and 4.76:1 in dark mode; hover states retain at least that contrast.

Dividers now distinguish subtle row separators, section rules and stronger
workspace boundaries. The selector header rule spans the whole workspace, and
Music, IR and Settings use the same boundary hierarchy. Native selector samples
were inspected in both themes, and the original dark theme was restored.

Reviewed the updated built-in renderer reference: 51,730 changed pixels are
confined to title, score, autoplay and judgement-counter rectangles. Every pixel
outside those HUD rectangles is unchanged, including the lane viewport.

Palette verification: desktop build and fresh full parallel CTest passed
(483/483); `git diff --check` passed.
