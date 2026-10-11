# Song selector and navigation QA

Date: 2026-10-11. Result: passed with the validation limits below.

This follow-up implements the user's explicit changes to the earlier selector
reference. The existing light/dark typography and palette remain; the requested
navigation and row structure supersede the earlier mockup's column heading.

## Requested changes

- Removed the list heading and numeric index column.
- Title and artist share one vertically stacked column at every width.
- Clear state uses the thin left vertical lamp; score and favorite remain visible.
- Details reserve jacket space with a music placeholder for absent, loading or
  failed artwork, including an empty selection.
- The bottom surface reaches the viewport edge. Dock controls retain bottom
  safe-area padding; portrait details extend behind that inset.
- PLAY omits course folders. COURSE displays the former Courses children as
  top-level folders and uses the same chart/details interface.
- MUSIC and IR retain their existing interfaces under the shared four-tab
  header. Ctrl/Cmd+1 through 4 switch tabs; Escape from a utility returns to PLAY.

## Visual checks

The final compiled native app was opened on all four tabs. COURSE showed only
course-table roots, MUSIC showed its library/player navigation, and IR showed
its existing provider/upload controls. Returning from IR restored PLAY.

Final dark screenshots:

- `/tmp/asobmashow-tabs-play.png`
- `/tmp/asobmashow-tabs-course.png`
- `/tmp/asobmashow-tabs-music.png`
- `/tmp/asobmashow-tabs-ir.png`

The screenshot library has no fabricated scores. Empty detail artwork and
missing course charts use the real placeholder/warning states. MUSIC and IR
keep their existing inner styling; their shared selector header is new.

## Functional checks

The desktop app and affected native targets built successfully. All 21 affected
CTest targets passed, including production-method tests for eight tab/folder
and chart-handoff scenarios, real Yoga rotation/narrow-header/safe-area layout,
row/details geometry, preview ownership, music/video lifecycle, IR preparation,
settings return ownership, profile ownership and localization.
The complete CTest suite subsequently passed all 483 tests in parallel.

Review fixes preserve separate PLAY/COURSE selection and scroll state, restore
pathless course-start/archive rows, end hidden text editing, avoid redundant
per-frame layouts, refresh MUSIC after PLAY changes, retain the shell across IR
Settings, and prevent recursive All Songs fallback while in COURSE.

## Limits

Native desktop tab navigation was exercised through keyboard input because
mouse automation on this machine can deliver stale SDL pointer coordinates.
Settings retention and background preparation are covered by extracted native
lifecycle tests rather than an end-to-end upload. Portrait and bottom safe-area
behavior were verified with layout tests, not a mobile device build. No upload,
deployment, real score submission or preference change was performed.
