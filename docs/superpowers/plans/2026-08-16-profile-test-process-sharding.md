# Profile Test Process-Sharding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:test-driven-development` while implementing each source change, and `superpowers:verification-before-completion` before reporting success.

**Goal:** Split the long `foundation_profile_archive` and
`foundation_profile_manager` CTest entries into selector-driven processes, so
the repository's normal `ctest -j 6` run can schedule their independent cases
concurrently. Leave `foundation_profile_switch` as one process.

**Architecture:** Keep one executable and one compile/link graph per existing
test target. Each executable gains `--shard <name>` and named shard runners;
the no-argument invocation retains the original complete test order. CMake
registers a CTest entry per shard, each passing the selector to the same test
binary. The iOS release verifier builds each binary once and selects every
successful shard CTest name.

**Tech Stack:** C++23, `std::filesystem`, CMake/CTest, shell/Python release
workflow contract tests.

## Constraints

- Work only in the current shared checkout; do not create a worktree.
- Do not change profile/archive production behavior. This is test execution and
  build-registration work only.
- Every existing case must occur in exactly one named shard. With no selector,
  the executable must still execute the complete suite in its present order.
- Each shard must retain the existing isolated temporary filesystem setup; no
  test case may share mutable fixture data across processes.
- Keep `profile_switch_tests` and `foundation_profile_switch` untouched.
- Do not commit or otherwise publish the changes unless the user asks.

## Task 1: Make test-process temporary roots collision-safe

**Files:**
- Modify: `tests/player_profile_manager_tests.cpp:40-63`
- Modify: `tests/profile_archive_tests.cpp:69-92`

**Why:** The current names combine a process-local atomic counter with a clock
sample. Once the same binary runs in several CTest processes, a collision must
be retried rather than being silently accepted as an existing shared root.

- [ ] Replace each `TempDirectory` constructor's one-shot
  `create_directories(path_)` with a bounded/retrying `create_directory`
  candidate loop. A candidate succeeds only when this process created the
  final leaf. If the leaf already exists, generate a fresh clock/counter
  candidate; report any other filesystem error.
- [ ] Preserve the existing label format and destructor cleanup semantics.
  Do not make fixture paths global or add a resource lock, because that would
  serialize the shards the change is intended to parallelize.
- [ ] Build both targets and use two concurrent invocations of each selector
  once selectors exist (Task 5) as the end-to-end collision regression.

## Task 2: Add explicit, testable shard selection to both binaries

**Files:**
- Modify: `tests/player_profile_manager_tests.cpp:3102-3137`
- Modify: `tests/profile_archive_tests.cpp:3368-3412`

**Interfaces:**
- Input: no arguments, or exactly `--shard <name>`.
- Output: exit 0 after the selected cases pass; exit 1 for assertion failures;
  exit 2 plus usage text for an unknown/malformed selector.

- [ ] First add the selector parser and the failure path: malformed arguments
  and an unsupported shard must emit a diagnostic and return 2 *before*
  calling any case. Verify directly that
  `profile_archive_tests --shard unknown` and
  `player_profile_manager_tests --shard unknown` exit nonzero.
- [ ] Extract the current calls from each `main` into named, no-argument shard
  runners. Keep `failures` process-local, preserve the current final summary,
  and make default invocation call runners in the exact original case order.
  A valid selector calls only its runner.
- [ ] Use the following complete, non-overlapping assignment. The ordering of
  these runner lists is also the no-selector order.

  `player_profile_manager_tests`:

  - `bootstrap`: `testSqliteSnapshotIncludesWalAndValidatesIdentifiers`,
    `testFirstRunCreatesMissingApplicationDataRoot`,
    `testFirstRunMigrationIsLosslessAndIdempotent`,
    `testEveryPreFinalizeFailureCleansStaging`,
    `testFinalizedOrphanRecoversAfterBootstrapFailure`,
    `testDurableFinalizePrecedesBootstrapAndRecoversAfterSyncFailure`,
    `testFailedCreateCommitSyncLeavesNoVisibleProfile`,
    `testFailedDuplicateCommitSyncLeavesNoVisibleProfile`,
    `testNewProfileFaultFixturesReuseOneSeedAndRemainIsolated`, and
    `testCreateAndDuplicateFaultMatrixHasFilesystemDerivedOutcomes`.
  - `deletion`: `testProfileDeletionFaultFixturesReuseOneSeedAndRemainIsolated`,
    `testProfileDeletionFaultMatrixHasFilesystemDerivedOutcomes`,
    `testProfilesRootSymlinkNeverEscapesApplicationRoot`,
    `testPartialTombstoneCleanupNeverRestoresOrExposesProfile`,
    `testFailedDeleteCommitSyncRestoresProfile`,
    `testFutureLegacyDatabaseVersionFailsClosedBeforeMigration`,
    `testSupportedOlderInactiveProfileUsesManagePolicy`,
    `testSupportedOlderDeleteRollbackRestoresManageableSource`,
    `testFutureDatabaseProfileIsNeverManageable`, and
    `testSupportedOlderActiveProfileWaitsForSchemaOwners`.
  - `integrity`: `testProfileCrudConstraintsAndDataIsolation`,
    `testOptionalOperationalFilesRejectLinksWithoutTouchingTargets`,
    `testPracticeDirectoryLifecycleAndValidation`,
    `testReplayDirectoryDuplicationUsesOwnedVerifiedInventory`, and
    `testFutureVersionsFailClosed`.

  `profile_archive_tests`:

  - `portable`: the first 17 current calls, from
    `testFixtureSeedIsBuiltOnceAndClonesAreIsolated` through
    `testOverwriteRollbackRestoresOriginalProfile`. This contains normal
    export/import, compatibility, and overwrite coverage (including the
    standalone rollback scenario).
  - `validation`: the next 7 calls, from
    `testStrictMemberAllowlistAndTypes` through
    `testFutureDatabaseAndCorruptionAreRejected`.
  - `transactions`: the next 8 calls, from
    `testExportFailurePreservesDestinationAndCleansTemps` through
    `testStartupRecoversInterruptedProfileOverwrite`.
  - `faults`: the final 3 fault-matrix calls:
    `testOverwriteFaultMatrixRecoversOneCompleteProfile`,
    `testCreateImportFaultMatrixHasUnambiguousOutcome`, and
    `testCommittedOverwriteSurvivesBackupCleanupFailures`.

