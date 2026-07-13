# Profile Directory Transaction Outcomes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make profile create, duplicate, non-overwrite import, and delete results match their safely classified filesystem outcome, while removing the unused `BuildProfileResult` transaction state.

**Architecture:** Extract the proven non-overwrite import finalization logic into one anonymous-namespace new-profile finalizer shared by create, duplicate, and import. Add a separate deletion finalizer whose first successful parent sync is the normal commit point and whose post-commit cleanup failures become success warnings. Keep migration and overwrite separate because their bootstrap/backup recovery contracts differ.

**Tech Stack:** C++23, `std::filesystem`, SQLite, dependency-injected filesystem operations, CMake/Ninja, CTest.

## Global Constraints

- Work from `/Users/xf/workspace/SNURhythm/AsoBMaShow` on `chore/refactor-2`.
- Treat `docs/superpowers/specs/2026-07-13-profile-directory-transaction-outcomes-design.md` as authoritative.
- Use test-driven development: observe a behavioral RED before each production behavior change.
- Do not edit generated `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Keep all new transaction helpers private to `src/PlayerProfileManager.cpp`; do not add a public API or source file.
- Keep first-run migration, overwrite, archive format, schema migration, and profile activation semantics unchanged.
- A deeply valid canonical profile is the only successful create/duplicate outcome. Delete succeeds only when the canonical source is physically absent; a valid or unverifiable present source fails.
- When all parent sync attempts fail, a success warning describes only the exclusive runtime-visible state; it does not claim power-loss durability.
- Unsafe path inspection fails closed and must not trigger destructive cleanup.
- Do not deploy or upload any build.

---

## Task 1: Make create and duplicate use one filesystem-derived outcome

**Files:**

- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `src/PlayerProfileManager.cpp:35-41`
- Modify: `src/PlayerProfileManager.cpp:774-1091`
- Modify: `src/PlayerProfileManager.cpp:1346-1357`
- Modify: `src/PlayerProfileManager.cpp:1451-1495`

- [ ] **Step 1: Add the minimal failing parent-sync regressions**

Add one create fixture and one duplicate fixture that initialize normally, then
enable a one-shot `filesystem.syncDirectory` failure only for the first
`profiles/` sync after the final staging rename. Delegate every other sync to
`atomic_file::syncDirectory`.

For each operation assert:

```cpp
expect(result.error == ProfileError::IoFailure,
       "failed commit sync reports create/duplicate failure");
expect(!std::filesystem::exists(manager.pathsFor(generatedId).root),
       "failed create/duplicate commit leaves no visible destination");
expect(stagingDirectories(temp.path()).empty(),
       "successful rollback cleans hidden staging");
expect(manager.listProfiles().size() == countBefore,
       "reported failure does not change the visible catalog");
```

The failure injection must be disabled before constructing a clean manager and
asserting reinitialization preserves the failed outcome.

- [ ] **Step 2: Run the focused test and confirm RED**

```sh
cmake --build cmake-build-debug --target player_profile_manager_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_profile_manager$' --output-on-failure
```

Expected: the new assertions fail because current `buildProfile` returns
`IoFailure` while the canonical destination still exists and is listed. Record
the exact failing assertion(s).

- [ ] **Step 3: Add the complete create/duplicate fault matrix before production changes**

Add a shared test driver that runs once for create and once for duplicate.
Mirror the injected paths/counters from
`testCreateImportFaultMatrixHasUnambiguousOutcome` in
`tests/profile_archive_tests.cpp`.

Cover these base points:

| Point | Injection | Expected canonical outcome |
|---|---|---|
| 0 | final rename fails before moving | failure, no valid destination |
| 1 | final rename moves then reports failure | success, valid destination |
| 2 | commit sync fails; rollback succeeds | failure, no valid destination |
| 3 | commit sync fails; rollback unavailable | success, valid destination |
| 4 | rollback moves then reports failure | failure, no valid destination |
| 5 | rollback staging cleanup fails | failure, hidden staging recovered on clean reinit |
| 6 | rollback parent sync fails | failure, hidden staging recovered on clean reinit |
| 7 | rollback unavailable; retry commit sync fails | success with warning, valid destination |

Add explicit cases for:

- ambiguous final rename leaving both a valid destination and staging;
- ambiguous final rename followed by retry-sync failure;
- present but invalid/unreadable destination after ambiguous final rename,
  which fails without destructive cleanup;
- neither destination nor staging after ambiguous final rename;
- successful rollback followed by staging-cleanup sync failure;
- unsafe destination inspection using an external symlink, which must fail
  without touching the external target.

For each safely inspected case assert:

```cpp
expect(result.ok() == manager.validateProfile(generatedId).ok(),
       "result matches canonical profile validity");
