# Reveal Context Menu and Same-Folder Scope Design

## Goal

Turn the main-menu `Reveal` action into a small anchored context menu that can
either show every chart beside the selected chart or perform the existing
platform file-manager reveal.

## Scope

- Replace the direct `Reveal` button action with a two-item context menu.
- Introduce the menu as a reusable view component rather than scene-local
  layout.
- Add an exact-folder constraint to chart-list queries.
- Keep the folder view temporary; do not add physical folders to the library
  sidebar.
- Preserve the existing platform-specific reveal behavior and error logging.
- Do not change archive scanning, physical folder scanning, or library sidebar
  categories.

## Context Menu Component

Create a common anchored context-menu view that receives a list of action IDs
and labels plus callbacks for action selection and open-state changes. The
component owns only menu presentation and interaction; the caller owns the
trigger button and decides what each action does.

The component will use `OverlayPortal` so the menu renders above normal scene
content. It will share the existing anchored-overlay placement rules, including
viewport clamping and choosing space above or below the trigger. Its panel and
action rows will use the existing themed control colors, border, corner radius,
shadow, and text styles.

The menu will close when:

- an enabled action is selected;
- the user presses or taps outside the trigger and menu;
- the user presses Escape or Android Back;
- the trigger is pressed while the menu is already open; or
- the owning scene closes it during another state transition.

Closing input must be consumed when necessary so it cannot activate content
behind the overlay. Destroying the component must unregister its overlay safely.

## Reveal Interaction

Pressing the existing `Reveal` button toggles the context menu with these
actions in order:

1. `Show Same Folder`
2. `Reveal File`

`Reveal File` calls the current reveal operation without changing its platform
logic, archive-to-container mapping, validation, or failure logging.

The menu is available under the same conditions as the current `Reveal`
button: a playable, available chart with a nonempty chart path is selected and
the main menu is not blocked by an in-progress transition.

## Same-Folder Chart Scope

`Show Same Folder` derives the scope from the selected chart's stored folder
metadata. If that metadata is empty, it falls back to the selected chart path's
parent. The chart repository will compare the normalized stored folder value
for exact equality; descendants are not included.

For ordinary charts, this selects charts with the same physical parent folder.
For an archive-backed chart, the stored folder is its virtual archive path plus
the exact inner parent path. For example, `pack.zip/A/song.bms` can match other
charts in `pack.zip/A/` but cannot match charts in `pack.zip/B/`.

Activating this scope will:

- clear the search text and visible search field;
- reset all chart filters;
- preserve the current chart sort criterion and direction;
- stop applying the previously selected library-sidebar category;
- reload the chart list with only the exact-folder constraint; and
- keep the originally selected chart selected and scroll it into view.

The physical folder is not inserted into the library sidebar. While the
temporary scope is active, the sidebar has no selected category. Selecting any
sidebar item clears the temporary folder constraint and loads that category
normally. Opening `Show Same Folder` again from a chart in the scoped list
replaces the constraint with that chart's exact parent.

If no valid folder can be derived, the list remains unchanged and the menu
simply closes.

## State and Data Flow

The main-menu scene will hold the optional temporary folder constraint and the
context menu's open state. Chart-query construction will return a folder-only
base query while that constraint is present, then apply the retained sort. A
sidebar selection clears the constraint before it constructs the selected
category's normal query.

`ChartMetaQuery` will carry an optional exact-folder value. Repository query,
count, paging, and index lookup paths will all apply and bind the same folder
predicate so the lazy chart-list cache remains consistent and can restore the
selected chart accurately.

## Verification

- Add component tests for action availability, selection, toggle behavior,
  outside dismissal, Escape/Back dismissal, overlay lifetime, and edge-aware
  placement.
- Extend repository tests with ordinary and archive-style folder values to
  prove exact-folder matching, rejection of descendants and sibling archive
  paths, consistent counts, paging, and index lookup.
- Add focused main-menu library/state tests for deriving an exact folder,
  clearing search and filters while retaining sort, temporary-scope precedence,
  and clearing the scope on sidebar selection.
- Run the focused context-menu, repository, and main-menu library tests.
- Build the desktop `main` target with the existing `cmake-build-debug`
  directory.

