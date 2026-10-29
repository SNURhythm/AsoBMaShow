# Goal navigation layout

Make the goal sidebar narrower, keep an 8-unit gap to the main view, and center
the main view in the available safe area whenever a 960-unit body and sidebar
fit. Smaller layouts give the body the remaining space. A single cycling button
switches Chart goals / Folder goals above their vertical tabs, using the existing cycle-button component and goal-tab styling.
Each section remembers its selected vertical tab. Chart-specific recommendations
retain their dedicated navigation and Back behavior.

- [x] Share section state and responsive column geometry between both goal scenes.
- [x] Add reciprocal folder/chart tab switching, preserving selector callbacks,
  return targets, profile guards, and deferred scene replacement.
- [x] Cover wide/narrow geometry and tab handoffs, then run both goal-scene test
  suites and the desktop build.

Implementation uses `src/goals/GoalNavigation.h` for section state and column
geometry, the two goal scenes for rendering and deferred navigation, and the
existing selector callbacks for returning to chart/folder selection. Extend the
existing scene fixtures; no new test executable or deployment is needed.

Verified with the desktop `main` build and the chart-goal, folder-goal, and
localization CTests (3/3 passing). Geometry and section-cycle tests cover safe
centering, constrained widths, remembered tabs, selector callbacks and profile
handoffs; the existing dedicated-recommendation Back regression also passes.
