# Profile Directory Transaction Outcomes Design

**Date:** 2026-07-13
**Branch:** `chore/refactor-2`

## Purpose

Make profile create, duplicate, import, and delete results agree with the
filesystem outcome visible to the running application. Remove transaction
state that competes with the filesystem, and give every profile-directory
mutation one authoritative terminal decision.

This is the second focused remediation from the `chore/refactor-2` audit. It
does not change profile database schemas, inactive older-schema eligibility,
archive formats, or runtime profile activation.

## Confirmed Defects

### Create and duplicate can fail after installing a profile

`buildProfile` makes its staging tree durable, renames it to the canonical UUID
directory, and then syncs `profiles/`. If that final parent sync fails, the
method reports `IoFailure`. The generic failure cleanup still targets the old
staging path, so the valid canonical profile remains visible. A retry can then
create a duplicate even though the first operation appears to have failed.

`BuildProfileResult` compounds the problem with three competing outcomes:

- `result`, which callers return;
- `finalized`, which records that rename occurred but is never read;
- `paths`, which is also never read.

The filesystem is authoritative, but the returned result is computed without
reconciling it.

### Delete can fail after logical deletion committed

Delete renames a canonical UUID directory to `.deleting-<uuid>` and syncs the
`profiles/` parent. That successful sync is the logical commit: the profile is
no longer addressable, and startup cleanup will finish removing its tombstone.

The current method nevertheless returns `IoFailure` when tombstone removal or
the later cleanup sync fails. The user is told deletion failed even though the
profile is already absent and will not be restored.

Delete also treats an ambiguous rename or a failed commit sync as an ordinary
pre-mutation failure. It does not inspect whether the source or tombstone won,
and it does not attempt to restore the source before the commit point.

## Existing Proven Behavior

The non-overwrite `installProfile` path already has the required new-directory
state machine. It:

- reconciles a rename that returned failure after actually moving the tree;
- rolls back when the first parent sync fails;
- reconciles an ambiguous rollback;
- returns success with a warning when rollback is unavailable and a single
  valid canonical destination remains.

Its eight-point fault matrix proves that the returned result matches the
recovered filesystem outcome. This behavior should become the common source
for ordinary create, duplicate, and non-overwrite import.

## Considered Approaches

### Patch every call site independently

Adding local rollback checks to `buildProfile` and post-commit warnings to
`deleteProfile` would minimize the immediate diff. It would retain two copies
of the same new-profile finalization policy in `buildProfile` and
`installProfile`, making later drift likely. This is not selected.

### Build a generic directory-transaction framework

A callback-driven abstraction could model create, delete, overwrite, and
migration. Those operations have different commit predicates, validation
rules, and recovery artifacts (`.staging-*`, `.deleting-*`, `.backup-*`). A
generic framework would obscure rather than clarify their safety rules. This
is not selected.

### Extract two domain-specific terminal state machines

Extract the proven new-profile finalizer and add a sibling deletion finalizer.
Each helper has one operation-specific commit rule and returns `ProfileResult`
directly. This removes duplicated policy without conflating unrelated
transactions. This is the selected approach.

## Architecture

Both helpers remain private to `PlayerProfileManager.cpp`. No public API,
header, CMake target, or Xcode membership changes are required.

### `finalizeNewProfileDirectory`

The helper receives the application root, filesystem dependencies, staging and
destination paths, and the completed `PlayerProfile`. It exclusively owns:

- the final staging-to-destination rename;
- parent-directory commit sync;
- rollback and ambiguous-rollback reconciliation;
- staging cleanup after rollback;
- the terminal `ProfileResult`.

`buildProfile` returns `ProfileResult` directly. `BuildProfileResult`, its dead
`paths`, and its dead `finalized` flag are removed. Create and duplicate return
the helper result without another interpretation. The non-overwrite branch of
`installProfile` delegates to the same helper after it has produced and synced
valid staging content.