const auto catalog = manager.listProfiles();
const bool listed = std::ranges::find(
                        catalog, generatedId, &PlayerProfile::id) !=
                    catalog.end();
expect(result.ok() == listed,
       "result matches canonical profile catalog membership");
```

Also assert clean reinitialization preserves the result, hidden artifacts are
cleaned, point 7 has a nonempty warning, and duplicate success preserves source
settings/database data without mutating the source.

Run the focused manager test after adding the matrix. Record every case that is
RED against the current implementation. If a case happens to pass because the
old code never reaches its branch, temporarily apply the precise opposite
mutation to the current production finalization branch, demonstrate the test
fails for the intended reason, then restore it before production work.
No matrix/classification behavior may be implemented before its RED or
controlled-mutation evidence exists.

- [ ] **Step 4: Extract `finalizeNewProfileDirectory`**

Place a private helper after `validateProfileFiles` and before `buildProfile`:

```cpp
ProfileResult finalizeNewProfileDirectory(
    const std::filesystem::path &applicationRoot,
    const PlayerProfileManagerDependencies &dependencies,
    const PlayerProfilePaths &staging,
    const PlayerProfilePaths &destination,
    PlayerProfile profile);
```

Start from the existing non-overwrite `installProfile` state machine at
`PlayerProfileManager.cpp:1971-2054`. Use generic profile messages rather than
archive-specific wording. The helper must own final rename, parent sync,
rollback, reconciliation, cleanup, and terminal result.

Implement these classifications exactly:

- A deeply valid canonical destination is success, whether staging is absent
  or a duplicate hidden staging tree also remains. Attempt staging cleanup and
  include any cleanup/sync uncertainty in the success warning.
- A physically absent destination is failure and permits safe staging cleanup.
  A present destination that does not deeply validate is an integrity or
  inspection exception: return failure and preserve destination plus staging
  evidence. Existing validation collapses some OS read failures into
  `IntegrityFailure`, so validation failure must never authorize deletion.
- A path-existence or containment inspection failure is unsafe: return
  `IoFailure` and preserve evidence.
- If rename succeeds but the first parent sync fails, try
  `destination -> staging`. A safely classified staging-only rollback returns
  failure after rollback/cleanup sync attempts. A valid destination that
  remains returns success, warning when retry sync or cleanup fails.
- Guard against ambiguous rename and rollback calls that move and then return
  false.

Use `validateProfileFiles(applicationRoot, destination, profile.id, false)` for
deep destination validation. Never infer success from `durableRename`'s return
value alone and never delete a present destination merely because validation
failed.

- [ ] **Step 5: Remove `BuildProfileResult` and wire create/duplicate**

Delete:

```cpp
struct BuildProfileResult {
  ProfileResult result;
  PlayerProfilePaths paths;
  bool finalized = false;
};
```

Change `buildProfile` to return `ProfileResult`. Its pre-finalization `fail`
lambda cleans staging and returns `failure(...)` directly.

At `FinalizeProfile`, preserve migration's current logic verbatim:

```cpp
if (mode == BuildMode::Migration) {
  if (!ensureContainedPath(applicationRoot, staging.root, errorMessage) ||
      !ensureContainedPath(applicationRoot, destination.root, errorMessage) ||
      !dependencies.filesystem.durableRename(
          staging.root, destination.root, errorMessage)) {
    return fail(ProfileError::MigrationFailure,
                "unable to finalize profile: " + errorMessage);
  }
  if (!dependencies.filesystem.syncDirectory(
          applicationRoot / "profiles", errorMessage)) {
    return fail(
        ProfileError::MigrationFailure,
        "unable to sync finalized profile directory: " + errorMessage);
  }
  return success(std::move(profile));
}
return finalizeNewProfileDirectory(
    applicationRoot, dependencies, staging, destination,
    std::move(profile));
```

Update `Initialize`, `createProfile`, and `duplicateProfile` to consume the
direct `ProfileResult`; remove every `.result`, `.paths`, and `.finalized`
reference.

- [ ] **Step 6: Confirm all create/duplicate regressions are GREEN**

Run the Task 1 focused commands again.

Expected: `foundation_profile_manager` passes, every new matrix/classification
case matches its safely classified destination outcome, and existing migration
orphan tests remain unchanged and green.

- [ ] **Step 7: Run focused tests, diff-check, and commit**

```sh
cmake --build cmake-build-debug --target player_profile_manager_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_profile_manager$' --output-on-failure
git diff --check
git add src/PlayerProfileManager.cpp tests/player_profile_manager_tests.cpp
git commit -m "fix: reconcile profile creation outcomes"
```

---

## Task 2: Replace the duplicate non-overwrite import finalizer

**Files:**

- Modify: `src/PlayerProfileManager.cpp:1971-2054`
- Verify: `tests/profile_archive_tests.cpp:2208-2331`

- [ ] **Step 1: Establish the existing archive regression baseline**

```sh
cmake --build cmake-build-debug --target profile_archive_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_profile_archive$' --output-on-failure
```

Expected: the complete archive test target, including the eight-point
non-overwrite create fault matrix, passes before refactoring.

- [ ] **Step 2: Delegate non-overwrite import to the common helper**

Replace the complete `if (!overwrite) { ... }` finalization state machine with:

```cpp
if (!overwrite) {
  return finalizeNewProfileDirectory(
      applicationDataRoot_, dependencies_, staging, destination,
      std::move(sourceProfile));
}
```

Do not change staging construction/schema validation before this call or any
overwrite/backup logic after it. Remove archive-specific duplicate
finalization messages along with the deleted branch.

- [ ] **Step 3: Run manager and archive tests**

```sh
cmake --build cmake-build-debug \
  --target player_profile_manager_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_archive)$'
