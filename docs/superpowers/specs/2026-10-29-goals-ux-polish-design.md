# Goal UX polish

User-approved scope: implement all improvements in the 2026-10-29 chart/folder goal UX assessment using parallel agents. Preserve the shared goal-kind cycling button and desktop vertical tabs.

## Behavior

- Back follows actual entry: a single goal opened from a selector returns there; multiple-goal detail returns to its source picker and then the selector; overview-origin detail returns to its original tab/page/scroll. All goals is an explicit destination. Chart-specific recommendation Back follows the same real-origin principle.
- Folder flags use the same zero/one/many routing as chart flags, including Average EX goals. New goals and editing are explicit operations.
- Goal rows are wholly clickable, target-first, with scope and status as labels, current/target progress, and a detail affordance. Source pickers show unfinished goals first and active/achieved counts.
- Editors have Save and Cancel. Field changes preserve entered values. Cancel/Back from an existing editor restores its detail; cancelling new goals restores the actual source. Existing folder source/mode/type identity is immutable during editing (labels replace navigation controls); Add goal supplies editable scope/type, preserving draft targets when changed. A new folder goal must not silently replace an existing occupied scope/type slot.
- Details prioritize current/target progress and the next action. Select chart replaces PLAY. Practice is secondary; management actions are compact and removal uses destructive styling. Dates/evidence appear after the main actions.
- Compact screens place goal-kind and primary status navigation above full-width content; recommendation target navigation must not create an unbounded header. Desktop keeps vertical navigation and centered content.
- Preserve per-tab page and scroll across section switches and folder rebuilds. Empty states distinguish no goals, no active goals, and no achieved goals, with Browse charts/folders or View achieved actions.
- Add a concise goal-kind scope cue without replacing its cycle control.

## Constraints

Use current checkout; no worktrees, deployments, whole-file formatting, or new dependencies. No native app visual verification claim without successful capture. Preserve profile guards, safe-area handling, source metadata-only browsing, and goal persistence/credit semantics. Native screenshot access currently fails with cgWindowNotFound; validate layout geometry and behavior with focused tests and the desktop build.
