# Records Result Recall Design

## Goal

Let users reopen the real result screen from the Records modal. A recalled
single-chart result can use the existing Bokutachi submission controls when
its durable replay proves that the result is eligible and has not already
been submitted. A recalled course is browsed manually from its first stage,
through every later stage, to the aggregate course result.

## Scope

- Replace the Records modal's `Export Photo` action with `View Result`.
- Support saved single-chart replays and saved course replays.
- Keep synthetic Auto Play entries disabled because they are not saved player
  results.
- Reuse the existing `ResultScene` presentation, gauge-history selector,
  timing analytics, rankings, photo export, and IR status/actions.
- Preserve the existing `Watch`, `G-BATTLE`, and `Export Video` actions.
- Do not add course-score submission. Bokutachi submission remains limited to
  eligible single-chart LR2 results.

## User Experience

### Single-chart records

Selecting a saved record and pressing `View Result` changes the button label
to `Loading...` while the replay and chart are loaded. Success opens
`ResultScene` in saved-replay-result mode. The screen shows the recorded score,
judgements, gauge history, timing analytics, rankings, replay action, and photo
export.

If Bokutachi is enabled and the stored replay can be reconstructed as the same
durable result attempt, the existing IR block shows one of its normal states:

- `Not submitted` with `Submit` when no outbox row exists.
- `Queued`, `Submitting`, `Polling`, or `Waiting` while delivery is active.
- `Submitted` after acceptance.
- `Authentication required` or `Submission failed` with `Retry` when the
  existing row is retryable.
- `Not eligible` when the durable attempt exists but the provider rejects its
  ruleset or provenance.

Legacy records without a durable attempt identity, records whose chart no
longer matches, and records whose reconstruction fails fingerprint validation
still open as result screens but do not expose a new submission action.

### Course records

`View Result` loads and validates every saved course stage before leaving the
Records modal. It opens the reconstructed result for stage 1. Pressing `Next`
opens stage 2 directly, continuing through the last completed stage. Pressing
`Next` on the last stage opens the aggregate course result.

Course result browsing never starts replay gameplay and never auto-advances.
Back exits to the main menu without the live-course progress-loss warning.
Each stage result and the aggregate result retain their existing `Export
Photo` action. Course stages and the aggregate do not show an IR submission
block.

If any required stage replay or chart cannot be loaded, the application stays
in Records and reports that the saved result is unavailable. It must not open
a partially prepared course browser.

## Durable Record Source

The replay repository will expose a result-recall record for a single chart.
It contains:

- The existing `ReplayData` and database replay ID.
- The stored `attempt_id`, when present.
- The stored `attempt_fingerprint`, when present.
- The replay row's `created_at` converted by SQLite to positive Unix
  milliseconds.

`LoadReplay` remains available for playback callers. The recall read uses the
same chart-identity matching and snapshot transaction as replay loading, but
also reads the attempt metadata atomically with the replay and its events.
Attempt IDs and fingerprints are device-local integrity data; no API key or
credential is introduced into replay or outbox rows.

Legacy rows with null attempt columns are valid for viewing but cannot be
turned into a new IR submission.

## Single-chart Reconstruction

A focused result-recall builder will:

1. Parse the stored chart using the replay's recorded parser random data,
   play options, and long-note mode.
2. Rebuild `RhythmState` with `replay_result::BuildResultState`, which uses the
   ruleset recorded in provenance and the replay's gauge snapshots.
3. Verify that the rebuilt score, gauge, combo, clear mark, chart identity,
   and provenance satisfy `makeChartResultAttempt`.
4. Require a canonical stored attempt ID and a nonempty stored fingerprint.
5. Compare the newly calculated attempt fingerprint with the stored
   fingerprint using exact canonical text equality.
6. Build `IrSubmission` with the replay row's original positive Unix
   timestamp only after the fingerprint check succeeds.

Result viewing succeeds independently of steps 3-6. Any failure in historical
IR reconstruction removes only the IR submission context; it does not prevent
the saved replay result from opening.

