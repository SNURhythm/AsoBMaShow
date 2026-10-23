# Settings and user interface

## Intent and user flow

Scenes provide the main menu, chart viewer, settings, music player, gameplay,
results, and supporting overlays. Settings exposes profile-scoped preferences
for input, audio/video, Internet Ranking, gameplay options, tables, and skins.
Views are reusable layout/rendering components, not owners of application
transactions.

## Application appearance and responsive layout

`src/view/AppTheme.h` shares the selector's typography and palette across
built-in screens: Roboto Condensed with CJK fallback, square controls and
artwork, and flat sections divided by rules. Application scene and modal
backgrounds are opaque in both themes. BGA-facing selector and gameplay HUD
surfaces retain explicit translucency; modal scrims dim surrounding content.
Background fills cover the full window, with safe-area insets applied to content
and controls. Gameplay lanes, user-authored skins, and semantic judgement,
gauge, rank, and graph colors retain their own presentation contracts.

Actions use semantic categories: neutral, primary, confirmation, information,
data transfer, caution, and destructive. Normal, hover, pressed, and disabled
states retain readable labels. Compact profile and replay actions keep localized
captions alongside icons; destructive confirmation keeps explicit text.

`src/rendering/ResponsiveUi.h` chooses compact built-in menus when the shorter
available dimension is below 600 density-independent points, using display
scale rather than device identity. Gameplay and custom skins keep their design
coordinates. Compact layout preserves simultaneous library, songs, and details
views, narrows panels, and hides secondary artwork/summary content. Essential
actions retain 44-point touch heights. Wider windows restore ordinary panels;
dialogs fit safe areas and scroll. Empty or hidden text viewports must not paint
outside their clip.

The compact landscape dock gives options and primary actions one shared height.
Its backgrounds extend through the bottom safe area while content stays padded
above it. Rotation restores the ordinary layout. Settings uses a navigation
rail on wide screens and a section dropdown in portrait; Music and IR share the
selector's workspace boundaries without redundant page headers. Results lead
with the current score; practice keeps Start outside scrolling content.

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
  unavailable. The live preview uses the same left-aligned editable rows, including its
  scroll duration and cover amounts. Judge line and measure line controls belong
  to Lane in both the skin editor and live preview.
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

## Choosing a settings control

Use this rule for new or revised built-in settings, including the same choices
shown in Play Options and previews. Existing controls are not exceptions by
precedent; apply the rule when revising them. The [migration inventory](settings-choice-migration-2026-10-24.md) lists the
Settings, Play Options, and preview controls aligned with it. Saved values
retain their existing meaning.

Use a cycle button for a toggle, visible buttons for a few choices that fit,
and a dropdown for other cases. Users must always be able to see the current
value.

| Control | Use when | Examples |
| --- | --- | --- |
| Cycling button | A toggle between two opposing states, such as On/Off, Show/Hide, or Light/Dark. Two options alone do not make a toggle. One activation switches directly to the other state. | Show lane indicators: On / Off; appearance: Light / Dark. |
| Multiple choice buttons | A few stable, mutually exclusive choices, including two distinct modes or roughly 3–4 options, with labels that all fit legibly together on the current screen. | Flick / Tap; LR2 / Beatoraja; Auto / Landscape / Portrait. |
| Dropdown | Other cases: larger sets, dynamic lists, long labels, or a small choice group that does not fit. A dynamic list does not become a toggle just because it currently has two entries. | Language, audio device, resolution, skin package, file choice. |

Apply these rules in order: a toggle uses one cycling button; a small choice
set uses multiple buttons if it fits; everything else uses a dropdown. Do not
extend cycling to three or more states to save space. Continuous numeric
values use a slider or editable numeric control rather than this rule.

### Consistent presentation and behavior

- Put a persistent setting label above the control and align the group with
  the section's left edge. A selected value must not be the only explanation
  of what the setting controls.
- Multiple buttons form one single-selection group with a clear selected
  state and consistent heights, spacing, and widths within the group. Selecting
  the active choice is a no-op. Independent toggles use separate cycling
  buttons, not a single mutually exclusive choice group.
- A cycling button displays the **current** state and a visible cycle
  affordance. One activation switches to the other state; the next switches
  back. Users must be able to distinguish it from an action button or a
  dropdown that opens a list. Use the Club Beat palette consistently: active
  (On/Show) uses the primary accent, inactive (Off/Hide) uses the neutral control
  surface. Opposing values without an enabled/disabled meaning—Left/Right,
  Regular/Bold, Combined/Separated, and Light/Dark—stay neutral in both states.
  Captions and cycle icons share a contrasting foreground in both themes;
  disabled toggles use muted text.
- Cycling toggles use compact content widths: label, state, icon, and padding,
  capped at the available parent width. Align them to the start of their row;
  do not grow them to fill unused space. Keep the normal touch-target height
  and refresh the width when the label or language changes. This applies to
  named toggles, current-value cycles, and two-state choice controls alike.
- Navigation tabs use transparent surfaces with an accent underline (or a left
  indicator for vertical navigation), keeping them distinct from filled setting
  choice buttons.
- A dropdown displays the current value and a disclosure indicator. Opening
  it does not change the setting. Indicate the selected option in the list;
  commit only the chosen option, and retain the previous value if applying it
  fails. Preserve an unavailable saved value visibly rather than silently
  substituting another choice. Menu rows use the 66-unit touch target in both
  wide and compact layouts. Gauge types and gauge bounds show leading color
  bars using the clear-lamp palette, including the selected trigger. Auto-shift
  modes have no lamp color bars.
- Keep the same labels, option order, selected value, and meaning wherever a
  setting appears. If a compact surface needs a different control, preserve
  those semantics. Use the same cycling-button convention for toggles across
  settings instead of mixing checkboxes, two-button groups, and dropdowns.

### Compact layouts and localization

Judge whether a button group fits at each supported size using supported
translations, including the smallest layout. A group may use buttons on a
wide screen and a dropdown on a narrow screen while retaining its selection.
Keep readable labels and the existing touch-target metrics. If the group cannot fit in one
row, use a dropdown instead of shrinking text excessively or leaving a lone
wrapped button. Do not replace it with a cycling button just to make it fit.

Dropdown menus use the shared safe-area placement and selected-option reveal
behavior. Verify both the outer allocation and the inner trigger bounds; a
visible row with a collapsed trigger is not a usable control. When changing
the control type, check selection, persistence, and normal/compact layouts
with the relevant existing UI tests.

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
