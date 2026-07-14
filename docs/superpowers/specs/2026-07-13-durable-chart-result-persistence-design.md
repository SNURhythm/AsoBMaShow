# Durable Chart Result Persistence Design

## Context

Normal chart completion currently has two independent persistence owners in
`ResultScene`: `saveScore()` inserts a score and `saveReplay()` inserts a
replay. Both normal-chart paths set their local `saved` boolean before the
database call. A failed call therefore becomes permanently non-retryable for
that scene and is visible only in the log.

The ownership boundary is also too late. Gameplay finalizes the result, then
may wait two seconds before the deferred result-scene transition. No durable
write exists during that interval, and deferred work is not processed while
the application is backgrounded. A quit, suspension, or process kill can lose
an otherwise completed result.

Finally, score and replay rows duplicate final result facts in separate
databases without a shared attempt identity. Either write can commit alone,
and repeating a write creates another row. The caller cannot safely recover
from a crash or an ambiguous acknowledgement.

This remediation makes the recorded replay plus a durable score outbox the
authoritative chart attempt. The score database remains a derived, indexed
projection for existing queries.

## Scope

This design covers live, non-practice, non-course, non-replay chart attempts
for which `resultCapturePolicy()` already requires both score and replay
persistence. It includes normal clears and gauge failures.

It does not change the intentional no-persistence policy for practice,
Auto Play, replay playback, or course playback. An in-progress multi-stage
course is a different recovery product boundary because recovery must define
whether to resume, finalize a partial course, or discard it. Course session
checkpointing will be handled as a separate remediation instead of silently
choosing that behavior inside the chart migration.

## Goals

- Stage a completed chart attempt durably before any delayed result
  transition.
- Give the replay and score projection one stable attempt identity and one
  captured result timestamp.
- Make replay staging, score projection, acknowledgement, and retry
  idempotent.
- Recover a committed replay whose score projection was interrupted.
- Keep one immutable score-write payload as the source for both the outbox and
  score row.
- Attempt all required persistence phases, retain pending work on failure, and
  expose a sanitized user-visible retry state instead of log-only loss.
- Preserve existing score/replay query behavior and legacy database rows.

## Non-goals

- Do not merge the score and replay database files.
- Do not rewrite historical rows or synthesize attempt identities for legacy
  records.
- Do not make background SQLite writes asynchronous.
- Do not add cloud sync, cross-device deduplication, or an unbounded retry
  loop.
- Do not reconstruct a score from a chart parse during recovery; the complete
  immutable score payload is captured at completion.

## Considered Approaches

### 1. Durable replay-backed outbox with attempt identity — selected

The replay database transaction stores the complete replay and a pending score
projection together. A unique attempt ID links that replay to an idempotent
score insert. The pending projection is deleted only after the score database
confirms the same attempt ID. This closes the delayed-transition window and
provides deterministic partial-commit recovery without trying to create a
cross-database SQLite transaction.

### 2. Result-screen retry only

Keeping the payload in memory and adding a Retry button would fix the current
one-shot booleans during one result scene. It would not protect the two-second
pre-scene interval, process death, or an ambiguous commit, and a retry could
duplicate a successful row.

### 3. Move the two existing writes into gameplay completion

This reduces the delayed-transition window but retains two independent,
non-idempotent commits and no durable record of which half remains. It also
moves persistence timing without establishing a common source of truth.

## Immutable Attempt Model

Add a platform-neutral `result_persistence` model with these concepts:

- `AttemptId`: a lower-case version-4 UUID generated once for a live attempt.
  Validation accepts only the canonical 36-character form with the correct
  version and variant bits. Tests inject fixed IDs.
- `ChartScoreWrite`: the immutable values currently bound by
  `ScoreDBHelper::InsertScoreOnConnection`: chart identity and display
  metadata, long-note mode, score/max score, combo values, six judgement
  counts, fast/slow counts, final gauge, clear rank, and provenance.
