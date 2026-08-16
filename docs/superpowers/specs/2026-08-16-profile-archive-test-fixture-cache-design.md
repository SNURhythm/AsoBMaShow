# Profile Archive Test Fixture Cache Design

## Goal

Reduce the runtime of `foundation_profile_archive` while preserving its existing
archive, import, filesystem, and profile-isolation coverage.

## Evidence

- A serial CTest baseline completed 255 tests in 127.79 seconds.
- `foundation_profile_archive` was the longest test at 30.76 seconds.
- It constructs the `Fixture` 33 times. Each construction initializes an active
  profile and creates source and target profiles.
- Process sampling showed that this repeated setup spends most of its time in
  fresh replay-database schema creation and migration, reached through
  `PlayerProfileManager::createProfile`.
- The production profile code deliberately performs durable file and directory
  synchronization. Those operations and the database migration path remain
  outside this change.

## Decision

Add a process-local, immutable profile-archive seed to
`tests/profile_archive_tests.cpp`.

The seed will be constructed once using the existing production
`PlayerProfileManager` setup and the same deterministic initial profile IDs.
It will contain the active profile, portable source profile, overwrite target,
and the shared settings, database, provenance, and practice-preset data that
the current `Fixture` constructor creates.

Each ordinary `Fixture` will instead copy the seed tree into its already
isolated temporary directory, construct its own manager against that copy, and
run the normal `Initialize` validation. Each fixture keeps its own exchange
directory, UUID generator state, and manager instance.

## Boundaries

- The profile-archive fixture work itself changes no production source,
  durable-write implementation, database schema, migration behavior, or
  archive import/export behavior.
- Fault-injection tests retain their explicit filesystem-operation overrides.
  Their cached seeds reproduce the prior source/target names, markers, UUID
  cursors, and exported archive payloads exactly.
- The seed is never mutated after construction. Test cases mutate only their
  private copied fixture tree.

### Later scope expansion

After this design was written, the user explicitly requested additional
parallel optimization and asked that underlying implementation bottlenecks be
addressed. The independent replay-schema and skin-archive production changes
are therefore outside this test-fixture design and require their own focused
regression coverage and review.

## Verification

Add a focused regression check before the main archive cases that creates two
fixtures, verifies the shared seed was built once, mutates data in one fixture,
and verifies the other fixture still contains the original data. The existing
archive suite continues to validate all normal and fault-path behavior.

Build the affected target, run `foundation_profile_archive`, then run the full
CTest suite serially with the same command used for the baseline. Report both
the focused and total wall-clock times against the 30.76-second and
127.79-second baselines.

## Alternatives Considered

1. Create a production fast path that directly emits the latest replay schema.
   This could improve real first-profile startup, but it would restructure
   security- and recovery-sensitive migration logic. It is out of scope for a
   test-runtime optimization.
2. Skip filesystem synchronization through test dependencies. This would leave
   database initialization as the dominant cost and weaken integration
   coverage of the normal setup path.
3. Rely only on parallel CTest execution. macOS CI already runs CTest in
   parallel, and it does not reduce the 30.76-second focused test.
