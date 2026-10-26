# Streak (MVP)

Open **Streak** in the built-in PLAY selector, then create a routine. Set daily,
weekly and monthly point goals and add assignments. Select levels from any
installed difficulty tables; an assignment matches the union of those levels.

An assignment filters the chart's previous best lamp, then requires the new
attempt to meet its target lamp and score together. Either target may be left
unrestricted, but at least one must be set. Score targets cover E, D, C, B, A,
AA, AAA, MAX -, MAX, and custom EX rates to 0.01 percentage-point precision.
Previous-lamp filters support exact lamps and “or below,” including NO PLAY.
The target lamp is a minimum; NO PLAY is an eligibility state, not a result.

Before the daily goal, completed rounds award their configured points, capped
at the points still needed for the goal. The play that reaches the goal awards
no extra savings, even if it completes several assignments. Later rounds award
**1 bonus point each**, with a hardcoded **5 bonus points per assignment per day**.
An assignment stops progressing when it reaches that cap.
Each chart counts once per assignment per local calendar day, even if present
in several tables or played in a different LN mode. A play can contribute to
several assignments. After the daily point goal is reached, a completed round
randomly raises one enabled requirement for its next round: chart count +1,
the next score rank (custom rate +2 percentage points), or the next lamp.
The goal-crossing round also raises its next requirement. Tomorrow restores
the original requirements and resets the bonus limit. Score/lamp increases stop at MAX/Full Combo and
chart counts at 10,000. An exhausted chart pool may require another assignment.

## Daily outcomes

- Reach the point goal: advance the streak once.
- Play a manual single-chart attempt without reaching the goal: protect the
  streak without advancing it. Failed and ineligible charts count as attendance.
- Do not play: automatically use one owned freeze, or reset the current streak.
- Autoplay, replay viewing and practice do not count. Course progress is outside
  this single-chart MVP.

Streak length counts achieved goal days separated by protected days, rather
than consecutive calendar dates. Dates use local midnight; weeks start Monday.
The first routine starts today. All subsequent routine edits apply tomorrow,
even before the first play; another save replaces the pending changes. Already-earned points are never recalculated when editing a routine.

Only post-goal bonus points become savings. Freeze prices start at **20 points**
and rise by **10 per purchase** (20, 30, 40, …), with room to purchase up to two.
A streak reset returns the price to 20; using a freeze or protecting a day by
playing does not reset the price. Spending never reduces
earned daily/weekly/monthly totals. No real-money transactions exist.

Milestones at 7, 14, 30, 50, 100 and 365 goal days, completed weekly/monthly goals,
and the best streak survive resets. Daily progress and streak acknowledgment
appear in built-in results. Rich celebrations, sound, share cards, eligible-chart
navigation, and custom-skin integration are deferred to the UI/UX pass.

## Storage and recovery

Streak state belongs to the active profile's score database and follows normal
profile backup/export. Versioned state is updated in the same transaction as a
new projected result. Retrying a save or recalling a result cannot award twice.
A late save uses its original play date. An unnecessarily used freeze is returned
and moved to a later missed day when needed; otherwise it returns to inventory.
Refunds may temporarily exceed the two-freeze purchase capacity. Prices reset
when a newly closed day breaks the current streak, never when a late save moves
an already observed historical gap.
Version 1 state migrates without changing historical points, balances or progress;
new bonus and purchase counters start at zero. Version 2 persists those counters.

Eligibility follows existing normalized chart identity matching (SHA-256, MD5,
and legacy paths), with records ordered before the current attempt. Imported IR
lamps participate when eligible for best records. Modified manual plays count,
using the same effective, assist-capped lamps as normal score display. They may
earn assignments whose targets they meet. Only local saved attempts earn points.

`streak/StreakModel.*` owns calendar and assignment rules. `streak/StreakStore.*`
owns versioned persistence and indexed difficulty metadata lookup.
`ScoreRepositoryStreak.cpp` exposes configuration/dashboard actions, and
`SaveProjectedScore` applies rewards atomically. `StreakScene.*` provides the
native dashboard and editor.

## Verification

Build `main`, `tests_results`, `tests_ui`, `tests_profiles`, and
`ir_score_import_projection_tests`. Run their CTest labels plus the projection
test. New coverage includes actual Scene deferred-callback execution, score
thresholds, distinct-chart repeat rounds, streak/freeze/calendar handling,
damaged state rejection, legacy identities, multi-table deduplication and
transaction rollback. Economy regressions cover next-day edits before play,
simultaneous goal-reaching rounds, daily bonus limits, escalating prices across
restarts, price resets, late attendance and version 1 migration. Native visual verification requires a desktop session
that exposes the game window to automation.