- [ ] Add normal CTest registrations in Task 3, then prove the new parser via
  the real executable: each of the seven supported selectors passes; each
  executable rejects `unknown` with no test summary. This is the selector
  regression rather than a mock of command-line parsing.

## Task 3: Register shard processes in CMake and retain test-target setup

**Files:**
- Modify: `CMakeLists.txt:6111-6134`
- Modify: `CMakeLists.txt:6476-6479`

**Interfaces:**
- Current `asobmashow_register_test(target [name])` registrations retain their
  behavior.
- New registrations pass positional command arguments to `add_test` without
  reapplying compile options or MSVC diagnostic sources to the same target.

- [ ] Refactor the existing test-target preparation and test registration just
  enough to support one prepared executable being registered many times. Keep
  the existing name-only call sites source-compatible. Ensure `SDL_MAIN_HANDLED`,
  `-UNDEBUG`/`/UNDEBUG`, and the MSVC diagnostics source are added only once
  per target.
- [ ] Replace the single registrations with these successful CTest names and
  commands:

  | Target | CTest name | Command arguments |
  | --- | --- | --- |
  | `player_profile_manager_tests` | `foundation_profile_manager_bootstrap` | `--shard bootstrap` |
  |  | `foundation_profile_manager_deletion` | `--shard deletion` |
  |  | `foundation_profile_manager_integrity` | `--shard integrity` |
  | `profile_archive_tests` | `foundation_profile_archive_portable` | `--shard portable` |
  |  | `foundation_profile_archive_validation` | `--shard validation` |
  |  | `foundation_profile_archive_transactions` | `--shard transactions` |
  |  | `foundation_profile_archive_faults` | `--shard faults` |

  Do not register the former unsuffixed aggregate names: otherwise a full
  CTest run would repeat every profile case in a serialized process.
- [ ] Register one fast invalid-selector CTest per executable, set each test's
  `WILL_FAIL` property, and name it outside the release selector family (for
  example `profile_archive_invalid_shard`). It protects the nonzero unknown
  selector contract without causing the iOS release regex to include it.
- [ ] Reconfigure CMake and inspect `ctest -N -V` to confirm that each
  successful CTest command contains the intended two selector arguments and
  that no unsuffixed foundation-profile archive/manager test remains.

## Task 4: Keep the iOS release-critical selection exact

**Files:**
- Modify: `scripts/ios_release_verify.sh:8-83`
- Modify: `tests/ios_release_workflow_tests.py:13-65, 224-246`

- [ ] Continue building `player_profile_manager_tests` and
  `profile_archive_tests` once each in `NATIVE_TEST_TARGETS`.
- [ ] Replace the two unsuffixed profile alternatives in
  `NATIVE_CTEST_PATTERN` with the seven successful shard names (or equivalent
  exact grouped alternatives). Keep the switch entry unchanged.
- [ ] Change the release-workflow contract fixture from a single
  target-to-registered-name mapping to an assertion that each profile target
  has its complete expected CTest-name collection. Preserve the existing
  single-name checks for all other release-critical targets.
- [ ] Run `python3 tests/ios_release_workflow_tests.py` and
  `scripts/ios_release_verify.sh --dry-run`; the dry run must be stable across
  two runs, build each binary once, and select all seven shard names.

## Task 5: Verify correctness, concurrency, and actual wall-clock gain

**Files:**
- Verify: all files above

- [ ] Build the two targets:

  ```bash
  cmake --build cmake-build-debug --target \
    player_profile_manager_tests profile_archive_tests -j 6
  ```

