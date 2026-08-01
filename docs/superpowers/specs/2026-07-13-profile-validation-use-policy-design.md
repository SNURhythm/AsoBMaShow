# Profile Validation Use Policy Design

## Context

`PlayerProfileManager` currently passes `ValidationDepth` and
`DatabaseVersionPolicy` independently at each call site. The public catalog
uses routine validation with `AllowSupportedOlder`, while rename and delete use
the strict public `validateProfile()` path and delete counts only current-schema
profiles.

That creates a contradictory UI contract. A supported older-schema inactive
profile is returned by `listProfiles()` and therefore receives the normal
rename, duplicate, activate, and delete actions, but rename/delete reject it.
If only one current profile and one supported older profile exist, deletion also
misreports the older profile as the last profile because the guard counts only
the current profile.

Duplicate and overwrite already use deep validation with
`AllowSupportedOlder`, activation intentionally admits supported older schemas
before the score/replay database owners migrate them, and runtime commit/export
paths intentionally require current schemas. The defect is therefore policy
drift, not a request to weaken runtime validation.

## Goals

- Make every profile operation select one semantic validation use instead of
  manually pairing two low-level validation flags.
- Let supported older-schema profiles shown in the catalog be renamed and,
  when inactive and not the last manageable profile, deleted.
- Count all deeply valid manageable profiles for destructive last-profile
  guards.
- Preserve deep integrity validation, future-schema rejection, confinement,
  active-profile protection, and strict current-schema runtime readiness.
- Leave database migration ownership in `ScoreDBHelper` and `ReplayDBHelper`.

## Non-goals

- Do not migrate databases while listing, renaming, deleting, duplicating, or
  overwriting profiles.
- Do not make future-schema or corrupt profiles manageable.
- Do not make routine catalog validation deep, change catalog sorting, or add
  new public APIs.
- Do not change transaction finalization, archive staging, profile switching,
  or bootstrap formats.

## Central Policy

Replace the private two-argument policy surface with one semantic enum used by
manager and path-based validation. Declare it opaquely in the header and define
its values in the implementation so callers do not gain a new operation API:

```cpp
// PlayerProfileManager.h, at namespace scope
enum class ProfileUse : unsigned char;

// PlayerProfileManager.cpp, before the anonymous-namespace helpers
enum class ProfileUse : unsigned char {
  Catalog,
  Manage,
  Activate,
  RuntimeReady,
};
```

The manager's private overloads name this namespace-scope type. Defining it
before the anonymous namespace also lets `validateProfileFiles`, transaction
finalizers, and recovery helpers use the same semantic values without moving
them into the class or exposing a new public method.

`validateProfile(id, ProfileUse)` and an exhaustive `validationPolicy(ProfileUse)`
mapping are the only places that translate use into low-level validation
behavior:

| Use | Depth | Database policy | Purpose |
|---|---|---|---|
| `Catalog` | Routine | Allow supported older | Discover and display profiles without expensive integrity checks |
| `Manage` | Deep | Allow supported older | Rename, duplicate, delete, overwrite, and destructive profile counts |
| `Activate` | Deep | Allow supported older | Preflight a switch before database owners migrate supported schemas |
| `RuntimeReady` | Deep | Current only | Commit/use/export paths that require migrated databases |

`Manage` and `Activate` deliberately map to the same low-level checks today but
remain distinct semantic uses. Activation and management have different
contracts and may diverge later; call sites should express intent rather than
reconstruct flags.

The private list helper becomes `listProfiles(ProfileUse)`. The public
`listProfiles()` remains the catalog API. The public `validateProfile()` remains
strict runtime-ready validation, and `validateProfileForActivation()` remains
the activation preflight API.

The mapping returns a small implementation policy value containing deep/routine
depth and supported-older/current-only compatibility. The path-based
`validateProfileFiles` helper accepts `ProfileUse` too, so transaction helpers
express a semantic purpose rather than passing a raw compatibility boolean.

## Call-site Assignment

- Initial bootstrap admission: `Catalog`, preserving its existing routine,
  read-only startup preflight.
