# Personal chart goals

Personal goals keep a chosen chart lamp and/or score target across sessions.
They have no deadline, automatic escalation, or points. Kombo routines continue
unchanged and are not required to use goals.

## Using goals

- In PLAY, tap the small flag on a chart row in the built-in selector, or choose
  **Personal goal** from the custom selector’s chart-actions menu. The row flag
  is filled when that chart has a goal. It opens the existing goal for the
  current LN mode, or another configured mode if none exists for the current
  mode. An empty flag opens a new goal editor. For charts with selectable long
  notes, the LN, CN and HCN buttons on goal details open or create each mode’s
  separate goal.
- The favorite star is beside the chart title in the detail panel.
- The **Goals** folder in either selector gathers indexed charts with goals
  across LN modes with at least one unfinished goal. Normal search and filters apply.
- Choose a minimum successful lamp, a score rank (E through MAX), a custom EX
  rate to two decimal places, or both. If both are chosen, both must be met.
- Open **Personal goals** from Kombo to browse goals, including achieved ones,
  in pages of 20. This entry is available before creating a Kombo routine.
- Goal details show independent best lamp and best score. Score gaps use exact
  EX integers, rounded up to the next achievable point at a fractional threshold:
  AAA on a 2000-point chart needs 1778 EX. Without a valid score maximum, the gap
  remains unknown.
- Goal details display **Set** and **First achieved** dates in local time.
  Saving an unchanged target preserves its dates; changing the target starts a
  new timeline. Goals created before date tracking show “Not recorded” for an
  unknown set date. New or changed targets must not already be achieved by
  eligible records; imported records count when evaluating targets.
- A newly achieved goal shows a short flag-burst celebration on the result
  screen after the score is saved. It waits for the Kombo panel to close and
  does not replay for later clears, view rebuilds, replay, practice, or autoplay.
- **PLAY** selects the chart and its LN mode in the originating selector.
  It does not start gameplay. **Practice chart** opens the existing chart viewer
  and practice tools, with a return path to the goal.
- Edit or remove goals at any time. Removal asks for confirmation and preserves
  all score records.

## Records, scope, and missing charts

Goals are profile-specific and keyed by chart SHA-256 and effective long-note
mode. Selectable LN/CN/HCN charts have separate goals; authored LN modes use the
fixed scope; charts without long notes share one goal across mode selections.
Changing the chart content at the same path cannot transfer a goal to it.

Existing eligible best records count immediately, including accepted imported
scores and lamps. Lamp and score may come from different plays. Replay-facing
best-clear queries remain local-only; goals explicitly opt into imported lamp
evidence, consistent with the selector lamp cache. Goals do not change score
eligibility, so practice, replay, and autoplay cannot create ordinary bests by
using this feature.

Achievement is a projection of current records, not a permanent badge. Importing
or deleting scores can change progress. Returning to goals refreshes it. The
first achievement date remains as history even if current records no longer
meet the target.

A removed/unavailable chart keeps its stored title, subtitle, targets, and
record-based progress. Editing and removal remain available; play and practice
are disabled until the chart resolves in indexed library metadata. No archive
payload scan is needed for goal browsing or resolution.

## Implementation and verification

Versioned rows in the profile score database's `chart_goals` table retain only
identity, display and LN metadata, targets, and goal dates. They use repository locking,
profile activity guards, transactional writes, and bounded page reads. Corrupt
or future-version rows produce an error and cannot be silently overwritten.
Switching profiles discards unsaved goal edits and queued navigation.

Behavioral coverage is in `chart_goal_tests`, `ir_score_import_projection_tests`,
and `chart_goals_scene_tests`. The rationale and limitations of the motivation
research are in [the ecosystem report](../research/rhythm-game-motivation-2026-10-28.md).

The goal editor shows the current best lamp and EX score before saving, including eligible imported records for the selected LN scope.

New achievements play a short trumpet-style fanfare when the result banner appears, using the existing system sound bus and master volume.

Flags are green when the goal they open is achieved. Achieved-only charts are
excluded from the Goals folder. A
chart remains in that folder while any LN-mode goal is unfinished. In Personal
goals, unfinished goals appear first; achieved goals are collapsed at the bottom
behind the same plus/hide control used by Kombo optional challenges. Each group
pages independently. Titles and subtitles share one heading.

Goal lists, details, and the editor show the chart jacket (or banner), with a
music-note placeholder while artwork is loading or unavailable. Artwork for
visible goal pages is resolved from chart metadata and loaded asynchronously.
