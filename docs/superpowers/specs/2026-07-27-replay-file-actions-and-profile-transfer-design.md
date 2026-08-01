# Replay File Actions and Profile Transfer Design

## Scope

This focused design implements delivery Slice 6 of the approved contract-first
file replay restart. It activates replay availability, share, deletion,
profile duplication, profile archives, and startup reconciliation for modern
chart and course replay files. It does not perform the legacy schema cutover;
that remains Slice 7.

## File State Authority

`modern_replay_files` remains the ownership authority and gains a durable
`user_deleted` flag. A reference is therefore in exactly one durable ownership
state:

- active: the result owns the expected contained path, hash, size, and codec;
- user-deleted: the result retains its historical reference, but replay bytes
  must not be consumed, copied, exported, or recreated for that attempt.

Filesystem inspection projects an active reference into verified, missing,
corrupt, mismatched/unsafe, or I/O-failure availability. User-deleted is
projected before touching the filesystem. A missing active file is normal and
does not mutate the record. Corrupt and mismatched present entries remain
deletable because the database still proves the exact contained path they may
occupy.

Deletion is ordered deliberately: atomically mark the exact owner/reference as
user-deleted, then remove the referenced contained entry. A failed database
transition never deletes bytes. A failed physical removal leaves the durable
tombstone in place; startup reconciliation retries only that proven path.
Result rows, snapshots, outbox work, receipts, and result fingerprints are not
changed.

## Shared Action Boundary

One `ReplayFileActionService` owns chart/course resolution and file actions.
It accepts a modern attempt identity, loads the exact result/reference, and
uses the same reference agreement and contained-file store used by playback.

- Share succeeds only from metadata-verified bytes. The handoff receives a
  stable verified snapshot and a canonical `.brd` filename.
- Delete is offered for verified, corrupt, or mismatched present files and
  performs the tombstone-first transition above.
- A missing or already user-deleted reference is idempotently unavailable.
- Legacy summaries, remote records, and results without a reference cannot
  enter this service.

Records UI visibility is derived only from `ReplayCapabilities`. It must not
infer availability from paths or legacy identifiers.

## Reconciliation

`ReplayFileReconciler` is run after the active replay database schema is ready
and before normal application work begins. It:

1. removes stale private temporary files using the contained store;
2. enumerates durable user-deleted references;
3. retries removal of their exact contained entries; and
4. reports failures without invalidating startup or result history.

It never removes active missing/corrupt files, unreferenced canonical BRDs,
user-imported stock files, or any entry whose database ownership cannot be
proven. Reservation/install recovery remains with the existing reservation and
association state machine.

## Profile Duplication and Archives

`PlayerProfilePaths` names the profile `replay/` directory. One
`ReplayProfileTransfer` boundary reads the snapshotted replay database,
enumerates active references, and copies only metadata-verified referenced
BRDs. Records are always preserved through the database snapshot even when a
file is user-deleted, missing, corrupt, or mismatched.

For each referenced file:

- verified active: copy and re-verify in the staging directory;
- user-deleted or missing: omit bytes and retain the record/reference state;
- corrupt, mismatched, unsafe, or I/O failure: fail the transfer atomically;
- unreferenced: do not copy and do not delete the source.

This policy avoids presenting unverified bytes as owned replay data. Imported
stock BRDs remain file-only and are not promoted into local result evidence;
unreferenced stock files are preserved in the source profile but are outside
record-owned duplication/archive transfer until an explicit import catalog
owns them.

Profile archive format version 3 may contain `replay/<canonical>.brd` members.
Versions 1 and 2 remain import-compatible and cannot contain replay members.
Export uses a sanitized replay-database snapshot plus verified file snapshots.
Import rejects extra replay members, duplicate paths, unsafe names, and bytes
that do not agree with an active database reference. Missing/user-deleted
members are allowed. The existing staging rename/rollback boundary makes the
database and copied replay set atomic.

## Failure and Safety Rules

- Every path is relative, canonical, and contained beneath the selected
  profile root; symlinks and non-regular entries fail closed.
- Duplicate/archive staging is removed on any injected copy, verification,
  sync, database, or finalize failure; the source profile and any overwrite
  target remain unchanged.
- Startup reconciliation is conservative and non-fatal.
- No operation reads or writes `~/Downloads/profiles`; migration fixtures use
  temporary copies only.
- No replay action reconstructs legacy data or result facts from BRD bytes.

## Contract Tests

The slice adds tests for durable user deletion, exact-owner conflicts,
verified sharing, corrupt/mismatched deletion, cleanup retry, and preservation
of result/IR rows. Profile manager and archive tests cover verified, missing,
user-deleted, corrupt, mismatched, and unreferenced files plus failure
injection at copy, validation, sync, and finalize boundaries. Startup tests pin
the ordering and non-fatal reconciliation policy. The complete slice diff is
then reviewed against `origin/develop` for duplicate file-state authorities.