- Bootstrap-backup recovery, orphan selection, and future-profile scan:
  `Activate`, preserving deep supported-older validation.
- Public catalog: `Catalog`.
- Duplicate source, rename target, delete target, and overwrite target:
  `Manage`.
- Delete and overwrite last-profile counts: `Manage`.
- Delete source/tombstone reconciliation: the database compatibility derived
  from `Manage`. This is essential because the deletion finalizer deeply
  revalidates both paths after ambiguous filesystem calls.
- Activation preflight: `Activate`.
- Public strict validation and `commitActiveProfile`: `RuntimeReady`.
- New-profile finalization and imported staging validation: `RuntimeReady`.
- Overwrite recovery destination/backup validation: `Manage`.

No call site should independently pair validation depth and database
compatibility. `validationPolicy(ProfileUse)` is the only source of those
low-level choices.

## Behavior and Safety

A supported older-schema profile is manageable only when every non-version
deep check succeeds: UUID/metadata, settings/input versions, safe contained
paths, practice directory, SQLite readability, supported version bounds, and
SQLite integrity. Rename changes metadata only and must leave database schema
versions and rows untouched. Delete uses the already-verified transaction
state machine and remains blocked for the active profile or when only one
manageable profile exists.

Future-schema profiles continue to return `FutureVersion`. Present but corrupt,
unsafe, or unreadable profiles continue to fail closed. `RuntimeReady` remains
current-only, so direct validation and the final active-profile commit cannot
silently admit a database that its owners have not migrated.

## Test Strategy

Add deterministic manager regressions before production changes:

1. Create a current active profile and an inactive profile, downgrade the
   inactive score/replay `user_version` values to supported older versions, and
   confirm it remains cataloged and activation-ready while strict public
   validation rejects it.
2. Rename the older inactive profile successfully; assert only metadata changes
   and both database versions and marker rows remain unchanged.
3. Duplicate the older profile; assert the source stays at its old versions,
   the copy is migrated to current versions by its normal database owners, and
   marker rows survive.
4. Return to exactly the current active profile and older inactive profile, then
   delete the older profile successfully with no warning or retained tombstone.
   This proves target validation, mixed-schema last-profile counting, and
   successful tombstone reconciliation use `Manage`.
5. Inject a deletion commit-sync failure for a supported older target and prove
   rollback restores a deeply valid source, reports `IoFailure`, and preserves
   its old versions, marker rows, and catalog entry. This exercises the
   finalizer's source-validation path, not only its successful tombstone path.
6. Characterize session activation of a supported older inactive profile: the
   preflight admits it, score/replay binders migrate it, runtime-ready commit
   succeeds, and rows survive.
7. Characterize archive policy: export remains runtime-ready strict, while an
   inactive supported-older overwrite target is manageable and receives the
   current imported profile.
8. Set an inactive profile database `user_version` to current-plus-one and prove
   catalog, rename, delete, duplicate, activation preflight, runtime commit, and
   overwrite target checks reject it without profile or transaction-artifact
   mutation.
9. Preserve the existing startup/recovery, transaction fault-matrix, and strict
   runtime/export tests.

Add a source audit scoped to `PlayerProfileManager.h/.cpp` that finds semantic
`ProfileUse` call sites and rejects raw profile validation-policy selection
outside the centralized mapping. The audit must explicitly permit the opaque
enum declaration/definition and low-level policy names inside
`validationPolicy()` itself. Run profile manager, switch, and archive tests,
then the desktop build and complete CTest suite.

## Acceptance Criteria

- A supported older inactive profile shown in settings can be renamed and
  deleted under the same safety guards as a current profile.
- Mixed current/older profile counts do not trigger a false last-profile error.
- Rename and all validation/preflight operations remain read-only for database
  schema and rows.
- Management and runtime operations continue to reject and preserve future,
  corrupt, unreadable, or unsafe profiles.
- Runtime-ready validation stays current-only and schema migration remains
  owned by the database helpers.
- One semantic policy mapping is the only source of truth for validation depth
  and database-version compatibility.