```

Expected: both tests pass. The existing archive fault matrix must retain its
point-by-point success/failure and warning outcomes.

- [ ] **Step 4: Prove the duplicate source of truth is gone and commit**

```sh
rg -n 'if \(!overwrite\)|finalizeNewProfileDirectory|BuildMode::(Create|Duplicate)' \
  src/PlayerProfileManager.cpp
if rg -n 'BuildProfileResult|outcome\.(paths|finalized)|\.result' \
  src/PlayerProfileManager.cpp; then
  echo 'obsolete build-profile outcome state remains' >&2
  exit 1
fi
git diff --check
```

Expected: one non-overwrite branch delegates to the helper, create/duplicate
reach the helper through `buildProfile`, and the negated search finds no
`BuildProfileResult`, old outcome fields, or discarded `.result` projection.

```sh
git add src/PlayerProfileManager.cpp
git commit -m "refactor: share profile directory finalization"
```

---

## Task 3: Make deletion results follow its logical commit point

**Files:**

- Modify: `tests/player_profile_manager_tests.cpp:616-668`
- Modify: `src/PlayerProfileManager.cpp:1524-1584`

- [ ] **Step 1: Change the post-commit cleanup expectation and confirm RED**

Update `testPartialTombstoneCleanupNeverRestoresOrExposesProfile` so the first
delete result must be successful with a nonempty warning:

```cpp
const auto deleted = manager.deleteProfile(deletedId);
expect(deleted.ok() && !deleted.message.empty(),
       "post-commit tombstone cleanup failure reports success warning");
```

Keep its assertions that the canonical source is absent, the partial tombstone
is hidden, and a later clean initialization removes it.

Run:

```sh
cmake --build cmake-build-debug --target player_profile_manager_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_profile_manager$' --output-on-failure
```

Expected: RED because current delete returns `IoFailure` after logical commit.

- [ ] **Step 2: Add a pre-commit sync rollback regression**

Add a fixture that fails the first `profiles/` sync after
`source -> .deleting-<uuid>` exactly once. Assert delete fails, the valid
canonical source is restored, no visible catalog change occurs, and clean
reinitialization preserves it.

Run the same focused test and confirm this new assertion is also RED against
the current no-rollback implementation.

- [ ] **Step 3: Add the complete deletion fault matrix before production changes**

Cover:

| Point | Injection | Expected outcome |
|---|---|---|
| 0 | source rename fails before moving | failure, valid source retained |
| 1 | source rename moves then reports failure | success, no valid source |
| 2 | commit sync fails; rollback succeeds | failure, valid source restored |
| 3 | rollback unavailable; retry sync succeeds | success, no valid source |
| 4 | rollback moves then reports failure | failure, valid source restored |
| 5 | post-commit tombstone removal fails | success warning, tombstone retained |
| 6 | post-commit cleanup partially mutates tombstone | success warning, partial tombstone retained |
| 7 | final cleanup sync fails | success warning, no valid source |
| 8 | rollback unavailable; retry commit sync fails | success warning, no valid source |

Add separate cases for:

- stale-tombstone removal failure before mutation;
- stale-tombstone cleanup-sync failure before mutation;
- rollback-parent sync failure after a restored source;
- both valid source and tombstone after ambiguous rename (failure);
- source absent with invalid tombstone (success warning, evidence preserved);
- physically present but deeply invalid/unreadable canonical source (failure,
  no cleanup);
- both source and tombstone absent (success warning);
- unsafe symlink/path inspection (failure without external mutation).

For every safely classified valid-source or absent-source case assert:

```cpp
std::error_code sourceError;
const bool sourceExists = std::filesystem::exists(
    manager.pathsFor(deletedId).root, sourceError);
expect(!sourceError && result.ok() == !sourceExists,
       "delete result matches physical canonical source presence");
const auto catalog = manager.listProfiles();
const bool listed = std::ranges::find(
                        catalog, deletedId, &PlayerProfile::id) !=
                    catalog.end();