- `ChartResultAttempt`: an attempt ID, the completed `ReplayData`, and its
  `ChartScoreWrite`.
- `StageReceipt`: attempt ID, replay row ID, the database-assigned result
  timestamp, and whether a score projection remains pending.

The model also produces a versioned SHA-256 `payloadFingerprint` from a
canonical, length-prefixed encoding of every replay and score-write field. It
excludes only database-generated row IDs and timestamps. Integers use fixed
widths, floating-point values use their bit representation, optionals carry an
explicit presence byte, and vectors preserve order and length. This avoids
locale/formatting ambiguity and lets a later retry prove that an existing
attempt ID still names the same immutable payload.

One factory captures `ChartScoreWrite` from `ChartMeta`, `RhythmState`,
`ScoreProvenance`, and the storage long-note mode produced by the existing
`scoreLongNoteModeForClearLamp(meta)` policy. Passing that value explicitly
keeps the platform-neutral model independent from `ScoreDBHelper` while
retaining one long-note-mode policy owner. Runtime score persistence and outbox
staging both consume the same captured value. Existing standalone
`SaveScore(meta, state, provenance)` is a compatibility wrapper that supplies
the policy value, creates a record without an attempt ID, and delegates to the
common binder.

The capture operation is public as `captureChartScoreWrite(...)` so both the
attempt factory and the compatibility Score DB API use the exact same field
derivation; neither reimplements judgement, path, hash, or clear-rank binding.

The repository already has private UUID generation and validation inside
`PlayerProfileManager.cpp`. Extract the byte generation/encoding and structural
validation into one small `Uuid` utility and make the profile manager consume
it. Profile IDs retain their current case-insensitive structural acceptance for
backward compatibility; new result attempt IDs use the stricter lower-case
version-4 validator. This avoids adding a second UUID truth.

The attempt constructor rejects mismatched chart identity, provenance, final
score, final gauge, or max combo between the replay and score record. Clear
rank uses the repository's existing normalized comparison: the score payload
keeps the base gauge clear rank stored in `scores`, while the replay keeps
`fullComboRankForPlayback(baseRank, fullCombo, playback)`. The constructor
derives that replay rank from score combo fields, total notes, and provenance
and requires it to equal `ReplayData::clearType`. This preserves the existing
score-summary trigger semantics while still detecting drift. A generated ID is
never reassigned to another payload.

## Database Schema and Migration

### Replay database version 5

Add nullable `attempt_id` and `attempt_fingerprint` to `replays`, plus a unique
partial index over non-null attempt IDs. New attempt rows require a canonical
64-character lower-case fingerprint. Legacy rows keep both values null and
retain their current semantics.

Add `pending_chart_score_writes` with:

- `attempt_id TEXT PRIMARY KEY`;
- `replay_id INTEGER NOT NULL UNIQUE` referencing `replays(id)`;
- every field of `ChartScoreWrite` in database-ready form;
- normalized provenance columns and canonical provenance JSON;
- `created_at TEXT NOT NULL`, copied from the staged replay row.
- `recovery_attempts INTEGER NOT NULL DEFAULT 0` and nullable
  `last_recovery_at TEXT`, used only for durable fair retry ordering.

The schema has no independent mutable `saved` boolean. Row presence means the
score projection is pending; row deletion means it was acknowledged. The
replay row remains the durable authoritative attempt after acknowledgement.

### Score database version 9

Add nullable `attempt_id` to `scores` and a unique partial index over non-null
values. Legacy inserts continue to use null. The new projected-score API binds
the supplied result timestamp rather than generating a second timestamp.

An insert for a non-null attempt ID is idempotent:

1. attempt the insert;
2. on the unique-attempt conflict, read the existing row;
3. return success only if its immutable score payload, shared result timestamp,
   and provenance equal the requested projection;
4. report an integrity conflict if the same ID names different data.

This distinction prevents a broad `INSERT OR IGNORE` from hiding unrelated
constraints.

