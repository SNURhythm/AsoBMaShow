# Selector layout refinements

Apply the eleven requested refinements without changing shared-tab behavior.
Remove the redundant search-row folder/count labels and duplicate detail
favorite and records overflow controls. Hide the wordmark in portrait in both
Yoga layout and rendering. Let long level/key labels grow left with right
alignment. Keep all chart details open, size stagefile artwork to its decoded
aspect ratio with a landscape placeholder, and preserve the title overflow for
Viewer/Reveal actions. Replace the dock key-mode cell with assist options, use
uppercase captions and a larger PLAY OPTIONS heading without its left rule.
Remove bottom safe-area padding from both the dock and Play control.

Validate native view geometry, async artwork resizing, portrait rotation,
right-aligned long labels, options/localization and existing navigation behavior.
Build once per directory, review the final changes, capture the native UI, and
commit/push the task files to the current upstream. No deployment.

Completed: implementation, desktop build, 483-test full suite, final focused
five-target rerun and review. Native screenshot capture was blocked by the
locked Mac; see design-qa.md for validation limits.