expect(result.ok() == !listed,
       "delete result matches catalog membership");
```

For the present-but-unverifiable exception, assert the canonical source path
still physically exists, strict validation fails, the result fails, and no
cleanup callback touched that source. Do not apply the generic relation above
to this explicit fail-closed exception.

Assert clean manager reinitialization preserves the outcome and removes
recoverable tombstones. Do not claim power-loss durability for repeated sync
failure points.

Run the focused manager test and record the failing matrix/classification
assertions. For any case that passes without exercising its intended branch,
use a temporary targeted production-code mutation to demonstrate the test
fails for the specific wrong source-presence or warning outcome, then restore
it. No deletion branch may be implemented before its RED or
controlled-mutation evidence.

- [ ] **Step 4: Extract `finalizeProfileDeletion`**

Add an anonymous-namespace helper:

```cpp
ProfileResult finalizeProfileDeletion(
    const std::filesystem::path &applicationRoot,
    const PlayerProfileManagerDependencies &dependencies,
    const PlayerProfilePaths &source,
    const std::filesystem::path &tombstone,
    PlayerProfile profile);
```

Move every filesystem mutation out of `deleteProfile` into this helper.
Eligibility/last-profile/active-profile checks remain in `deleteProfile`.

Implement the design state machine exactly:

- Remove and sync any stale tombstone before touching source. Either failure
  returns `IoFailure` with the valid source untouched.
- Reconcile `source -> tombstone` even if rename reports false.
- A safely inspected deeply valid canonical source means deletion failed.
- A physically absent canonical source means deletion is the exclusive visible
  outcome. A missing/invalid tombstone is preserved and returns success with a
  warning.
- A physically present source that does not deeply validate fails closed and
  is preserved; validation failure must not authorize destructive cleanup.
- The first successful `profiles/` sync after source-to-tombstone is the normal
  commit point.
- If that sync fails, attempt tombstone-to-source rollback and sync it. A valid
  restored source returns failure. If the source is physically absent, retry
  commit sync and return success with warnings for unresolved sync/cleanup.
- After normal commit, tombstone removal and final sync are cleanup only;
  failures return success with warnings and leave startup-safe artifacts.
- `IoFailure` during containment/path inspection is unsafe and fails closed
  without cleanup.

`deleteProfile` should end with:

```cpp
return finalizeProfileDeletion(
    applicationDataRoot_, dependencies_, pathsFor(id), tombstone,
    *validated.profile);
```

- [ ] **Step 5: Confirm every deletion regression is GREEN**

Run the focused manager test. Expected: partial cleanup reports success warning;
the one-shot pre-commit sync failure restores the canonical source and reports
failure; every matrix/classification case matches physical canonical-source
presence or the explicit present-but-unverifiable fail-closed exception.

- [ ] **Step 6: Run focused manager/archive tests and commit**

```sh
cmake --build cmake-build-debug \
  --target player_profile_manager_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_archive)$'
git diff --check
git add src/PlayerProfileManager.cpp tests/player_profile_manager_tests.cpp
git commit -m "fix: report committed profile deletions"
```

---

## Task 4: Verify and review the profile transaction remediation

**Files:**

- Verify only: changes since the commit that added this implementation plan

- [ ] **Step 1: Build the affected test executables**

```sh
cmake --build cmake-build-debug \
  --target player_profile_manager_tests profile_archive_tests -j 6
```

Expected: both executables build successfully.

- [ ] **Step 2: Run focused profile tests**

```sh
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_archive)$'
```

Expected: both focused CTest entries pass.

- [ ] **Step 3: Build the desktop application**

```sh
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` builds successfully.

- [ ] **Step 4: Run the complete desktop suite**

```sh
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: all configured tests pass.

- [ ] **Step 5: Audit scope and whitespace**

```sh
PROFILE_TRANSACTION_PLAN_BASE=$(git log -1 --format=%H -- \
  docs/superpowers/plans/2026-07-13-profile-directory-transaction-outcomes.md)
git diff --check "$PROFILE_TRANSACTION_PLAN_BASE"..HEAD
git diff --stat "$PROFILE_TRANSACTION_PLAN_BASE"..HEAD
git status --short --branch
```

Expected: implementation changes are limited to
`src/PlayerProfileManager.cpp` and `tests/player_profile_manager_tests.cpp`;
the worktree is clean after commits.

- [ ] **Step 6: Request independent reviews**

One reviewer must check every design acceptance criterion and point in both
fault matrices. A separate quality reviewer must inspect filesystem ambiguity,
commit-point semantics, symlink safety, cleanup behavior, migration/overwrite
isolation, and test determinism. Fix all Critical/Important findings, rerun the
affected tests, and re-review before reporting this remediation complete.
