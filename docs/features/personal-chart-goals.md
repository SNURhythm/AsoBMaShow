# Personal chart goals

Personal goals keep a chosen chart lamp and/or score target across sessions.
They have no deadline or automatic escalation, and a Kombo routine is not
required to use them. With a routine active, newly achieved goals from normal
local play count toward its permanent **Achieve goals** assignment. Set its
goals-per-round quota when creating the routine or editing tomorrow's routine.
Regular rounds grant 10 points up to the remaining daily target; extra rounds
use one newly achieved goal for 1 point within Kombo's shared daily bonus limit.
Previously achieved goals and imported records do not grant retroactive points.
Separate LN-mode goals and newly set harder targets count as separate achievements;
a goal containing both lamp and score targets counts once when both are met.

## Recommendations

The Personal goals screen offers expandable **Near E** through **Near MAX**
groups for best scores within 2 percentage points below their next rank. Empty
groups are hidden. Candidates are ordered by EX points remaining, nearest first, with 20 per
page. Tapping a recommendation opens the normal goal editor with that rank
selected; saving remains explicit. Existing lamp targets are preserved. LN/CN/HCN
evidence is combined into one chart card per target group. The editor provides
a mode picker for selectable-mode charts, initially using the closest evidence;
changing the mode keeps the draft targets without saving.

Candidates come from the profile's best-score summary, with indexed chart-hash
lookups to verify library membership and LN scope. Historical plays are not
scanned, duplicate installations do not duplicate candidates, and chart metadata
is loaded only for the displayed page. Recommended, active, achieved, and detail
cards show jacket artwork and a tinted difficulty-table label (or chart level
when no table label is available).

Lamp recommendation groups use the recorded evidence for Assisted Easy, Easy,
Normal, Hard, and EX Hard. Groove gauges qualify within
**5 gauge points** below their ruleset/profile's clear border; full-chart
background clears also qualify. Survival gauges qualify when they first died in
the **final 10% of notes**, or survived the complete chart in the background.
Incomplete plays cannot supply final groove values or claim complete survival.
Full Combo recommendations use the lowest **combo-break count**, independently
of Hazard, and qualify with **1–5 combo breaks** in a completed eligible play.
They are ordered by fewest breaks and show that count on the chart card.

Each group hides lamps already earned and sorts by closeness: gauge points below
the border for groove gauges, remaining chart proportion after death for survival
gauges. Selecting a candidate prefills the lamp in the normal editor and preserves
any score target. Background evidence is a recommendation, not an earned lamp.

Only eligible, unmodified local chart plays with recorded evidence contribute;
unknown historical evidence, practice/autoplay/assisted plays, altered playback,
and course stages do not. A per-profile summary retains the best evidence for
each chart/LN mode/lamp (five gauge rows plus one best combo count), updated
atomically with score inserts. Existing Hazard proximity rows are ignored.
Existing profiles start with empty gauge summaries. The version-15 migration
backfills qualifying counts (1–5 breaks) once from eligible, completed local scores, independently
of whether gauge evidence was recorded. It preserves better existing combo bests,
keeps LN modes separate, and records completion in the same transaction so
subsequent startups do not rescan history. Imported scores and unreadable or
ineligible provenance are excluded; the existing explicit ruleset compatibility
exception also applies. Browsing uses these indexed
summaries and the existing best-lamp cache, without scanning play history or replay
files. Grade-family gauges are course evidence and do not create chart goals.

New plays calculate every gauge, including when Gauge Auto
Shift is off. Each survival gauge stores the passed-note count at its first
death, or **-1** if it never died during the played portion. Each groove gauge
stores its final calculated value. The evidence also retains how many notes
were actually passed, so surviving an aborted portion is not mistaken for
surviving the whole chart. Abort cleanup does not overwrite this evidence.

Historical records have unknown gauge evidence and cannot supply gauge recommendations;
their saved combo-break counts can supply Full Combo recommendations.
Only numeric bests remain in the score database. In-flight chart saves temporarily
keep typed numeric evidence in the replay database's pending-save tables, with
integrity validation, so recovery can finish the score and summary transaction.
Acknowledging that save deletes the temporary evidence. Completed chart and course
result history does not store a gauge-evidence payload, and migration does not
backfill gauge evidence.

## Using goals

- In PLAY, tap the small flag on a chart row in the built-in selector or the flag
  in the custom selector toolbar. The row flag is filled when that chart has a goal.
  It opens the existing goal for the current LN mode, or another configured mode if none exists for the current
  mode. An empty flag opens a new goal editor. When a selected chart has an
  unfinished goal, a brief animated bubble above its flag shows the lamp
  target with its clear-lamp color and the score target. For charts with
  selectable long notes, the LN, CN and HCN buttons on goal details open or
  create each mode’s separate goal.
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

Target bubbles center above the flag, with the lamp bar and label centered
together and the score centered beneath them. The native
selector draws its bubble above the list clip; safe-area edges remain respected.

## Folder goals

Use a folder or difficulty-table flag to set a goal for the entire indexed
folder subtree, table, or level. In the built-in selector, table and level rows
have flags; **Folder goals** also opens the current physical folder when browsing
one. In the custom selector, the toolbar flag supports folders, tables and levels.
**Personal goals → Folder goals** lists all saved folder goals.

Two independent targets are available for each folder and selected LN mode:

- **Lamp coverage:** a percentage of charts at a chosen lamp or better.
  **100% Failed or better** covers every available chart with an eligible play.
- **Average EX rate:** the average of each available chart's best EX percentage,
  with equal chart weights. Available unplayed charts count as 0%.

Missing charts are excluded from both denominators and shown separately.
Duplicate installations count once. Membership is live, so downloading charts,
rescanning, or updating a table can change progress. Selector search and filters
do not change a saved goal's scope. Empty or unavailable folders cannot complete
a goal. Goals keep their source identity if the table is reimported.

Cards and details show a progress bar, the current percentage, a target marker,
and available/missing counts. Lamp goals also show the qualifying count and the
number needed. Edit the percentage to preview its target marker; use **Open
folder** to return to the goal's collection. New targets must exceed current
progress. The two goal types and LN modes have separate targets.

Eligible imported records contribute to progress. Only a newly completing local
play grants the result celebration and Kombo goal credit. Imports, membership
changes and revisiting a completed target do not award points. First-achieved
history remains even when live progress subsequently drops.
