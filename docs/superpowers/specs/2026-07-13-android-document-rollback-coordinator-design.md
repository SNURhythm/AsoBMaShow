# Android Document Rollback Coordinator Design

**Date:** 2026-07-13
**Branch:** `chore/refactor-2`

## Purpose

Prevent a cancelled or failed Android document export from skipping the attempt
to restore an originally empty provider document. Make one component own the
URI write lease, rollback scheduling, and lease completion so these states
cannot diverge.

This is the first focused remediation from the `chore/refactor-2` audit. The
older unmerged `chore/refactor` branch remains separate.

## Defect and Root Cause

Android exports accept only provider destinations proven to be empty. Opening
the destination with `rwt` can truncate it and a later cancellation, copy
failure, or rejected native commit can leave partial bytes behind.

The current activity submits an empty-document restore to a single-worker
executor with an eight-item bounded queue. The executor uses the default abort
policy. If one restore is running and eight are queued, the next submission is
rejected. The activity then completes the URI restore ticket without running a
restore. The operation reports cancellation or failure while the provider
document can retain partial exported bytes.

The root cause is split transaction ownership:

- `AsoBMaShowActivity` decides whether and when a ticket is completed.
- `DocumentHandoffRestoreRegistry` serializes a URI but does not own its
  terminal transition.
- The rollback executor independently decides whether rollback work is
  accepted.

Executor acceptance and ticket completion therefore act as competing sources
of truth.

## Scope

The remediation covers Android profile/document export rollback after an
originally empty provider destination has been touched.

It does not change:

- import behavior;
- native commit semantics;
- the refusal to overwrite non-empty or unknown-size documents;
- the maximum export size or MIME policy;
- iOS or desktop document handoff;
- provider behavior when a restore attempt itself fails.

## Architecture

Add a package-private `DocumentHandoffRollbackCoordinator` under the Android
application package. It is the only owner of `DocumentHandoffRestoreRegistry`
and the rollback executor.

The coordinator exposes a small lease-oriented interface:

- acquire a lease for a provider URI, respecting cancellation while waiting;
- finish a lease without rollback after a committed or untouched operation;
- finish a lease through an empty-document rollback action after a touched
  export fails.

`AsoBMaShowActivity` holds one static coordinator. It no longer holds a restore
registry or rollback executor and never completes registry tickets directly.
This makes the coordinator the single authoritative lifecycle for a URI write:
acquired, resolving, then completed.

Each lease may be resolved once. A committed/untouched resolution completes it
immediately. A rollback resolution marks it resolving, submits exactly one
rollback task, and completes the underlying registry ticket only in that
task's `finally` path.

## Scheduling and Backpressure

Production keeps a single daemon rollback worker and a bounded queue of eight.
The rejection handler runs overflow work on the submitting thread. This
preserves bounded memory while applying backpressure instead of discarding the
transactional cleanup.

The coordinator also guards the executor submission boundary: if an injected
or unexpected executor throws before accepting the task, the same run-once
task executes synchronously. The task itself prevents double execution, so a
submission failure cannot create a second rollback.

The executor has application-process lifetime and is not exposed for shutdown.

## Data Flow

1. The activity canonicalizes the selected provider URI and asks the
   coordinator for its lease before inspecting or opening the destination.
2. If cancellation occurs before the destination is touched, the activity
   finishes the lease without rollback.
3. If the copy and native commit both succeed, the activity finishes the lease
   without rollback and returns success.
4. If an originally empty destination was touched and the operation does not
   commit, the activity gives the coordinator a restore action that opens the
   URI with `wt`, flushes, and closes it.
5. The coordinator guarantees that action is attempted once, either on the
   worker or synchronously under queue pressure, then releases the URI lease.
6. A later write to the same URI cannot acquire a lease until the prior restore
   attempt has finished.

Different URI keys may have pending restores concurrently in the queue. The
existing native document-handoff mutex serializes foreground operations but
does not need to remain locked while an accepted background restore completes.

## Error Handling

The restore action reports failure when the content resolver returns no output
stream and throws on provider errors. The coordinator accepts an error sink so
production logs these failures and unit tests can assert them without Android
logging dependencies.

Whether restoration succeeds or fails, the URI lease is released after the
attempt. Holding it forever would deadlock every later operation for that URI.
If the provider retains partial bytes after a failed restore, the existing
non-empty-destination policy prevents a later export from silently overwriting
them.

Duplicate terminal calls are ignored by the lease's run-once state and cannot
release another operation's registry ticket.

## Components and Files

- Create
  `android/app/src/main/java/com/snurhythm/asobmashow/DocumentHandoffRollbackCoordinator.java`:
  lease lifecycle, executor submission, synchronous overflow/fallback, and
  terminal completion.
- Create
  `android/app/src/test/java/com/snurhythm/asobmashow/DocumentHandoffRollbackCoordinatorTest.java`:
  deterministic lifecycle and saturation tests.
- Modify
  `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`:
  replace direct registry/executor ownership with coordinator calls and a
  restore action.
- Keep `DocumentHandoffRestoreRegistry` as the coordinator's URI-serialization
  primitive. The activity no longer accesses it.

## Testing

Unit tests must prove:

- a committed or untouched lease is released exactly once;
- a same-URI waiter remains blocked until the rollback action finishes;
- rollback exceptions are reported and still release the lease;
- a null provider output stream is reported as a rollback failure;
- with one worker occupied and the bounded queue full, the overflow rollback
  runs synchronously instead of being rejected;
- an executor that throws at submission still causes one synchronous rollback;
- duplicate resolution calls cannot execute rollback twice or release a newer
  lease.

The saturation test uses latches and a coordinator constructed with a small
test executor; it does not depend on sleeps or timing races.

Verification includes:

- the complete `testPlayDebugUnitTest` Android Java suite under Java 17;
- the focused CTest `foundation_platform_document_handoff` target;
- the complete desktop CTest suite;
- `cmake --build cmake-build-debug --target main -j 6`;
- `git diff --check`.

No Firebase deployment is part of this remediation.

## Acceptance Criteria

- Queue saturation cannot complete a URI lease without attempting its required
  rollback.
- The activity has no direct restore-registry or rollback-executor ownership.
- Every acquired lease has exactly one terminal path.
- Same-URI writes remain serialized through completion of the prior rollback
  attempt.
- Provider restore failures are observable in logs and cannot authorize a
  silent overwrite of remaining bytes.
- Existing document handoff behavior and test suites remain green.
