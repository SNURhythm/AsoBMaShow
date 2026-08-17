# Settings and user interface

## Intent and user flow

Scenes provide the main menu, chart viewer, settings, music player, gameplay,
results, and supporting overlays. Settings exposes profile-scoped preferences
for input, audio/video, Internet Ranking, gameplay options, tables, and skins.
Views are reusable layout/rendering components, not owners of application
transactions.

## Code map

- `src/scene/SceneManager.*` and `Scene.*` define scene navigation and shared
  lifetime conventions.
- `MainMenuScene.*`, `ChartViewerScene.*`, `SettingsScene.*`,
  `MusicPlayerScene.*`, and `ResultScene.*` implement primary flows.
- `src/view/` contains controls, scrolling/recycling, text/image loading,
  dropdowns, overlays, and common UI theme primitives.
- Focused scene models/controllers in `src/scene/` keep persistence, input
  capture, settings validation, and feature actions out of rendering code.

## Boundaries and invariants

Scene changes and modal operations have explicit ownership: background work
must prove the initiating scene/request is still current before applying a
completion. Root-level overlays such as menus and blocking dialogs must not be
clipped by scrolling content. View components present prepared state and send
intent to their controller/scene; they must not open databases, own native
callbacks, or reconstruct feature policy.

## Verification

Use `*_view_tests`, `settings_*_tests`, `dropdown_view_tests`,
`context_menu_view_tests`, text/image tests, and the feature-specific scene
tests named by the affected page.

## Related pages

- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Input and controllers](input-and-controllers.md)
- [Gameplay skins](gameplay-skins.md)
