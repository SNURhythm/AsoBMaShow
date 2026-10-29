# Folder goals

Status: proposed design, awaiting user approval.

## Intent and agreed behavior

Extend personal chart goals with persistent goals for a folder, difficulty
table, or table level. Players can pursue coverage and score improvement across
a collection instead of setting every chart individually.

The user selected:

- Lamp coverage in place of a dedicated no-play completion goal: choose a
  minimum lamp and a percentage of charts that must reach it.
- Average EX rate, with unplayed charts contributing 0% and equal weight for
  every chart.
- Live membership: additions and removals change the denominator and progress.

Examples: 80% Easy or better; 100% Failed or better (no no-play charts);
an average EX rate of 75% across a table level.

## Approaches

Live membership was recommended and chosen because goals should describe the
current folder. Progress can decrease after a table update or score deletion.
A fixed chart snapshot would provide a stable denominator but drift from the
folder; it is not included.

Implement collection-level targets alongside chart goals. Creating individual
chart goals automatically would clutter existing goals and would not express
an average or a partial coverage target, so that approach is not used.

## Proposed defaults

The following are implementation proposals, not additional user decisions.

### Scope and identity

Support ordinary indexed directory subtrees, complete difficulty tables, and
individual table levels. Include indexed archive charts under a directory by
the same repository membership rules as the selector. Do not expand this first
version to courses, search results, recent-play lists, assignment folders,
favorites, or other transient/synthetic collections.

Directory identity uses the repository's canonical directory identity. Table
identity uses its source URL plus the exact level string when applicable, not
a transient database row ID. Store the display name for unavailable sources.
Membership ignores temporary selector search, sorting and filter settings.
State the scope clearly in the editor.

Deduplicate charts by resolved SHA-256. For unavailable table entries, resolve
the table hashes against indexed metadata; otherwise use their declared SHA-256
or MD5 identity, avoiding duplicate entries with the same known identity.
Do not infer identity from title or path when chart hashes disagree.

All current table members count, including unavailable charts. Eligible records
count when identity and LN scope can be resolved even if playback is unavailable.
Unresolved members contribute no qualifying lamp and 0% EX. Show the unavailable
count. Ordinary directories include their currently indexed descendant charts.
An unavailable source retains its goal but shows unavailable progress; it must
not silently resolve to another source. An empty source is not achieved.

Goals are profile-specific and store a selected LN/CN/HCN scope. Use the existing
chart rules for authored modes and charts without long notes. Switching the
selector's LN mode must not silently change a saved goal.

### Targets and arithmetic

Allow one lamp-coverage goal and one average-EX goal per source and selected LN
mode. Editing replaces that type's target; the two types are independent.
Use percentage input with at most two decimal places, strictly above 0 through
100 inclusive. Select lamps from Failed through the existing highest supported
chart lamp; exclude No Play and course-only lamps. Compare normalized semantic
lamp ranks, preserving the reserved legacy lamp-ID behavior.

Lamp coverage is qualifying distinct charts / total distinct members. Check
the target with integer arithmetic; required count is the ceiling of the exact
percentage times member count. Display both the fraction and percentage.
Failed-or-better means an eligible recorded lamp at least Failed, consistent
with score eligibility; it does not count autoplay/practice sessions.

Average EX is the arithmetic mean of each member's best eligible EX / maximum
EX, multiplied by 100. It is not a total-note-weighted average. Missing bests
contribute zero. A missing or invalid maximum also contributes zero, with an
explicit unknown-score-data count. Use score evidence and indexed metadata for
maxima, matching chart goals, without opening chart payloads. Do not round each
chart's rate before averaging or use rounded display values for achievement.
Choose a numerically stable aggregation with threshold-boundary tests.

Accepted imported records count. Keep existing best-score eligibility and LN
projection rules; do not scan historical attempts or replay files to calculate
progress. New targets already met by current records are rejected, matching
chart goals. Source/query failures disable saving rather than masquerading as
zero progress.

