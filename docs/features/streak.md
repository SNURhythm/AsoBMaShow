# Streak (MVP)

Open **Streak** in the built-in PLAY selector, then create a routine. Set daily,
weekly and monthly point goals and add assignments. Select levels from any
installed difficulty tables; an assignment matches the union of those levels.

An assignment checks selected table levels and the chart's previous best lamp.
Once a chart satisfies all eligibility conditions, it stays eligible for that
assignment for the rest of the day, even if its lamp or table membership changes.
An initially ineligible chart can qualify on a later attempt. From that point,
its best lamp and best EX rate accumulate across attempts: the lamp and score
targets can be achieved on different plays. Earlier ineligible attempts do not
contribute retroactively. Daily eligibility and achievements survive app restarts
and reset the next day. Either target may be left
unrestricted, but at least one must be set. Score targets cover E, D, C, B, A,
AA, AAA, MAX -, MAX, and custom EX rates to 0.01 percentage-point precision.
Previous-lamp filters support exact lamps and “or below,” including NO PLAY.
The target lamp is a minimum; NO PLAY is an eligibility state, not a result.

Before the daily goal, completed rounds award their configured points, capped
at the points still needed for the goal. The play that reaches the goal awards
no extra savings, even if it completes several assignments. Later rounds award
**1 bonus point each**, sharing a hardcoded **10-point daily budget** across all
assignments. There is no per-assignment cap: one assignment can earn all ten.
All assignment progression stops when the shared budget is spent. Simultaneous
completions use stable assignment-ID order.

Each chart counts once per assignment per local calendar day, even if present
in several tables or played in a different LN mode. A play can contribute to
several assignments. After the daily goal, **every assignment uses exactly one
new qualifying chart per bonus round**, regardless of its normal chart quota.
Unfinished pre-goal rounds switch to this rule too; their partial counts do not
grant automatic bonus points, and already-counted charts remain used for today.

Completed rounds at or after the goal may randomly raise the next score rank
(custom rate +2 percentage points) or lamp, but only when the finishing play
already meets the proposed target. Otherwise the target stays unchanged. Bonus
chart counts never increase. Tomorrow restores all configured chart counts and
targets and resets the shared bonus budget. An exhausted chart pool may require
another assignment.

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
even before the first play; another save replaces the pending changes.
Already-earned points are never recalculated when editing a routine.

Only post-goal bonus points become savings. Freeze prices start at **20 points**
and rise by **5 per purchase**, capped at **50** (20, 25, 30, …, 50), with room
to purchase up to two.
A streak reset returns the price to 20; using a freeze or protecting a day by
playing does not reset the price. Spending never reduces
earned daily/weekly/monthly totals. No real-money transactions exist.

Milestones at 7, 14, 30, 50, 100 and 365 goal days, completed weekly/monthly goals,
and the best streak survive resets. Daily progress and streak acknowledgment
appear in built-in results. Completed days lead with “Today is complete” and a
“Done for today” action. Optional challenges start collapsed, and bonus points
appear separately from the finished daily goal. Rich animated celebrations,
sound, share cards, eligible-chart navigation, and custom-skin integration are deferred to the UI/UX pass.

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
Existing days above the shared bonus budget keep their earnings but cannot earn
additional bonus points. Older partial bonus rounds switch to the one-chart rule
on the next saved play, without credit for their partial counts.

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

## Quick manual test

Use a temporary player profile so the test routine does not affect normal progress.
Open **PLAY → Streak**, set the daily goal to **1**, and add an assignment with
broad difficulty-table levels, **Any previous lamp**, **FAILED or above**,
**Any score**, **1 chart** and **1 point**. For a quick milestone check,
set weekly/monthly goals to **3/5** while creating the routine; bonus points
should unlock those milestones during the same session.

1. Finish and save a manual eligible chart. Today should complete, the streak
   should advance once, and bonus savings should remain zero.
2. Reopen Streak. Confirm the completion panel and **Done for today** action;
   optional challenges should initially be collapsed.
3. Open the optional challenges. Play different eligible charts to complete
   rounds. Each uses one chart and pays one bonus point, up to ten total.
   Replaying a previously counted chart should add nothing. Score/lamp targets
   must not jump above the result that finished the previous round, and chart
   count must stay fixed at one for all bonus rounds.
4. Reach the bonus limit. The dashboard should mark bonuses complete without
   showing another unfinished round. Further play must not increase savings.
5. To check overlapping assignments, repeat in another test profile with three
   assignments using the same levels. They must share ten bonus points, even
   when one play completes several rounds.
6. Edit the routine before or after playing. Today stays unchanged, and the
   dashboard shows the effective date for tomorrow. Multiple saves replace the
   pending routine.

To check daily eligibility and combined targets, use a fresh test profile with
an assignment for an EASY-cleared chart, target HARD and a score threshold you
can reach. First finish NORMAL with the target score; it should not count yet.
Then finish HARD with a lower score. It should count once, using the earlier
score, despite the previous lamp now being NORMAL. Restart between attempts to
check persistence. The next day, the previous-lamp filter applies afresh.

At the default ten-point daily bonus maximum, the first freeze needs two fully
completed bonus days. Check prices of 20, 25, 30 and the eventual 50-point ceiling.
For midnight rollover, missing-day protection, price resets and delayed saves,
use the automated model/store tests below instead of changing the system clock.
Only one build may run in this checkout at a time.

```sh
cmake --build cmake-build-debug --target streak_model_tests streak_store_tests -j 6
ctest --test-dir cmake-build-debug -R '^streak_(model|store)_tests$' --output-on-failure -j 6
```

## Verification

Build `main`, `tests_results`, `tests_ui`, `tests_profiles`, and
`ir_score_import_projection_tests`. Run their CTest labels plus the projection
test. New coverage includes actual Scene deferred-callback execution, score
thresholds, distinct-chart repeat rounds, streak/freeze/calendar handling,
damaged state rejection, legacy identities, multi-table deduplication and
transaction rollback. Economy regressions cover next-day edits before play,
simultaneous goal-reaching rounds, daily bonus limits, escalating prices across
restarts, price resets, late attendance and version 1/2 migration. Daily progress
regressions cover eligibility changes, separate target achievements, raised
targets, delayed saves and restart/rollback behavior. Native visual
verification requires a desktop session that exposes the game window to
automation.
