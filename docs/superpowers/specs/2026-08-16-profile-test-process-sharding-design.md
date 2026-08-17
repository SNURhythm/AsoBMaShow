# Profile Test Process-Sharding Design

## Goal

Split the two remaining long foundation-profile test executables into
independent CTest processes so `ctest -j 6` can schedule their cases in
parallel. The scope is limited to `foundation_profile_archive` and
`foundation_profile_manager`; `foundation_profile_switch` is already about
three seconds after fixture caching and is not a split candidate.

## Evidence

- A six-way full CTest run completed 255 tests in 21.03 and 22.70 seconds.
  The prior serial run was 78.90 seconds.
- In the parallel runs, `foundation_profile_archive` remained the wall-time
  limiter at 21.02 and 22.69 seconds because all of its cases share one CTest
  process.
- `foundation_profile_manager` also ran as one process and took 14.29 and
  15.95 seconds under six-way contention.
- Both executables already give each test case an isolated temporary root.
  Their static fixture caches are process-local and can be recreated safely in
  each shard.

## Design

Each existing executable gains an explicit shard selector and preserves a
default all-shards mode for direct execution.

`profile_archive_tests` shards:

1. `portable` — export, ordinary import, compatibility, and overwrite cases.
2. `validation` — hostile member, checksum, database-version, and size-limit
   validation.
3. `transactions` — export transaction handling and startup recovery.
4. `faults` — overwrite/create/cleanup fault matrices and rollback recovery.

`player_profile_manager_tests` shards:

1. `bootstrap` — first-run migration, durable creation, and create/duplicate
   fault coverage.
2. `deletion` — deletion fault matrix, tombstones, and profile-policy cases.
3. `integrity` — CRUD constraints, profile validation, practice, replay, and
   future-version coverage.

The case lists will be explicit. Every existing case appears in exactly one
shard, while the default mode invokes all three or four shard lists in the
same order as today. Unknown selectors will fail with a useful diagnostic.

`CMakeLists.txt` will register each selector as its own CTest test, rather
than registering the all-shards executable. The CTest names will retain their
foundation-profile prefixes so existing regular-expression workflows can
select the whole family. The iOS release-verification allowlist and its
contract test will be updated to require all new shard names.

## Safety and Verification

- Test cases continue to create isolated on-disk state; no shard relies on a
  preceding shard's process state.
- Process-local immutable fixture caches remain private to their shard, so
  mutations cannot cross test or process boundaries.
- Add a selector regression check: every supported shard succeeds, and an
  unknown selector exits nonzero without running tests.
- Build both targets, run every shard serially, then run the full suite twice
  with `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
- Retain the split only if both full parallel runs pass and improve on the
  established 21.03–22.70 second parallel baseline.

## Alternatives Rejected

Building a separate executable for each shard would duplicate the substantial
profile/archive compile and link graphs. Filtering within the existing
executables provides process-level CTest parallelism without that build cost.
