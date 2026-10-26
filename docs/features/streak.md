# Combo (MVP)

Open the **Combo** tab beside PLAY, COURSE, MUSIC and IR, then create a routine.
Custom selectors expose the same dashboard through the toolbar’s flame button. Set daily,
weekly and monthly point goals and add assignments. Select levels from any
installed difficulty tables; an assignment matches the union of those levels.

New routines default to **20 daily / 150 weekly / 650 monthly** points, with
**10 points per round**. Two default rounds meet the daily goal. Daily goals
alone total 140 per week and 560–620 per month, so the longer goals require
some optional bonus play. Saved routines retain their chosen goals.

An assignment checks selected table levels, previous best lamp and previous best
score rank. All selected eligibility conditions must match.
Once a chart satisfies all eligibility conditions, it stays eligible for that
assignment for the rest of the day, even if its lamp, score or table membership changes.
An initially ineligible chart can qualify on a later attempt. From that point,
its best lamp and best EX rate accumulate across attempts: the lamp and score
targets can be achieved on different plays. Earlier ineligible attempts do not
contribute retroactively. Daily eligibility and achievements survive app restarts
and reset the next day. Either target may be left
unrestricted, but at least one must be set. Score targets cover E, D, C, B, A,
AA, AAA, MAX -, MAX, and custom EX rates to 0.01 percentage-point precision.
Previous-lamp and previous-score filters support exact values and “or below,”
including NO PLAY. Score ranks include F through MAX; “or below” also includes
charts without a previous score. Best scores use the same LN-mode selection as
the selector and exclude the current attempt and later-dated results during
recovery. Existing routines migrate with no previous-score restriction.
Lamp options and summaries use the shared colored vertical lamp indicators.
The target lamp is a minimum; NO PLAY is an eligibility state, not a result.

The PLAY selector's **Assignments** folder shows all playable charts that can
still contribute today, deduplicated across assignments. Expand it to browse a
subfolder for each assignment. Charts already counted for an assignment are
hidden there; they remain available for other assignments that can still use
them. Sticky-eligible charts stay visible even after their lamp, score or table
membership changes. Lamp/score targets are goals to work toward, not filters on
this list. Once the shared daily bonus budget is spent, the folders are empty.
Both the built-in browser and skin-driven selector support these folders, with
normal search/filter/navigation behavior and refresh after play or day rollover.

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
targets and resets the shared bonus budget. **Edit for tomorrow** is the normal
editing flow. Only when fewer distinct eligible, playable charts remain than
needed to finish the current round does **Adjust today** appear on that
assignment. Difficult targets alone do not enable immediate editing. Levels,
eligibility, targets and quota change immediately for today. Rewards, used charts, partial achievements and points are retained;
editing itself awards nothing. A lowered quota still needs another distinct
qualifying chart. The regular routine and pending changes stay unchanged.
Today's points per round and daily goal cannot be raised through this action.

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
and the best streak survive resets. The native dashboard uses animated meters,
a flame, progress cards and milestone badges. The flame is gray and still before
the first play, small and animated after playing, and larger and more energetic
once the daily goal is reached. Daily progress
opens automatically in a dismissible slide-over after a fresh manual chart's
successful save, including successful save retries and custom result skins.
It shows saved daily totals rather than claiming a per-attempt point increase.
Escape or tapping outside dismisses it without activating result controls;
custom-skin auto-exit pauses until dismissal. Resizing never reopens it. Completed days lead with “Today is complete” and a
“Done for today” action. Optional challenges start collapsed, and bonus points
appear separately from the finished daily goal. Short Earshot-generated progress, completion and freeze-purchase cues use the
system audio bus and master volume; they do not repeat on redraws or failed
purchases. Progress and round animations are finite. The flame animates while a played
day is visible; an unplayed day stays still. Share
cards remain deferred.

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
Open **Combo**, set the daily goal to **1**, and add an assignment with
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

To check assignment navigation, select the flame-marked **Assignments** heading in PLAY. The root shows
the union of eligible charts; each named subfolder shows its own candidates.
Complete a chart for one assignment and return: it should disappear from that
subfolder, remaining in another only if it can still contribute there. Search,
filters and random selection must stay within the current assignment folder.
The same folders are available in a skin-driven selector.

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

### Motion and immediate-adjustment checks

- Complete a manual chart with a configured routine: saved progress opens once
  over built-in or custom result skins. Dismiss with the button, Escape or an
  outside tap; the gesture must not activate the result behind it.
- Rotate while open or after dismissing. The drawer fits the new viewport and
  dismissal stays remembered. Autoplay, replay, practice and history browsing
  do not open it. A successful score-save retry may open it once.
- Verify **Adjust today** appears only when the remaining eligible chart pool
  is too small. Verify points and counted charts remain, the new requirement applies now, and tomorrow retains
  the regular routine. A stale editor crossing midnight must refuse the save.
- Listen for the short progress/completion cue and successful freeze-purchase
  cue. Master mute silences them; redraws and rotation do not replay them.

Assignment meters animate from the previous saved count. A completed round fills
to its original quota, holds a “Round complete” state, then reveals the next
round with a separate promotion cue. The result drawer includes these rows even
when the play reaches the daily goal. Animation receipts are bounded, transient,
created only after a successful score transaction, and claimed once by the
result drawer or dashboard. The result drawer opens only when the saved run
advances at least one assignment; unrelated runs leave the result screen
uninterrupted. Rebuilds retain the animation clock.
