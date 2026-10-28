# Personal chart goals

## Intent and authorization

The user asked for rhythm-game ecosystem research and subsequently authorized
implementation when the research was sufficient. The selected first direction
is persistent personal chart goals. This selection is the agent's product
judgment, not a claim that the user explicitly picked the feature.
See [research and tradeoffs](../../research/rhythm-game-motivation-2026-10-28.md).

## Behavior

- One goal per chart SHA-256 and effective long-note mode, within the active
  profile. A goal has a minimum lamp and/or minimum score rate. At least one
  target is required. Lamp targets start with a successful assisted clear;
  FAILED and NO PLAY are not goal achievements.
- Score choices use E through MAX, including MAX -, plus custom EX rate to
  0.01 percentage points. Integer rational comparisons avoid rounding near
  thresholds. No claim that score ranks measure overall skill.
- Goals persist across days and sessions with no deadline, automatic target
  escalation, points, notifications, or new streak. Combo remains independent.
- Existing eligible best results count immediately, including imported scores
  wherever the normal best-record queries admit them. Lamp and score can be
  established by different plays. Practice/replay/autoplay do not invent new
  eligible best records. Progress is a live projection of current best records,
  not a permanently latched badge; deleting/importing records can change it.
- Show best lamp separately from best score, and show exact EX points remaining
  when a trustworthy max score is available. An unknown max score must not be
  shown as zero remaining or complete. No estimated chance of clearing.
- Show achieved goals alongside unfinished ones; players can edit or remove
  them. Removal affects only the goal, not any scores. Confirm removal.

## Entry points and navigation

- Both native and custom-skin selectors offer a Personal goal action for the
  selected available chart in the chart-actions menu.
- Combo offers Personal goals even before a routine is created. It opens the
  same standalone scene with a paged list (20 goals per page). Goals do not
  require a Combo routine. The detail view displays the chart subtitle and
  effective LN scope, targets, and current progress.
- An Open in PLAY action returns to the originating selector with that exact
  chart and LN mode selected, ready for normal gameplay; it never starts play
  automatically. The originating selector owns its normal launch flow.
- A Practice action reuses ChartViewerScene and returns to the goals scene.
  Do not treat practice results as ordinary goal-clearing scores.
- Missing library charts retain their stored title, goal, and record-based
  progress. Editing/removal remains available; play/practice are unavailable.
  Resolve charts by hashes from indexed metadata, allowing relocation without
  loading or scanning archive payloads.
- Use the existing theme, scrollable responsive layout, dropdown controls for
  targets, shared lamp colors, and all five existing UI locales. Back/Escape
  return safely; deferred view rebuilding avoids deleting controls mid-callback.
- Follow-up: a chart-row flag opens the configured goal (current LN mode first,
  then another configured scope); the favorite star moves beside the detail title.
  A Goals folder aggregates indexed charts with unfinished goals in both selectors.
  Achieved goal flags are green; achieved goals are collapsed below unfinished
  goals with Combo-style plus/hide controls and independent pagination.
- Follow-up: retain and display set/first-achieved timestamps. Changed targets
  start a new timeline; unchanged targets preserve dates. Goal completion during
  a saved play produces one finite result banner, delayed until Combo closes.
  It does not repeat for practice, replay, autoplay, or scene rebuilding.

## Storage and boundaries

Store versioned goal rows in the profile score SQLite database, using the
ScoreRepository lock and profile activity guards. Like the existing independent
streak state, the goals table is created lazily without changing the legacy
score table schema. Rows are keyed by normalized chart SHA-256 and effective
LN mode and contain a versioned JSON body with only necessary chart metadata
(hashes/title/subtitle/LN metadata) and targets. Reads are bounded/paged; no full
library scan or replay hydration is required. Unknown/corrupt payloads fail
visibly without overwriting existing data. Writes use transactions.

The current best-score and best-clear queries are the authority on eligibility
and LN compatibility. Goal storage never writes scores. Profile changes discard
unsaved forms and old-profile navigation. Chart identity must be a canonical
SHA-256 after case normalization; a changed chart at the same path must not
inherit a goal. A chart without selectable LN behavior has one shared goal.

## Alternatives and deferred work

1. Weekly recap: less actionable until meaningful goals exist; not selected.
2. Automatic skill coaching: needs validated pattern metadata and training
   assumptions; not selected.
3. Persistent goals: familiar, explicit, independently useful; selected.

Table-wide targets, baseline/consistency analytics, rivalry, goal sharing,
and automatic chart recommendations are deferred.
No deployment, worktree creation, or changes to Combo rewards.

## Verification

Behavioral tests cover exact fractional score boundaries, unknown score maximum,
independent lamp/score records, LN identity, upsert/delete/reopen persistence,
profile separation, corrupt/future row preservation, pagination, and chart
resolution/navigation. Use focused results/music-select/UI/profile checks and
the desktop main build. Run one build at a time; preserve surrounding formatting.
Use a fresh review before final commit/push to feature/enhance-streak.
