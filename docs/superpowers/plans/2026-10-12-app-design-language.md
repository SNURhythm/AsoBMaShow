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

## Opaque scene and modal backgrounds

Transparency exists to reveal BGA, not as a general application surface style.
Application scene backdrops and all panel strengths are now opaque in both
color modes. Dialogs, dropdowns, settings, results and utility scenes inherit
those opaque backgrounds. Modal scrims still dim the surrounding scene.
Selector surfaces retain their existing transparency. Gameplay HUD panels use
explicit translucent tokens with identical colors/alpha to the prior rendering,
so the gameplay presentation does not change as a side effect.

The theme contract now checks opaque scene/modal surfaces and translucent
BGA-facing selector/HUD surfaces separately in both modes.

Opacity correction verification: desktop build and full parallel CTest passed
(483/483), including the unchanged gameplay renderer golden. Fresh native visual
inspection was unavailable because the Mac was locked. `git diff --check` passed.

## Action categories

Grouped actions use a shared semantic palette instead of sharing one accent.
`ActionCategory` defines neutral, primary (indigo), confirmation (green),
information (teal), data transfer (violet), caution (amber), and destructive
(red). Each palette entry includes normal, hover, pressed, and label colors.
Existing semantic accessors route through this palette. Profile action rows
explicitly assign roles to activate, rename, copy/create/apply, import/export,
overwrite, and delete; cancel and disabled actions retain neutral styling.
Scene/modal opacity and BGA-facing surface tokens remain unchanged.

Verification checks distinct categories, opaque colored buttons, and at least
4.5:1 label contrast in every state in both color modes.

Action-category verification: desktop build and both focused tests passed
(settings layout/theme contracts and unchanged gameplay renderer golden).
`git diff --check` passed. This pass did not include fresh native visual inspection.

## Icons for crowded actions

Profile rows use compact 128/144-by-72 icon-and-caption buttons for activation,
rename, copy, delete, export, and overwrite. Their existing semantic colors stay
in place. Delete/overwrite confirmation states retain full-size explicit text.
The replay footer uses the same icon-and-caption content for watch, ghost battle,
results, and video export. Captions remain localized and visible for touch users;
glyph and caption colors update together when actions become disabled.

Verification: desktop build passed; both focused tests passed. Rendered caption
and glyph bounds are checked at compact widths in English, Japanese, and Korean,
and replay tests exercise the actual button clicks through the new content.
`git diff --check` passed. Native preview was blocked by the locked Mac.

## Responsive sizing within the existing layout

Built-in menus choose compact sizing when the shorter available window dimension
is below 600 density-independent points. SDL display scale supplies the density
on every platform, including Android and iOS; no device-model or phone/tablet
identity check is used. Gameplay and skin scenes retain their design coordinates.

Keep the existing navigation and simultaneous library, songs, and details views.
Compact screens narrow the library/details panels, reduce horizontal padding,
and hide secondary chart artwork and summary content. Portrait retains its
existing two detail columns below the song browser, capped at 380 UI units;
landscape retains the existing play-options/Play dock. Essential actions and
search controls use 44-point touch heights. Wider windows restore the ordinary
panels and artwork. Replay/play-options dialogs retain safe-area fitting and
scrolling behavior.

Validation: desktop build and seven focused tests passed, covering compact-size
selection at different densities, portrait/landscape geometry without added
navigation, restoring secondary content, dropdowns, option interaction, replay
modal bounds, and settings/scene regression checks.

Android restricted-file release APK and iOS simulator builds passed. This
correction has automated layout coverage; no fresh hands-on visual pass was
performed. No distribution was run.

## Compact dock and overflow corrections

The compact landscape dock uses one shared height for options and all primary
actions, extending its backgrounds through the bottom safe area while keeping
content padded above it. Five equally sized option cells replace the redundant
Play Options heading in that row. Compact library width grows with the window
(250–350 UI units in landscape; 33% in portrait). Rotation restores the heading
and ordinary padding when leaving this layout.

A zero-width or zero-height hidden/marquee TextView now skips rendering instead
of disabling its clip and drawing text across nearby views. Music page titles
and the IR workspace title are removed in both compact and wide layouts;
existing tabs, status counts, and actions remain.

Regression checks first reproduced the mismatched dock heights and the empty
marquee viewport leak, then passed with the fixes. Ten focused tests passed,
including geometry at resized tablet/phone window sizes, option-cell bounds,
empty text clipping, tab behavior, IR logic, and text/font lifetime. Desktop,
iOS simulator, and Android restricted-file release builds passed. No fresh
hands-on visual pass was performed.
