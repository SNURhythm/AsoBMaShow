# Profile Validation Use Policy Implementation Plan

> **For Codex:** Execute this plan with the subagent-driven-development and
> test-driven-development workflows. Do not broaden public runtime validation or
> move schema migration out of the database helpers.

**Goal:** Replace scattered profile validation flag pairs with one semantic
`ProfileUse` policy and make supported older-schema catalog profiles safely
manageable.

**Architecture:** Define an opaque namespace-scope `ProfileUse` for manager
internals and one exhaustive `validationPolicy(ProfileUse)` mapper. Route both
canonical manager validation and path-based transaction validation through it.
Use `Manage` for rename/delete/duplicate/overwrite and deletion reconciliation,
while preserving `Activate` before schema-owner binding and `RuntimeReady` for
commit/export/new destinations.

**Tech stack:** C++23, `std::filesystem`, SQLite, Bash/ripgrep source audit,
CMake/Ninja, CTest.

**Design:**
`docs/superpowers/specs/2026-07-13-profile-validation-use-policy-design.md`

**Execution prerequisite:** Commit this approved plan before changing production
or test code. Task 3 derives its scope baseline from that plan commit.

## Invariants

- Catalog rows use routine validation and may include supported older database
  schemas; every action revalidates at its semantic depth.
- Manage and Activate are deep, read-only, and allow supported older schemas.
- RuntimeReady is deep and current-only.
- Every profile/settings/input future version and every future database version
  fails closed for every use.
- Validation never migrates or mutates databases. Only the score/replay database
  owners migrate them.
- Delete target validation, last-profile counting, tombstone validation, and
  rollback source validation all use Manage.
- Newly built/imported destinations remain RuntimeReady; overwrite recovery uses
  Manage because the retained original may be supported older.

---

## Task 1: Add manager RED regressions and centralize the policy

**Files:**

- Modify: `src/PlayerProfileManager.h`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Create: `scripts/check_profile_validation_policy.sh`

### Step 1: Establish the manager baseline

```sh
cmake --build cmake-build-debug --target player_profile_manager_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_profile_manager$' --output-on-failure
```

Expected: 1/1 passes before new tests.

### Step 2: Add supported-older fixture helpers

In `tests/player_profile_manager_tests.cpp`, reuse `setDatabaseVersion` and add a
small helper that writes supported score/replay versions 5/3. Add a marker table
and row to both databases so every operation can prove validation and metadata
management remain non-mutating.

Add a helper that lists hidden `.staging-*`, `.deleting-*`, and `.backup-*`
profile transaction artifacts for no-artifact assertions.

### Step 3: Add the inactive supported-older management regression and prove RED

Add `testSupportedOlderInactiveProfileUsesManagePolicy`:

1. Initialize a current active profile and create one inactive target.
2. Seed marker rows and set target score/replay versions to 5/3.
3. Assert public catalog and activation preflight admit it while public strict
   validation and direct `commitActiveProfile` reject it without changing the
   bootstrap.
4. Rename it and assert the new metadata is cataloged while versions and marker
   rows remain unchanged.
5. Duplicate it and assert the source remains 5/3, the copy is current, and both
   marker rows survive.
6. Delete the current copy to return to exactly two profiles.
7. Delete the older inactive target and require success with an empty warning,
   no canonical target, no `.deleting-*` artifact, and only the active profile in
   the catalog.

Run the manager test before production changes. Expected RED:

- rename fails because it uses current-only validation;
- older deletion fails before mutation;
- after any temporary partial fix, the test separately detects the current-only
  last-profile count and current-only tombstone reconciliation.

Do not weaken the empty-warning/no-tombstone assertions; they distinguish a
proper Manage commit from misclassifying the valid older tombstone as corrupt.

### Step 4: Add the older rollback source-validation regression and prove RED

Add `testSupportedOlderDeleteRollbackRestoresManageableSource` with injected
filesystem dependencies:

- allow `source -> tombstone` to move;
- fail the first `profiles/` commit sync exactly once;
- allow `tombstone -> source` rollback and its sync.

Arm injection only after initialization, target creation, marker seeding, and
version changes. The first injected `profiles/` sync is therefore
deterministically the deletion commit sync, not fixture setup.

Require `IoFailure`, a valid/cataloged restored source, no tombstone, versions
5/3, marker rows intact, and the same restored filesystem/catalog state after
clean manager reinitialization. Assert each injected rename/sync branch was
reached.

