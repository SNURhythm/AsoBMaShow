# Dropdown Overlay Portal Design

## Goal

Open dropdown menus above settings cards, controls, and footers, without being
clipped by a scrolling container. Keep menus inside the application window and
preserve existing selection, disabled-option, scrolling, and outside-click
behavior.

## Design

Add one dropdown overlay host to the settings scene root. The host is an
absolute, window-sized layer above normal content and below blocking modals.
Every settings `DropdownView` uses that host for its menu while retaining its
trigger in the normal layout.

The open menu is positioned from the trigger's absolute bounds. Placement uses
only the window safe margin: open below when it fits, otherwise above; clamp the
visible height when neither side fits; shift horizontally to stay on-screen.
Because the menu is a child of the root overlay host, settings scroll views do
not clip it and later cards or footers cannot paint over it.

The host owns at most one active menu. Opening another dropdown replaces the
active menu. Selection, an outside press, disabling the control, rebuilding the
scene, or destroying the owning dropdown dismisses it. Existing blocking
overlays keep higher z-indices and remain authoritative.

`DropdownView` keeps an optional local-menu fallback so existing non-settings
callers remain compatible. The portal interface is reusable by other scenes.

## Verification

- Unit-test window-edge placement below, above, horizontally shifted, and
  height-clamped.
- Unit-test portal activation/dismissal ownership where practical.
- Run the dropdown-focused test and compile the main application.
- Manually verify the Display/Frame cap menu opens above Apply Display and the
  settings footer and flips upward near the bottom edge.
