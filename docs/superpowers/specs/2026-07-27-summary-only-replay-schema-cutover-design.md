# Summary-Only Replay Schema Cutover Design

## Scope

This design implements delivery Slice 7 of the approved contract-first file
replay restart. Slices 1 through 6 already make modern chart and course results,
IR snapshots, BRD ownership, playback, file actions, and profile transfer
independent from the legacy SQLite replay format. Slice 7 removes that format
from the durable schema and from runtime behavior.

The source schema is replay database version 13. The target is version 14.
Migration is SQLite-only and performs no replay-file I/O. It may read legacy
chart and course header tables, but it must never read replay events, touch
samples, lane-cover events, or course-stage rows to manufacture a fact.

## Considered Approaches

### Selected: dedicated nullable summaries plus a one-way v14 cutover

Create separate chart and course summary tables whose nullable columns express
unknown or malformed legacy header facts honestly. Copy every header row,
re-parent durable legacy receipts, retire non-runnable legacy pending work, and
drop the raw playback schema in one transaction. Runtime exposes the copied
rows only through a summary read model.

This matches the umbrella design, keeps modern result validation strict, and
makes the absence of legacy playback data structural rather than conventional.

### Rejected: retain the old tables as read-only history

This would leave detail rows, foreign keys, write APIs, and reconstruction code
available indefinitely. It would also preserve two authorities for Records and
make a future accidental event-row write possible.

### Rejected: promote legacy rows into modern results

Legacy headers lack max score, judgement counts, gauge history, canonical
attempt identity, and other required modern facts. Filling those gaps from
events or defaults would reconstruct history and violate result provenance.

## Version 14 Schema

`legacy_chart_result_summaries` stores:

- the stable legacy replay ID as its primary key;
- nullable chart path, MD5, SHA-256, title, artist, and long-note mode;
- nullable final score, header max combo, final gauge, and clear type;
- nullable creation time; and
- nullable ruleset version, eligibility, and canonical provenance JSON;
- a required `partial` flag.

`legacy_course_result_summaries` stores:

- the stable legacy course replay ID as its primary key;
- nullable legacy course ID and stored course key;
- nullable course and group names and constraint JSON;
- nullable final score, header max combo, final gauge, and clear type;
- nullable completed and total chart counts and creation time;
- nullable ruleset version, eligibility, and canonical provenance JSON; and
- a required `partial` flag.

The tables have only lookup indexes for chart path/hashes and course key/legacy
ID. They have no event, file, modern-result, or course-stage relationship and
no public write API. A fresh database reaches v14 without retaining any legacy
playback table.

The current `ir_submission_receipts.replay_id` column remains the historical
legacy-owner field, but its foreign key is changed from `replays(id)` to
`legacy_chart_result_summaries(legacy_replay_id)`. The mutually exclusive
`modern_chart_result_id` owner remains unchanged. This preserves independently
durable receipt history without treating a legacy summary as replay evidence.

## Header Copy Rules

Migration enumerates header columns explicitly and binds each destination
value only when the source SQLite storage class and value range are valid. It
never uses `SELECT *` and never joins a detail table.

- A malformed or absent optional value becomes SQL `NULL`.
- A row is `partial` when a designated header fact is unavailable or invalid.
- A course row is also partial when its stored completion count is smaller
  than its total count.
- Provenance is copied only when its JSON deserializes canonically and agrees
  with independently stored ruleset and eligibility columns. Otherwise all
  provenance fields become unknown and the row is partial.
- Schema versions before header `max_combo` existed keep max combo unknown.
- Schema versions before provenance existed keep provenance unknown.
- Stored chart hashes and course keys are not repaired from event or stage
  rows. Malformed values become unknown.
- No result fingerprint, attempt identity, IR snapshot, score maximum,
  judgement history, gauge history, or replay input is invented.

One damaged row cannot block other structurally readable rows. Missing header
tables or required header columns are structural failures and abort migration.

## Atomic Migration

Version 14 migration runs inside the repository's existing immediate
transaction or nested savepoint:

1. Validate the expected legacy header-table shape and exact v13 modern schema.
2. Create and inspect the two exact legacy summary tables and indexes.
3. Copy every chart and course header using the rules above.
4. Verify copied row counts equal source header row counts.
5. Rebuild `ir_submission_receipts` with legacy-summary or modern-result
   ownership and copy every structurally stored existing receipt without
   changing IDs or reinterpreting its evidence.