Migration intentionally keeps its separate final rename policy. During
first-run initialization, a retained finalized profile with no bootstrap is a
startup recovery artifact. More importantly, migration must not write a
bootstrap after a repeatedly failed parent sync. The existing migration and
bootstrap-failure recovery tests remain unchanged.

Overwrite also remains separate because it owns two complete versions and a
`.backup-*` recovery policy.

### `finalizeProfileDeletion`

The deletion helper receives the validated profile and its canonical source
and tombstone paths. It exclusively owns:

- cleanup of a prior tombstone;
- source-to-tombstone rename and ambiguity inspection;
- the first parent sync, which is the logical deletion commit point;
- pre-commit rollback to the canonical source;
- post-commit tombstone cleanup;
- the terminal `ProfileResult`.

Eligibility checks remain in `deleteProfile`; once the mutation begins, the
helper is the only component that decides whether deletion succeeded.

## New-Profile State Machine

1. Attempt the durable rename from staging to the canonical destination.
2. If rename reports failure, inspect both paths and deeply validate any
   destination:
   - any valid canonical destination means installation is the sole
     user-visible winner, even if a duplicate staging tree also remains; clean
     staging when possible, retry the parent sync, and return success with a
     warning for deferred cleanup or unconfirmed durability;
   - no valid destination means no usable installation occurred; remove any
     invalid destination, clean staging when possible, and return failure;
   - a path that cannot be inspected safely returns failure and retains
     evidence rather than guessing.
3. If rename succeeds, sync `profiles/`. A successful sync is the ordinary
   commit point and returns success.
4. If the commit sync fails, attempt destination-to-staging rollback.
5. If rollback wins, sync the parent, clean staging, sync cleanup when
   possible, and return failure. No canonical profile remains visible.
6. If rollback reports failure, inspect both paths:
   - a valid destination means installation remains the visible winner; clean
     duplicate staging when possible, retry the parent sync, and return
     success with a warning for any remaining cleanup or sync uncertainty;
   - no valid destination means rollback won or no usable installation
     exists; remove any invalid destination, clean staging when possible, and
     return failure;
   - an unsafe inspection returns failure while preserving evidence.

The rule is simple: a valid canonical destination produces success; no valid
canonical destination produces failure. A nonempty success message carries
durability or cleanup warnings through the existing controller warning UI.

## Deletion State Machine

1. Remove a preexisting `.deleting-<uuid>` tombstone and sync that cleanup
   before beginning a new deletion. A removal or cleanup-sync failure returns
   failure while the canonical source is still untouched.
2. Attempt the durable source-to-tombstone rename.
3. If rename reports failure, inspect both paths and deeply validate any
   tombstone:
   - a valid canonical source means deletion did not win and returns failure;
     an extra tombstone is cleanup evidence, not a successful deletion;
   - no valid canonical source means deletion is the sole user-visible
     outcome; a complete tombstone proceeds to the commit sync, while a
     missing or invalid tombstone or a physically present but invalid source
     is cleaned when possible and returns success with an
     integrity/durability warning;
   - a path that cannot be inspected safely returns failure without destroying
     evidence.
4. Sync `profiles/`. The first successful sync after source-to-tombstone rename
   is the logical deletion commit point.
5. If that sync fails, attempt tombstone-to-source rollback and reconcile an
   ambiguous rollback:
   - a valid canonical source means restoration won and returns failure after
     a rollback sync attempt; any extra tombstone is retained or cleaned as
     recovery evidence;
   - no valid canonical source means deletion remains the sole visible
     outcome; retry the commit sync, clean an invalid source or tombstone when
     possible, and return success with a warning when durability or cleanup
     still cannot be reconfirmed;
   - an unsafe inspection returns failure and retains evidence.
6. After commit, remove the tombstone. Removal failure, partial cleanup, or the
   final cleanup-sync failure returns success with a warning. Startup already
   retries `.deleting-*` cleanup, and a committed deletion is never rolled
   back merely because physical cleanup is incomplete.

## Error and Warning Contract

- Before a commit point, a successfully restored pre-operation state returns
  failure because the requested mutation did not occur.
