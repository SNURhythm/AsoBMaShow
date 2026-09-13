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
- `SettingsCacheMaintenance.*` owns archive-cache jobs, admission, completion,
  and joins; Settings formats their typed results on the application thread.

## Boundaries and invariants

Scene changes and modal operations have explicit ownership: background work
must prove the initiating scene/request is still current before applying a
completion. Root-level overlays such as menus and blocking dialogs must not be
clipped by scrolling content. View components present prepared state and send
intent to their controller/scene; they must not open databases, own native
callbacks, or reconstruct feature policy.

Cache cleanup protects the jukebox's active materialized paths and completes
its filesystem work even if shutdown requests stop. Measurement receives a
stop token. Cleanup may supersede an existing measurement, while new
measurement is rejected during cleanup. Admission clears older queued results,
and generation validation/publication share a mutex so stale workers cannot
overwrite the current request. Scene cleanup and destruction join both jobs.

## Verification

Use `*_view_tests`, `settings_*_tests`, `dropdown_view_tests`,
`context_menu_view_tests`, text/image tests, and the feature-specific scene
tests named by the affected page.
`settings_cache_maintenance_tests` compiles the real job owner, private cache
operations, and the production scene status methods; it covers overlap,
cancellation, stale results, active-file protection, teardown, and UI handoff.

## Related pages

- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Input and controllers](input-and-controllers.md)
- [Gameplay skins](gameplay-skins.md)