6. Preserve `ir_outbox` payload rows. Rows already marked ready remain
   runnable because their payload is independently durable. Inactive legacy
   work that can no longer be activated is retained as blocked historical work
   with a bounded `legacy_result_cutover` diagnostic.
7. Drop `pending_chart_score_writes`; its owning replay header is already a
   legacy summary and it is not promoted to a modern result.
8. Drop `course_replay_stages`, `replay_events`, `replay_touch_samples`,
   `replay_lane_cover_events`, `course_replays`, and `replays`. Their indexes
   disappear with their owners.
9. Verify source-to-summary counts, exact target schema, absence of every raw
   table, receipt ownership, and `PRAGMA foreign_key_check`.
10. Set `PRAGMA user_version=14` and commit.

Any prepare, step, allocation, verification, version-write, or commit failure
rolls the whole operation back. Tests inject interruptions throughout the
SQLite virtual-machine execution and compare the complete pre-migration schema
and row content after every failed run. A separate authorizer denies reads of
all detail tables while allowing migration; success under that authorizer is
the executable proof that summary copy is header-only.

The old version-1 max-combo backfill and version-3 course-key reconstruction
are removed. Upgrades from those versions add missing header columns as
unknown/default storage only; v14 records the affected summary as partial.

## Summary Read Model

`LegacyChartResultSummary` and `LegacyCourseResultSummary` are repository read
models with optional fields matching the nullable tables. The repository
provides bounded newest-first chart and course history queries. No API loads a
`ReplayData` or `CourseReplayData` from a legacy summary.

`ResultRecordSummary` gains explicit legacy chart and legacy course identities
and carries the matching optional summary. Its factory obtains capabilities
from `replay::capabilitiesFor`, so every legacy state has Records visibility
and no View Result, Watch, Retry Same, G-Battle, practice ghost, video, share,
delete, or IR-upload action. Unknown score, lamp, combo, gauge, or time values
render as unavailable rather than as manufactured zeros.

Records merges legacy summaries, modern local results, ephemeral autoplay,
and remote results newest-first. Legacy summaries participate in filtering and
sorting only when the selected fact is known. They remain in profile database
snapshots and archives without requiring a BRD member.

## Runtime Cutover

All new chart and course persistence already goes through
`ChartReplayPersistence` and `CourseResultPersistence`; Slice 7 makes that the
only route.

- Application recovery runs only modern pending-score recovery.
- ResultScene removes the legacy chart/course save and retry fallbacks.
- MainMenu and ChartViewer remove legacy playback, result recall, G-Battle,
  ghost, video, sharing, deletion, and reconstructed-IR branches.
- Practice ghost, Watch, Retry Same, video, and G-Battle consume only verified
  modern BRDs through the existing chart/course consumer authorities.
- Legacy best-replay searches are removed. Static score pacemakers may still
  use independently durable score snapshots, but no progression is rebuilt
  from legacy events.
- Manual and reconciliation IR candidates use modern result IDs and stored IR
  snapshots. Historical legacy receipts remain display evidence only and
  cannot create a new submission.

Legacy repository mutation and hydration implementations are deleted. Any
short-lived compatibility surface needed by unrelated pure tests fails closed
and contains no raw-table SQL; no application call site uses it.

## Error and Availability Behavior

Migration failure leaves the source database unchanged and prevents profile
activation. A successfully migrated partial row remains visible in Records.
It cannot block modern history, profile duplication, or archive export.

Missing, corrupt, mismatched, or user-deleted modern BRDs continue to affect
only replay-dependent capabilities. Legacy summaries never inspect a file and
therefore have no replay state to reconcile.

## Test Strategy

The slice is driven by these contract groups:

- v13-to-v14 header-copy tests with valid, partial, malformed, and incomplete
  chart/course rows;
- a read-denying authorizer proving no detail table participates in migration;
- progress-interruption and structural-failure matrices proving exact rollback;
- fresh-schema and post-play assertions proving raw tables never exist and raw
  row counts cannot grow;
- receipt/outbox tests proving historical ownership and durable payloads
  survive without enabling legacy IR reconstruction;
- legacy summary repository, projection, Records UI, filter, profile duplicate,
  and archive tests;
- modern-only producer, consumer, recovery, manual IR, and reconciliation
  integration tests; and
- source and SQL audits proving no raw replay insert or legacy hydration path
  remains reachable.

Final verification runs all focused replay/result/IR/profile/migration suites,
the full configured CTest suite, desktop `main`, and the non-deploying iOS
compile check.
