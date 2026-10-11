# Settings and user interface

## Intent and user flow

Scenes provide the main menu, chart viewer, settings, music player, gameplay,
results, and supporting overlays. Settings exposes profile-scoped preferences
for input, audio/video, Internet Ranking, gameplay options, tables, and skins.
Views are reusable layout/rendering components, not owners of application
transactions.

## Settings organization

Navigation follows this order: Profiles, Input, Timing, Lane, HUD, Background,
Skins, Audio, Display, BMS Library, Difficulty Tables, IR, General.

- Profiles shows the active profile first, other profiles next, then creation
  and import/export tools.
- Input separates profile-wide note priority, touch indicators, and iPad
  gesture reminders from scoped configuration. These preferences appear above
  the scope selector and apply to every mode/device. In the scoped section,
  key mode selects touch/virtual-controller layouts and bindings; player and
  device select bindings. Touch and virtual-controller editors precede bindings,
  and the input monitor follows the binding list.
- Timing contains audio and visual calibration offsets. HUD owns judgement
  text/combo styling and position, FAST/SLOW and millisecond feedback, timing
  indicators, judgement counts, and gauge placement. Built-in-only controls
  are identified separately from HUD settings shared with custom skins.
- Lane contains preview, shared scroll/covers, and visibility aids. Built-in
  appearance has one editor in Skins, with horizontal Lane, Notes, Effects,
  and HUD tabs. Numeric appearance controls consistently use left-aligned
  decrement, editable value, increment, and reset controls, retaining each
  setting’s limits and reset behavior. Toggle and choice-button groups start
  below their labels at the left edge. Lane groups width, length, angle, scratch position, opacity,
  and judge/measure lines; Effects groups beams and keybombs. The catalog's
  screen/mode selectors stay vertical; only the built-in editor uses horizontal tabs.
  Built-in controls remain available here when custom skin services are
  unavailable. The live preview retains its compact editing controls.
  Background groups BGA playback, aspect, brightness, and blur. Skin package
  installation and diagnostics follow selection and appearance controls.
- Audio starts with volumes and input sounds/count-in, followed by device,
  sample rate/buffer settings, and test sound/status. Display retains orientation
  and platform display controls.
- BMS Library groups folders, archive preview/extraction preferences, and cache
  cleanup. Difficulty Tables puts installation in its own first card, before
  installed-table updates/removal. IR groups connection and submission tools.
- General contains language and application theme.

Keybomb appearance is stored per key mode within each profile and orientation.
Scratchless modes follow the parent mode when their skin is set to follow it.
Other settings retain their existing scope and saved values.

## Code map

- `src/scene/SceneManager.*` and `Scene.*` define scene navigation and shared
  lifetime conventions.
- `MainMenuScene.*`, `ChartViewerScene.*`, `SettingsScene.*`,
  `MusicPlayerScene.*`, and `ResultScene.*` implement primary flows.
- `src/view/` contains controls, scrolling/recycling, text/image loading,
  dropdowns, overlays, and common UI theme primitives.
- Focused scene models/controllers in `src/scene/` keep persistence, input
  capture, settings validation, and feature actions out of rendering code.
- `SettingsLibraryTask.*` owns the exclusive table/folder worker and typed
  pending updates; its workers capture repository/request data, not the scene.
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

Table/folder jobs hold admission until their operation returns. The latest
status per channel and import progress are consumed on the application thread;
reload requests accumulate until consumption. Shutdown joins uncancellable
operations, suppresses late publication, and discards pending updates. Both
scene cleanup and destruction stop the owner while dependencies remain alive.

## Verification

Use `*_view_tests`, `settings_*_tests`, `dropdown_view_tests`,
`context_menu_view_tests`, text/image tests, and the feature-specific scene
tests named by the affected page.
`settings_cache_maintenance_tests` compiles the real job owner, private cache
operations, and the production scene status methods; it covers overlap,
cancellation, stale results, active-file protection, teardown, and UI handoff.

`settings_library_task_tests` exercises task admission, publication, shutdown,
and complete production launch/delivery methods with controlled repository and
import operations. `settings_library_lifecycle_tests` compiles the production
destructor to verify stop/join before callback dependencies are destroyed.

## Related pages

- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Input and controllers](input-and-controllers.md)
- [Gameplay skins](gameplay-skins.md)