Both migrations use the existing guarded `user_version` pipelines, preserve
future-version fail-closed behavior, and create indexes inside the migration
transaction.

## Replay Staging Transaction

`ReplayDBHelper::StageChartResult(attempt)` is the only runtime API that creates
a persistent chart replay for a live result.

It returns a typed `StageStatus`: `Staged`, `AlreadyStaged`,
`StorageFailure`, or `IntegrityConflict`. Storage failure means no new
transactional state was committed. Integrity conflict means the supplied ID
already names a different fingerprint; the existing row is left untouched and
the current in-memory result is not claimed as durable.

For a new attempt it performs one Replay DB transaction:

1. validate the attempt and serialize provenance;
2. insert the replay and all child event/touch/lane-cover rows with its attempt
   ID;
3. read the replay row's `created_at`;
4. insert the pending score payload using the same attempt ID and timestamp;
5. commit and return the receipt.

If the same attempt is staged again, the helper compares the stored replay
fingerprint with the supplied immutable payload. It returns the existing
receipt only when they match, whether the pending row still exists or has
already been acknowledged; otherwise it reports an integrity conflict. A
transaction failure leaves neither replay nor outbox row.

The existing `SaveReplay()` API remains for legacy/import/test callers and
creates a null-attempt row. Normal gameplay must no longer call it directly.

## Projection and Acknowledgement

`ResultPersistenceCoordinator` owns the cross-database state machine through
injected adapters so its behavior is unit-testable:

1. stage the attempt in Replay DB;
2. load the pending `ChartScoreWrite` by attempt ID;
3. call the idempotent Score DB projected-write API;
4. after confirmed score success, delete that exact pending outbox row;
5. return `Saved`, `PendingScore`, `PendingAcknowledgement`, `Unstaged`,
   `UnstagedConflict`, or `PendingConflict` with a sanitized message and the
   latest receipt when available.

Each adapter has a typed result rather than a boolean:

- staging uses `Staged`, `AlreadyStaged`, `StorageFailure`, or
  `IntegrityConflict`;
- projection uses `Inserted`, `AlreadyPresent`, `StorageFailure`, or
  `IntegrityConflict`;
- acknowledgement uses `Acknowledged`, `AlreadyAcknowledged`,
  `StorageFailure`, or `IntegrityConflict` (the last case means the requested
  attempt/replay pair does not match the retained outbox row).

Outbox reads are typed as well. A single-attempt lookup returns `Found`,
`NotFound`, `StorageFailure`, or `IntegrityConflict` plus a record only for
`Found`. A recovery-batch query distinguishes a query-level `StorageFailure`
from a successful snapshot and returns each decoded row as either `Found` or
`IntegrityConflict`. A malformed row therefore remains identifiable and
retained without preventing later valid rows in the same query from being
projected. No empty `optional` is allowed to mean both "not found" and "could
not read."

The coordinator maps a staging integrity conflict to `UnstagedConflict`: it has
no receipt for the current payload and explicitly reports that the current
result is not durable. A projection or acknowledgement integrity conflict maps
to `PendingConflict`: it retains the replay/outbox receipt and reports the
current payload as durable but unverified. No conflict path deletes,
overwrites, or marks pending work complete.

One outer `profile_database_activity::WriteGuard` is held for the entire
stage -> load -> project -> acknowledge sequence. The helper-level guards nest
under it, so a profile switch cannot rebind either database between phases.
`recoverAll()` holds the same outer guard for its bounded batch. When recovery
runs inside `ProfileSessionCoordinator`'s existing `SwitchGuard`, the nested
operation guard reuses that already-exclusive binding lease instead of trying
to reacquire the gate. No guard is held while waiting for user input.

Every retry resumes from persisted state. It never re-inserts a confirmed
score or replay. If the process dies after the score commit but before outbox
deletion, recovery observes the existing identical score as success and then
deletes the pending row.

