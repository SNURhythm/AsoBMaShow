# PR #82 Third Review Fixes Design

**Date:** 2026-07-26
**Branch:** `feature/file-based-replays`
**Reviewed base:** `6ac945f`

## Context

The latest PR review identified four follow-up issues in the file-based replay
cutover:

1. Profile export treats a deliberately deleted replay file as archive
   corruption because its compact database reference remains.
2. The Beatoraja slot-copy service has no production caller.
3. Legacy replay migration infers key mode only from lanes observed in replay
   events, so all-miss and sparse plays can be migrated with the wrong mode.
4. A failed course replay save is logged and discarded instead of entering the
   retry-or-continue flow already used by chart results.

The slot-copy finding is intentionally deferred. Safely exposing the operation
requires relocating an existing destination-slot replay and updating its
database reference atomically. Replacing only the visible file would mutate
the bytes referenced by an older result, breaking replay integrity and profile
export. This change therefore adds no slot-copy UI, does not overwrite an
occupied Beatoraja slot, and leaves that review thread open.

## Goals

- Let users delete replay files without making their profile unexportable.
- Preserve integrity validation for every replay file that is present during
  export.
- Migrate sparse and all-miss legacy replays with the chart's actual key mode
  whenever chart metadata is available.
- Keep replay migration non-blocking when chart metadata is unavailable or
  stale.
- Map legacy replay input correctly for every key mode supported by the
  Beatoraja codec.
- Give failed course replay persistence the same retryable user decision as a
  failed chart result save.

## Non-goals

- Do not expose Beatoraja slot-copy actions in production UI.
- Do not relocate or rewrite occupied replay slots.
- Do not remove stale replay-file references merely because their files were
  deleted; the result record remains valid and the missing attachment remains
  observable.
- Do not reconstruct result or IR provenance from replay events.
- Do not make chart metadata availability a prerequisite for opening or
  migrating a profile.
- Do not change the `.brd` envelope or filename grammar.

## Considered Approaches

### 1. Fix each affected persistence boundary independently — selected

Profile export changes only how missing optional replay attachments are
handled, migration receives a read-only chart-key-mode resolver, and result
presentation gains a course-attempt retry path. This preserves the existing
domain separation and keeps each regression test close to the boundary that
failed.

### 2. Eagerly delete replay references when files disappear

This would make export pass, but a user can delete files outside the app and
the database cannot observe that atomically. Startup reconciliation would add
unnecessary writes and would erase useful information that a replay once
existed. Missing attachments should instead be valid state.

### 3. Expose slot copy with destructive replacement

This would satisfy the UI review literally, but it would leave the result that
previously owned the destination path pointing at unrelated bytes. The
operation is deferred until reference relocation can be designed and tested
as an atomic transaction.

## Design

### Profile archive export

Replay files are optional attachments. The exporter continues to enumerate
the files currently present beneath the profile's `replay/` directory and
includes every valid `.brd` member in the archive.

For a present file with a `replay_files` reference, the exporter must still
verify its normalized relative path, compressed byte count, and SHA-256 before
adding it. A mismatch remains an integrity failure. Unreferenced present replay
files retain the existing export behavior.

After enumeration, references with no corresponding file no longer make the
export fail. They represent replay attachments the user deleted intentionally
or that were already unavailable. The database result, provenance, and IR
state are still exported, while the absent `.brd` member is simply omitted.
Import behavior does not synthesize the missing file and does not need to
alter the retained reference.

### Metadata-aware legacy key-mode migration

The replay-file migration receives an optional, read-only key-mode resolver.
Its input is the legacy chart identity already being migrated: stored chart
path plus available MD5 and SHA-256 values. Its output is an optional supported
key mode.

Production wiring resolves this from the initialized chart metadata database.
An exact content-hash match is authoritative. A path match is accepted only
when every non-empty legacy hash also agrees, preventing a replaced chart at
the same path from supplying unrelated metadata. Unsupported or ambiguous
metadata is treated as no answer.

When the resolver returns a supported mode, migration uses it for both the
persisted replay setup and conversion of legacy physical lane events to
Beatoraja logical controls. The mapping covers the codec's supported 5, 7, 9,
10, 14, 24, and 48-key modes, including the correct player-two offsets and
scratch controls where the mode has scratches.

If the chart database cannot be opened, has no matching row, contains an
unsupported mode, or the resolver otherwise returns no answer, migration
falls back to the existing observed-lane inference. This fallback can be less
precise, but it preserves the current ability to migrate a profile whose chart
library is incomplete. Metadata lookup failure does not abort the schema
migration.

The existing staging-directory and database transaction sequence remains
unchanged: converted files become visible together with the committed schema,
or migration rolls back without a partially upgraded profile.

### Retryable course replay persistence

`ResultPersistenceOptions` retains the completed in-memory course attempt when
`persistCourse` fails, alongside the returned `SaveOutcome`. At most one live
chart attempt or course attempt may be attached to a result source.

The existing persistence status UI becomes attempt-kind agnostic:

- **Retry Save** invokes chart persistence for a chart attempt or course
  persistence for a course attempt.
- **Details** displays the same outcome diagnostics and uses the applicable
  attempt ID.
- **Continue** dismisses the blocking decision without pretending that the
  replay was saved.

On a successful course retry, the session receives the saved course replay ID,
the saved flag, and the course playback data exactly as it does after a
successful first attempt. The retry state is then cleared by the successful
outcome. Automatic IR drafts remain chart-only; course retry never enters the
IR pipeline.

If persistence is non-retryable, the status remains visible but Retry Save is
disabled according to the existing `SaveOutcome` policy. Choosing Continue
allows navigation while preserving the failed status for the lifetime of the
result scene.

## Error Handling and Invariants

- Missing replay attachment: export the remaining valid profile and omit the
  missing archive member.
- Present replay with wrong size or digest: fail export as an integrity error.
- Chart metadata database unavailable or unmatched: fall back to legacy key
  inference without failing migration.
- Metadata returns an unsupported mode: ignore it and use the fallback.
- Legacy input cannot be mapped for the selected supported mode: fail that
  conversion so the atomic migration rolls back.
- Course save failure: retain the attempt and outcome until Retry Save succeeds
  or the user chooses Continue.
- Slot-copy production access remains absent in this change.

## Testing

### Profile archive

- Create a result with a referenced replay, delete only its `.brd` file, and
  verify profile export succeeds and omits that member.
- Keep the existing regression that a present referenced replay with modified
  bytes fails integrity validation.

### Replay migration

- Migrate an all-miss or player-one-only 14-key legacy replay while the resolver
  returns 14, then verify the persisted setup and decoded `.brd` retain mode 14.
- Cover at least one non-7/14 mapping whose physical lane conversion differs,
  proving the expanded mapping table is used.
- Verify a missing resolver answer still uses the existing inference and
  completes migration.
- Preserve existing crash/rollback migration tests.

### Course persistence

- Produce a retryable `persistCourse` failure and verify the result source keeps
  the completed course attempt and exposes the decision state.
- Retry and verify course persistence is called with the same attempt and the
  session receives the saved ID and playback data on success.
- Choose Continue and verify the scene can proceed without marking the replay
  saved.
- Preserve the chart persistence retry tests to ensure the generalized flow
  does not regress chart results.

## Review-Thread Disposition

- Address: profile export after user replay deletion.
- Address: metadata-aware, multi-mode legacy replay migration.
- Address: retryable course replay persistence.
- Defer and leave open: production UI for Beatoraja slot copy.