- After a commit point, cleanup failures return success with a warning because
  the requested logical mutation did occur.
- If no parent sync succeeds and rollback is unavailable, `ProfileResult`
  describes the one safe, exclusive state visible to the running application.
  Its warning explicitly records that crash durability could not be
  reconfirmed; it does not describe that fallback as a durable commit.
- When an operation reports an error after an ambiguous filesystem call, the
  helper inspects source, staging/tombstone, and profile validity before
  deciding.
- Warnings never claim durability that could not be reconfirmed; they state
  that sync or cleanup should be retried.
- Future initialization remains responsible for removing hidden staging and
  tombstone artifacts.

## Testing

Add deterministic dependency-injection matrices to
`tests/player_profile_manager_tests.cpp`.

For both create and duplicate, cover:

1. final rename fails before moving;
2. final rename moves and then reports failure;
3. commit sync fails and rollback succeeds;
4. commit sync fails and rollback is unavailable;
5. rollback moves and then reports failure;
6. rollback staging cleanup fails;
7. rollback parent sync fails;
8. rollback unavailable and repeated commit sync fails.

Add classification cases where an ambiguous call leaves both a valid
destination and staging, leaves an invalid destination, or leaves neither.
The valid destination is success with staging cleanup; invalid or absent
destinations are failures. Inject an unsafe inspection and assert it fails
closed without destructive cleanup.

Also cover the distinct sync branches after reconciliation: an ambiguous final
rename whose retry parent sync fails, and a successful rollback whose later
staging-cleanup sync fails. These must not be conflated with the initial commit
sync or rollback-parent sync failures.

Each point asserts result status equals canonical destination visibility,
clean manager reinitialization preserves that outcome, hidden artifacts are
recovered, and point 8 returns a warning. Duplicate success also proves the
source payload is preserved.

For deletion, cover:

1. rename fails before moving;
2. rename moves and then reports failure;
3. commit sync fails and rollback succeeds;
4. rollback unavailable and retry sync succeeds;
5. rollback moves and then reports failure;
6. tombstone cleanup fails without mutation;
7. tombstone cleanup partially removes data;
8. final cleanup sync fails;
9. rollback unavailable and repeated commit sync fails.

Separately cover stale-tombstone removal failure and stale-cleanup sync failure
before mutation. Add ambiguity cases with source plus tombstone, source absent
plus an invalid tombstone, a physically present but deeply invalid source, and
both paths absent. A safely inspected valid canonical source produces failure;
the absence of a valid source produces success with a warning and cleanup
attempt; unsafe inspection fails closed. Cover rollback-parent sync failure as
a distinct pre-commit branch.

Each point asserts `ProfileResult` matches `listProfiles()`, clean manager
reinitialization preserves the reported logical outcome, and recoverable
tombstones are eventually removed. Existing import/overwrite fault matrices
remain green and prove that extracting the new-profile helper did not change
archive behavior.

## Verification

Run:

```sh
cmake --build cmake-build-debug \
  --target player_profile_manager_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_archive)$'
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
```

No Firebase deployment is part of this remediation.

## Acceptance Criteria

- Create, duplicate, and non-overwrite import use one finalization policy.
- `BuildProfileResult` and its unused state are removed.
- A valid retained canonical profile is never reported as a failed create or
  duplicate.
- A failed create or duplicate leaves no deeply valid canonical profile; an
  invalid directory may remain only when cleanup itself fails.
- Delete reports failure when the source is restored before commit.
- Delete reports success with a warning when cleanup fails after commit.
- After a safely classified ambiguous delete, result success matches the
  absence of a valid canonical source and result failure matches a retained
  valid source.
- Ambiguous rename and rollback outcomes are reconciled against filesystem
  state and deep validation.
- Clean manager reinitialization preserves every safely classified reported
  logical outcome; hidden or invalid recovery artifacts may be removed without
  changing it. Repeated parent-sync warnings make no stronger power-loss
  durability claim.
- Existing migration, overwrite, archive, and desktop behavior remains green.