Expected RED against the old code: target validation blocks before the injected
transaction. A partial top-level-only fix must also remain RED because the
finalizer would classify the older tombstone/source with RuntimeReady instead
of Manage.

### Step 5: Add the database-future fail-closed characterization

Add `testFutureDatabaseProfileIsNeverManageable` and loop it once for the score
database and once for the replay database:

- create an inactive profile;
- set one database `user_version` to current-plus-one;
- snapshot metadata/bootstrap and transaction-artifact state;
- assert it is omitted from the catalog and `renameProfile`, `duplicateProfile`,
  `deleteProfile`, `validateProfileForActivation`, `validateProfile`, and
  `commitActiveProfile` each return `FutureVersion`;
- assert the profile path/version, metadata/bootstrap, and artifact state are
  unchanged.

This test should already pass and protects the newly centralized permissive
uses from admitting future schemas.

### Step 6: Implement the single semantic policy

In `PlayerProfileManager.h`, replace the low-level private enums/overloads with:

```cpp
enum class ProfileUse : unsigned char;

// private members
std::vector<PlayerProfile> listProfiles(ProfileUse use) const;
ProfileResult validateProfile(std::string_view id, ProfileUse use) const;
```

Define the enum in `PlayerProfileManager.cpp` before the anonymous namespace:

```cpp
enum class ProfileUse : unsigned char {
  Catalog,
  Manage,
  Activate,
  RuntimeReady,
};
```

Add an exhaustive no-default mapper with a safe strict fallback:

```cpp
struct ProfileValidationPolicy {
  bool deep;
  bool allowSupportedOlderDatabases;
};

constexpr ProfileValidationPolicy validationPolicy(ProfileUse use) {
  switch (use) {
  case ProfileUse::Catalog:
    return {.deep = false, .allowSupportedOlderDatabases = true};
  case ProfileUse::Manage:
  case ProfileUse::Activate:
    return {.deep = true, .allowSupportedOlderDatabases = true};
  case ProfileUse::RuntimeReady:
    return {.deep = true, .allowSupportedOlderDatabases = false};
  }
  return {.deep = true, .allowSupportedOlderDatabases = false};
}
```

Change `validateProfileFiles` to accept `ProfileUse`, consult the same mapper,
and remove its raw `allowSupportedOlderDatabases` argument.

Route every call site exactly as follows:

| Call site | Use |
|---|---|
| primary bootstrap admission, public catalog | Catalog |
| backup/orphan/future scan startup recovery | Activate |
| duplicate, rename, delete target/count/reconciliation | Manage |
| overwrite target/count and overwrite backup recovery | Manage |
| activation preflight | Activate |
| public strict validation and active commit | RuntimeReady |
| new-profile final destination and imported staging | RuntimeReady |

Preserve migration/overwrite transaction algorithms; change only their
validation purpose arguments.

### Step 7: Add a deterministic semantic call-site audit

Create executable `scripts/check_profile_validation_policy.sh`. It must inspect
only `src/PlayerProfileManager.h/.cpp` and fail unless all of the following hold:

- obsolete `ValidationDepth` and `DatabaseVersionPolicy` names are absent;
- no `validateProfileFiles` call passes a raw boolean;
- `ProfileValidationPolicy` is constructed only inside `validationPolicy`;
- low-level `deep`/`allowSupportedOlderDatabases` choices occur only in the
  policy struct/mapper and are consumed only by canonical/path validators;
- exact `ProfileUse` enumerator counts in `PlayerProfileManager.cpp`, including
  one mapper case each, are Catalog=3, Manage=13, Activate=5,
  RuntimeReady=4;
- the counted occurrences match this explicit allowlist:
  - Catalog: mapper, primary bootstrap admission, public catalog;
  - Activate: mapper, bootstrap-backup recovery, orphan list, future-profile
    scan, public activation preflight;
  - Manage: mapper, duplicate, rename, delete target, delete count, overwrite
    target, overwrite count, deletion source and tombstone, three startup
    overwrite-recovery validations, and live overwrite rollback backup;
  - RuntimeReady: mapper, new-profile destination, public strict validation,
    imported staging.

The script may use multiline `rg`/Perl patterns for the named contexts, but a
count alone is insufficient: every allowlisted role must have a positive exact
match, and any extra occurrence must fail.

Demonstrate the audit itself works with temporary source mutations: change the
delete target from Manage to RuntimeReady, introduce a raw boolean path
validation, and directly construct a policy outside the mapper. Each mutation
must make the script fail; restore all mutations before continuing.