The recalled `ResultScene` receives a synthetic `SaveOutcome` in `Saved` state
only when a validated historical attempt is available. Its receipt carries
the stored attempt ID, replay ID, and database timestamp. This lets the
existing result presentation exclude the current attempt from previous-best
queries and lets the existing IR presentation query the outbox by the original
attempt ID. It does not stage, rewrite, or acknowledge the result again.

## Course Reconstruction and Navigation

A course recall builder will operate on `CourseReplayData` and prepare all
completed stages as one atomic in-memory browse session:

- Parse every stage with its stored replay configuration.
- Rebuild each stage's `RhythmState` using the course gauge profile and the
  carried gauge snapshot from the previous stage.
- Populate `CoursePlaySession::entries`, `completedResults`, stage provenance,
  replay stages, aggregate combo, final carried gauge, and the original course
  metadata.
- Mark the session as replay-backed so result initialization never saves a
  score or replay.

`ResultCourseOptions` gains an explicit saved-result-browsing flag. In this
mode:

- Stage `Next` creates the next reconstructed `ResultScene` directly.
- The final stage creates the aggregate `ResultScene` directly.
- The replay-playback timer is disabled.
- Back exits without a live-course confirmation.
- The aggregate result may still start normal course replay through its
  existing `Replay` action.

The browsing flag is separate from `courseReplayPlayback`: replay playback
continues to auto-advance after its recorded inter-stage delay, while saved
result browsing is manual and does not launch gameplay.

## Records Modal Integration

The existing footer slot and member names associated with record-photo export
will become result-view names. The visible label is `View Result`. It is
enabled only for a selected persisted replay or course replay and disabled for
Auto Play, filter/sort mode, watch options, export options, progress mode, and
other active record operations.

The click handler dispatches to single-chart or course reconstruction. It uses
the modal's existing deferred loading pattern so a scene transition never
occurs inside the pointer callback. A single in-progress guard prevents double
launches. On failure, the button shows a bounded failure label before returning
to `View Result`; diagnostics are sanitized and may be logged without replay
payloads or credentials.

The old Records-modal batch photo-export entry point is removed from the
footer. Photo export remains available from every recalled result screen.
Video export is unchanged.

## Error Handling and Safety

- Missing replay, missing chart, cancelled parsing, invalid provenance, and
  corrupt events fail closed.
- A fingerprint mismatch never creates or mutates an IR outbox row.
- Opening a result is read-only with respect to replay and score databases.
- Only an explicit press of the existing result-screen `Submit` or `Retry`
  action mutates the outbox.
- Existing outbox rows remain authoritative; recall never replaces their
  payload or submission state.
- Provider settings and the current per-profile credential are resolved only
  when the existing submission service sends work. Credentials are never
  copied into recalled models or outbox rows.
- Course reconstruction is all-or-nothing before the first stage is shown.

## Testing

Focused tests will cover:

- Repository recall reads return attempt metadata and an original Unix
  timestamp atomically, while legacy null attempt metadata remains viewable.
- A matching reconstructed attempt enables historical IR construction.
- Missing attempt metadata, malformed timestamps, changed chart identity, and
  fingerprint mismatch suppress IR without suppressing result viewing.
- Existing outbox states drive the same result presentation after recall as
  they do immediately after play.
- The Records footer contains `View Result`, no longer contains its photo
  action, disables Auto Play, and preserves other action availability.
- Single-chart recall enters `ResultScene` with replay-result behavior and the
  original attempt ID.
- Course recall prepares every stage, advances stage-by-stage only when `Next`
  is clicked, reaches the aggregate result, does not launch gameplay, and does
  not show the live-course exit warning.
- Any course stage preparation failure leaves Records active and creates no
  partial browse session.

Verification includes the focused repository, result, replay, IR, view, and
course tests; the configured CTest suite; `git diff --check`; and the desktop
`main` build.

## Out of Scope

- Uploading course scores.
- Uploading Auto Play, practice, assisted, modified, or legacy-unverified
  results.
- Adding Previous navigation or random stage jumps to course result browsing.
- Returning to the exact Records selection after leaving a recalled result.
- Changing Bokutachi payload format, credentials, polling, or retry policy.