`recoverAll()` processes at most 256 pending rows per invocation. Selection
orders rows by the lowest `recovery_attempts`, then `last_recovery_at`, creation
time, and attempt ID; never-attempted rows therefore come first. Every
unsuccessful or conflicting recovery updates that row's retry count and
timestamp without
changing its payload. This durable fair rotation prevents a fixed set of bad
oldest rows from starving newer valid work. Each row is independently attempted
so one bad row does not prevent later valid rows. The coordinator returns
aggregate attempted/saved/pending/conflict counts and never deletes a failed or
conflicting row. The pending count also includes rows omitted by the bounded
batch, so startup and profile activation still warn when more recovery work
remains. Later startup/profile-activation/result-save invocations continue the
backlog.

## Gameplay and Result Flow

At normal completion, after `finishReplayRecording()` has finalized the
replay and before `scheduleResultTransition()` installs deferred work:

1. create the immutable attempt and ID;
2. run the persistence coordinator;
3. retain the attempt and outcome in memory for the result scene;
4. use zero transition delay if persistence is not fully saved; otherwise
   preserve the existing result delay;
5. pass the authoritative receipt and outcome to `ResultScene`.

When a receipt exists, its database row ID and captured timestamp are copied
into the separate mutable presentation/retry replay. Those fields are excluded
from the immutable payload fingerprint. A successful Retry Save performs the
same copy, so replay playback launched directly from the current result retains
the durable timestamp used by legacy previous-best boundaries.

`ResultScene` no longer owns independent `scoreSaved`/`replaySaved` booleans or
calls the two database helpers directly. For a live persisted result it loads
the previous best with an exact `excludeAttemptId` query filter, so moving the
current score earlier cannot make it compare against itself and timestamp
precision cannot hide an unrelated attempt from the same second. The existing
`beforeCreatedAt` filter remains only for legacy replay playback that has no
attempt ID.

If the initial outcome is not `Saved`, the result screen presents one prominent
status with a Retry action. Retrying calls the same coordinator with the same
attempt ID. A user may explicitly continue without a fully saved result; that
choice is recorded only in memory and does not delete durable pending work.
When replay staging itself failed, the message states that continuing will
discard this result. When only projection or acknowledgement is pending, the
message states that the durable replay is retained and recovery will retry.

The native message-box adapter may additionally draw attention to the failure,
but the in-scene status is authoritative and remains available if a native
dialog cannot be shown.

## Startup and Profile Recovery

After both Replay and Score schemas are ready, startup enters a small pure
`application_result_recovery` orchestration function. It calls `recoverAll()`,
reports one sanitized warning when work remains, and only then invokes the
scene-registration/runtime callback. Pending rows are durable, so recovery
failure is non-fatal to application readiness; it retains the rows for later
retry. The orchestration order is unit-tested independently from SDL/bgfx, and
`main.cpp` only supplies recovery, warning, and ready-runtime adapters. Detailed
attempt IDs, paths, and SQLite text remain log-only.

The same recovery callback runs after a successful profile database rebind and
before refreshed score/replay caches are published. A failed recovery does not
roll back an otherwise valid profile switch. It returns the existing successful
`ProfileSwitchResult` with the sanitized warning in its `message`; the profile
settings controller already renders a non-empty success message as a warning.
This ensures the active profile, not only the profile selected at process
startup, receives recovery without adding a second warning channel.

The recovery write is an allowed, idempotent side effect of activating the
target profile. If a later profile-switch step rolls back, the target may have
fewer pending projections but no user settings or active-profile metadata were
committed to it. A subsequent activation observes the same complete result.

## Error and Privacy Policy

The coordinator owns deterministic user messages:

- `Unstaged`: "This result could not be stored. Retry before leaving to avoid
  losing it."
- `PendingScore`: "The replay is safe, but the score is still pending. Retry
  now or it will be retried automatically later."
- `PendingAcknowledgement`: "The result was stored, but save confirmation is
  pending. Retrying is safe."