### Step 8: Make the manager tests GREEN and commit

```sh
cmake --build cmake-build-debug --target player_profile_manager_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_profile_manager$' --output-on-failure
scripts/check_profile_validation_policy.sh
git diff --check
```

Expected: manager passes; raw split-policy selectors are gone; only semantic
uses and the single mapper remain.

Commit:

```sh
git add src/PlayerProfileManager.h src/PlayerProfileManager.cpp \
  tests/player_profile_manager_tests.cpp \
  scripts/check_profile_validation_policy.sh
git commit -m "fix: unify profile validation policy"
```

---

## Task 2: Characterize activation and archive boundary policies

**Files:**

- Modify: `tests/profile_switch_tests.cpp`
- Modify: `tests/profile_archive_tests.cpp`
- Production changes are forbidden in this task. If a characterization exposes
  a defect, stop and create a separate test-first production follow-up before
  committing Task 2 tests.

### Step 1: Establish focused baselines

```sh
cmake --build cmake-build-debug \
  --target profile_switch_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_switch|foundation_profile_archive)$'
```

Expected: 2/2 passes.

### Step 2: Add a supported-older switch characterization

Using `SwitchFixture`, set the inactive second profile score/replay versions to
5/3 and add marker rows after fixture construction. Assert:

- activation preflight succeeds and strict validation fails before switching;
- `coordinator.switchTo` succeeds;
- score/replay binders migrate both databases to their current versions;
- the manager commits the second profile active;
- existing scores/replays and marker rows survive;
- strict validation succeeds after the switch.

This proves Activate remains supported-older while RuntimeReady remains strict
until database-owner binding completes.

### Step 3: Add archive boundary characterizations

Extend archive tests with three cases:

1. Downgrade an otherwise valid source to 5/3 and prove Export rejects it before
   writing an archive because export is RuntimeReady.
2. Downgrade an inactive overwrite target to 5/3, import a current archive over
   it, and prove overwrite succeeds, installs a current valid profile, and
   leaves no transaction artifacts because target management uses Manage.
3. Set an inactive overwrite target database to current-plus-one and prove
   import returns `FutureVersion`, preserves the target/version/metadata, and
   creates no transaction artifacts.

### Step 4: Verify focused boundaries and commit

```sh
cmake --build cmake-build-debug \
  --target player_profile_manager_tests profile_switch_tests \
           profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_switch|foundation_profile_archive)$'
scripts/check_profile_validation_policy.sh
git diff --check
```

Expected: 3/3 passes and the semantic source audit passes. No production file
changed in Task 2.

```sh
git add tests/profile_switch_tests.cpp tests/profile_archive_tests.cpp
git commit -m "test: cover profile validation boundaries"
```

---

## Task 3: Verify and independently review the policy remediation

### Step 1: Build affected targets and desktop application

```sh
cmake --build cmake-build-debug \
  --target player_profile_manager_tests profile_switch_tests \
           profile_archive_tests main -j 6
```

Expected: all targets build.

### Step 2: Run focused and complete tests

```sh
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_switch|foundation_profile_archive)$'
ctest --test-dir cmake-build-debug --output-on-failure
scripts/check_profile_validation_policy.sh
```

Expected: focused 3/3 and every configured desktop test pass.

### Step 3: Audit scope and source of truth

```sh
PROFILE_USE_PLAN_BASE=$(git log -1 --format=%H -- \
  docs/superpowers/plans/2026-07-13-profile-validation-use-policy.md)
git diff --check "$PROFILE_USE_PLAN_BASE"..HEAD
git diff --stat "$PROFILE_USE_PLAN_BASE"..HEAD
git diff --name-only "$PROFILE_USE_PLAN_BASE"..HEAD
git status --short --branch
```

Expected implementation scope:

- `src/PlayerProfileManager.h`
- `src/PlayerProfileManager.cpp`
- `tests/player_profile_manager_tests.cpp`
- `tests/profile_switch_tests.cpp`
- `tests/profile_archive_tests.cpp`
- `scripts/check_profile_validation_policy.sh`

### Step 4: Request two independent reviews

One reviewer checks every `ProfileUse` call-site assignment and all design
acceptance criteria. A separate quality reviewer checks future-version
fail-closed behavior, non-mutating Manage/Activate semantics, deletion
source/tombstone reconciliation, migration ownership, opaque-enum feasibility,
and deterministic test coverage. Fix every Critical/Important finding, rerun
affected tests, and re-review before reporting this remediation complete.