- [ ] Run every successful shard once serially, then run the two invalid
  selector CTests. Also invoke each binary with no selector to preserve the
  direct complete-suite contract.
- [ ] Stress process isolation by launching two same-target shard CTests at
  once with CTest parallelism, for example the archive `portable` and
  `validation` cases in one `ctest -j 2` command and two manager shards in
  another. Confirm no temporary-root collision, fixture mutation, or cleanup
  failure occurs.
- [ ] Run the full suite twice with the documented command:

  ```bash
  /usr/bin/time -lp ctest --test-dir cmake-build-debug --output-on-failure -j 6
  ```

  Both runs must pass. Retain the split only if the new wall time improves on
  the established 21.03–22.70 second six-way baseline; report both raw runs,
  the prior range, and any change in CTest count due to the two expected-fail
  selector checks.
- [ ] Run `git diff --check` and `git status --short`. Verify no production
  profile/archive source and no `profile_switch_tests` files changed as part
  of this task.

## Task 6: User-approved rebalance from per-case timings

The initial split proved faster, but the timing preflight showed two thin
shards (`integrity` at about 1.1s and archive `transactions` at about 1.7s)
beside larger 4.4–4.7s shards. The user explicitly authorized reordering the
long-running chunks. This amendment overrides the earlier no-argument
original-order constraint: no-argument mode still runs every case exactly
once, in the balanced runner order, but does not preserve the former source
call sequence.

Keep the seven public selector and CTest names unchanged. Reassign only the
case membership as follows, using the measured second-run durations as the
expected balance check:

| Executable and shard | Cases | Target measured duration |
| --- | --- | ---: |
| manager `bootstrap` | temporary-root regression; SQLite snapshot; new-profile fixture-cache regression; create/duplicate fault matrix | ~3.60s |
| manager `deletion` | deletion fixture-cache regression; deletion fault matrix; symlink; partial tombstone; failed delete sync; future legacy DB; supported older inactive; supported older delete rollback; supported older active | ~3.41s |
| manager `integrity` | first-run root/migration; pre-finalize cleanup; finalized orphan; durable finalize; failed create/duplicate sync; future database not manageable; CRUD; optional-file links; practice; replay inventory; future versions | ~2.32s |
| archive `portable` | temporary-root and fixture-cache regressions; streaming hash; deterministic export; IR non-portability; older source; sidecar; malformed practice; Unicode; create round-trip/UUID retry; overwrite restrictions/older/future/last-profile/rollback | ~3.56s |
| archive `validation` | create-import fault matrix; checksum/version/validator limits; size policy; ZIP parser limits; supported-older migration; future/corrupt DB | ~3.42s |
| archive `transactions` | replay round-trip; v1 archive import; strict member allowlist; all eight export/startup transaction cases | ~3.71s |
| archive `faults` | practice-member validation; overwrite fault matrix; committed-overwrite cleanup matrix | ~3.74s |

After reassignment, rebuild both targets, run all seven selectors and both
no-argument modes, time each selector directly, and repeat the fresh
two-consecutive-pass full-suite gate with `ctest -j 6`. Retain the rebalance
only if both runs pass and improve on the prior sharded 19.18–19.27s pair (or
otherwise demonstrably improve the scheduler's wall clock without regressing
reliability).

**Outcome:** The reassignment passed all focused checks and the two full-suite
gates, but measured 19.67s and 19.28s. It did not beat the 19.18–19.27s
retained-sharding pair, so its runner membership and default order were
reverted. The current source keeps the original successful split; see
`task-6-report.md` for the experimental timing and scoped-revert evidence.

## Task 7: Fix final-review parallel safety and release-contract gaps

The final cross-file review reproduced an archive-shard race:
`testStaleArchiveWorkspacesAreSweptWithoutTouchingFreshOnes` used two fixed
global `/tmp/asobmashow-profile-import-<32-hex>` paths. Concurrent
`transactions` processes can delete one another's fresh workspace. Its stale
and fresh suffixes must remain valid 32-character lowercase hexadecimal
tokens, but be generated independently per invocation (for example, remove
the dashes from two `uuid::generateV4()` values). Do not weaken the production
stale-workspace parser or test only one process.

- [ ] First reproduce the failure with two concurrent `transactions` CTest
  processes. After changing the test fixture, run a repeated same-selector
  two-process stress loop that would formerly race; retain its full failure
  output if any round fails.
- [ ] Assert the generated workspace suffixes are distinct and remain valid
  lower-hex tokens before creating the stale/fresh directories. Keep the
  stale timestamp and fresh-retention assertions unchanged.
- [ ] Add a release-workflow contract check that each profile test executable
  occurs exactly once inside `NATIVE_TEST_TARGETS`, rather than merely
  somewhere in the shell script. Keep the existing exact CTest-family check.
- [ ] Build/rerun the archive target, all profile shards, both no-argument
  binaries, the workflow Python test, and two fresh full `ctest -j 6` runs.
  Repeat final review after the fix.