- `UnstagedConflict`: "This result conflicts with an existing save and was not
  stored. Retry before leaving; continuing will discard this result."
- `PendingConflict`: "This saved replay could not be verified against its
  score. It was kept for recovery and was not overwritten."

Startup and profile activation use one aggregate warning:

> Some previously completed results are still waiting to be saved. They were
> kept safely and will be retried later.

Messages never contain an attempt ID, profile identifier, chart path, database
filename, SQL text, or raw exception. Logs may contain the attempt ID and
technical diagnostics but must not log replay input events or private profile
paths.

## Test Strategy

### Pure coordinator tests

- all phases succeed in stage -> project -> acknowledge order under one
  profile-binding guard;
- stage failure does not call project or acknowledge and returns `Unstaged`;
- a stage conflict is distinct from storage failure, returns
  `UnstagedConflict`, has no current-payload receipt, truthfully warns that the
  current result can be discarded, and preserves the existing row;
- projection failure retains pending work and returns `PendingScore`;
- projection conflict returns `PendingConflict` and retains the outbox;
- acknowledgement failure returns `PendingAcknowledgement`;
- acknowledgement conflict returns `PendingConflict` and retains the
  outbox;
- retry after each failure resumes without repeating a confirmed phase;
- recovery attempts every snapshot row once even when an earlier row fails;
- 256 persistent conflicts followed by one valid row recover the valid row on
  the next invocation because the first batch has durable retry markers;
- a concurrent profile-switch attempt remains blocked until a full persistence
  call or recovery batch releases its outer guard;
- user messages are exact and contain no injected raw diagnostic.

### Replay database tests

- version-4 migration adds the nullable ID, unique index, and pending table;
- legacy null-ID replay inserts remain repeatable;
- staging creates replay children and pending score payload atomically;
- staging the same ID and identical payload returns the same replay ID;
- staging the same ID with changed data fails as an integrity conflict;
- staging the same completed/acknowledged ID validates via its retained
  fingerprint and does not recreate the pending row;
- forced failure before commit leaves neither replay nor pending row;
- future schema versions remain rejected.

### Score database tests

- version-8 migration adds nullable attempt ID and unique index;
- two identical projected writes for one ID produce one row and succeed;
- the same ID with different score/provenance fails without mutation;
- legacy null-ID score inserts remain repeatable;
- projected score and replay use the same captured timestamp;
- exact attempt exclusion returns the true previous best even when both rows
  share a one-second timestamp;
- score summary triggers/caches update once, not once per retry.

### Integration and mutation checks

- source audit proves result staging occurs before every normal chart result
  transition and `ResultScene` contains no direct `SaveScore`/`SaveReplay`;
- mutate staging below the deferred transition and prove the audit fails;
- commit the score but skip acknowledgement, reconstruct the coordinator, and
  prove recovery leaves one replay, one score, and no pending row;
- simulate process exit after replay/outbox commit and before score projection,
  then prove startup recovery produces exactly one score;
- inject a persistent stage failure and prove the result scene exposes Retry
  and explicit continue-without-saving state;
- focused tests, full CTest, desktop `main`, iOS build-only, and Android
  build-only pass without uploads.

## Acceptance Criteria

- A persistable chart result is staged before any delayed scene transition.
- A healthy completion produces exactly one replay and one score linked by one
  attempt ID and timestamp.
- Retrying any known or ambiguous failure cannot create a duplicate row.
- A crash after replay staging but before/during score projection is recovered
  on the next startup or profile activation.
- Failed or conflicting pending work is retained and is never silently marked
  saved or overwritten.
- The result UI always exposes an unsaved/pending state and Retry action when
  the coordinator is not fully saved.
- Previous-best presentation excludes the newly projected current attempt.
- Normal runtime has one persistence coordinator; `ResultScene` does not make
  independent score/replay save decisions.
- Legacy rows and compatibility APIs keep their existing query behavior.