### User experience

Add a goal action for supported folder/table/level selections in both the
built-in and custom selectors, reusing the existing flag convention. Open an
editor/detail screen with source, LN mode, goal type, current value and target.
Lamp coverage adds a minimum-lamp dropdown and shows the required chart count.
The two distinct goal types use separate choice buttons, consistent with the
settings-control convention. Reuse existing UI components and visual language;
exact layout is left to implementation.

Personal goals exposes folder goals alongside chart goals with active and
achieved groups and bounded pagination. Details show set and first-achieved
dates, current counts/rate, target, missing-data counts, and a short explanation
that membership is live. Edit and confirmed removal retain all scores.
Open folder navigates to the source in the originating selector without
starting gameplay. Disable navigation when the source is unavailable.
No new recommendation system or automatic per-chart goals are required.

### Achievement and Kombo

Current achievement is a projection of live membership and eligible records.
Preserve first-achieved history when progress later drops. Unchanged saves
retain dates; changed targets start a new goal revision and timeline.

Use existing goal celebration and Kombo integration: a local eligible score
save that newly completes a folder goal revision can celebrate and contribute
one achievement. Count each type separately. Do not award again when a completed
revision drops and recrosses its target. Imports, deletions, membership updates,
opening screens and already-achieved goals never earn retroactive Kombo points.
They may update progress and first-achieved history without play credit. Keep
achievement IDs distinct from chart goals and persist deduplication atomically.

## Architecture and constraints

Add versioned folder-goal rows to the profile score database, through existing
migrations, activity guards, repository locking and transaction conventions.
Persist source identity/display data, LN mode, goal type, target, revision and
dates. Do not persist a frozen member list as the authoritative membership.
Corrupt/future-version goal data must report an error rather than be overwritten.

Build a shared membership/progress service over chart indexes and profile best
summaries. Cache by library revision, score revision, source and LN mode;
invalidate on membership/score changes and profile switches. Do not perform
per-frame aggregation or a best-score SQL query for every member. Page display
metadata separately. Keep repository snapshot/lock ordering explicit; do not
hold UI locks during large membership queries. On a local save, evaluate only
saved goal scopes that contain the changed chart, reuse membership and best
summary data, and commit achievement events through the existing save/recovery
path. Profile switches discard pending UI work and stale async results.

Archive libraries normally span tens or hundreds of gigabytes. All goal work
uses indexes and metadata, never payload scans, extraction, hashing, or a new
archive traversal. A large table may require an indexed membership aggregation;
reuse it until relevant revisions change, and keep memory proportional to
relevant indexed membership rather than archive bytes.

Likely touch points: `src/goals`, score and chart repositories,
`ChartGoalsScene`, built-in selector folder actions, custom-selector toolbar,
goal result presentation and Kombo achievement receipts. Existing chart-goal
behavior and course semantics remain intact. No parser or shader work is needed.

## Verification

Extend relevant behavioral tests for threshold rounding, Failed coverage,
semantic lamp ordering, equal-weight averages, unplayed zeroes, invalid maxima,
duplicates, missing table entries, empty/unavailable sources, live membership,
LN modes, imports and profile isolation. Cover target revision/dates and
exactly-once local celebration/Kombo credit versus non-play updates.

Exercise selector creation/navigation and personal-goal paging through existing
scene tests. Include a bounded synthetic large-membership case or query-count
assertion to catch per-chart score queries and repeated aggregation. Use focused
CTest groups from `docs/testing/README.md` and a desktop `main` compile at `-j 6`.
Review both selector integrations. No distribution or platform deployment.

## Non-goals and builder discretion

No deadlines, automatic escalation, fixed snapshots, folder-goal recommendations,
course goals, automatic chart-goal creation, or new score eligibility rules.
The builder selects class/file boundaries, schema details, caching mechanics,
pagination layout and localized control placement while preserving this
behavior. Document any material deviation before implementation.
